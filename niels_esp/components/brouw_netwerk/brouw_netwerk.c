#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/event_groups.h"
#include "netwerk_priv.h"
#include "nvs_flash.h"
#include "mdns.h"

#define EERSTE_IP_TIMEOUT_MS 25000
#define HERPROBEER_MS 2000
#define SLUIT_PORTAAL_NA_MS 750

enum {
    STATION_ACTIEF = BIT0,
    STATION_HEEFT_IP = BIT1,
    STATION_IP_EVENT = BIT2,
    STATION_VERBIND = BIT3,
    SETUP_ACTIEF = BIT4,
    NETWERK_FOUT = BIT5,
    SETUP_GEVRAAGD = BIT6,     // het scherm vraagt de configuratiemodus
};

static const char *TAG = "brouw_netwerk";
static EventGroupHandle_t s_events;
static esp_netif_t *s_station;
static esp_netif_t *s_ap;
static char s_setup_wachtwoord[64];
static bool s_setup_afdwingen;
static TaskHandle_t s_taak;          // netwerktaak
static volatile bool s_setup_op_vraag;   // setup staat open omdat het scherm erom vroeg

const char* dnsName = "Brouwketel-setup";

static void netwerk_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if ((xEventGroupGetBits(s_events) & STATION_ACTIEF) == 0) {
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(s_events, STATION_VERBIND);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "Wifi niet verbonden (reden %u).", (unsigned)event->reason);
        xEventGroupClearBits(s_events, STATION_HEEFT_IP);
        xEventGroupSetBits(s_events, STATION_VERBIND);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        if (event->esp_netif == s_station) {
            ESP_LOGI(TAG, "Wifi IPv4-adres: " IPSTR, IP2STR(&event->ip_info.ip));
            xEventGroupSetBits(s_events, STATION_HEEFT_IP | STATION_IP_EVENT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        xEventGroupClearBits(s_events, STATION_HEEFT_IP);
    }
}

static esp_err_t station_start(const brouw_instellingen_t *instellingen)
{
    wifi_config_t wifi = {0};
    // De driver heeft 32 bytes voor het SSID. Een SSID van 32 bytes past exact.
    memcpy(wifi.sta.ssid, instellingen->ssid, strlen(instellingen->ssid));
    memcpy(wifi.sta.password, instellingen->wifi_wachtwoord, strlen(instellingen->wifi_wachtwoord));
    wifi.sta.threshold.authmode = wifi.sta.password[0] == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Instellen stationmodus");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi), TAG, "Instellen stationconfiguratie");
    xEventGroupClearBits(s_events, STATION_HEEFT_IP | STATION_IP_EVENT | STATION_VERBIND);
    xEventGroupSetBits(s_events, STATION_ACTIEF);
    return esp_wifi_start();
}

static esp_err_t station_onderhouden(void)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(EERSTE_IP_TIMEOUT_MS);
    bool eerste_ip_gekregen = false;

    for (;;) {
        TickType_t wachttijd = portMAX_DELAY;
        if (!eerste_ip_gekregen) {
            const TickType_t verstreken = xTaskGetTickCount() - start;
            wachttijd = verstreken < timeout ? timeout - verstreken : 0;
        }

        const EventBits_t events = xEventGroupWaitBits(s_events, STATION_IP_EVENT | STATION_VERBIND | SETUP_GEVRAAGD,
                                                      pdTRUE, pdFALSE, wachttijd);
        if ((events & SETUP_GEVRAAGD) != 0) {
            return ESP_ERR_NOT_FINISHED;    // netwerktaak opent de setup
        }
        if ((events & STATION_IP_EVENT) != 0) {
            eerste_ip_gekregen = true;
        }
        if (!eerste_ip_gekregen && wachttijd == 0) {
            return ESP_ERR_TIMEOUT;
        }
        if ((events & STATION_VERBIND) != 0 && brouw_netwerk_status() != BROUW_NETWERK_IP_GEREED) {
            const esp_err_t fout = esp_wifi_connect();
            if (fout != ESP_OK) {
                ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(fout));
                xEventGroupSetBits(s_events, STATION_VERBIND);
            }
            vTaskDelay(pdMS_TO_TICKS(HERPROBEER_MS));
        }
    }
}

static esp_err_t setup_wachten(void)
{
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_AP, mac), TAG, "Lezen MAC-adres");

    wifi_config_t wifi = {0};
    wifi.ap.ssid_len = snprintf((char *)wifi.ap.ssid, sizeof(wifi.ap.ssid),
                                "Brouwketel-Setup-%02X%02X%02X",
                                (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);
    memcpy(wifi.ap.password, s_setup_wachtwoord, strlen(s_setup_wachtwoord));
    wifi.ap.channel = 1;
    wifi.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wifi.ap.max_connection = 2;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "Instellen accesspointmodus");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi), TAG, "Instellen accesspoint");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Starten accesspoint");

    // Een oude "sluiten"-vraag mag deze setup niet meteen weer dichtdoen.
    ulTaskNotifyTake(pdTRUE, 0);

    httpd_handle_t server;
    const esp_err_t fout = brouw_portaal_start(xTaskGetCurrentTaskHandle(), &server);
    if (fout != ESP_OK) {
        esp_wifi_stop();
        return fout;
    }
    esp_err_t mdns_fout = mdns_init();
    const bool mdns_gestart = (mdns_fout == ESP_OK);

    if (mdns_gestart) 
    {
        mdns_fout = mdns_hostname_set(dnsName);
    }
    if (mdns_fout != ESP_OK) 
    {
        ESP_LOGW(TAG, "mDNS: %s", esp_err_to_name(mdns_fout));
    }
    xEventGroupSetBits(s_events, SETUP_ACTIEF);
    ESP_LOGI(TAG, "Verbind met %s en open http://192.168.4.1/", (const char *)wifi.ap.ssid);

    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // Laat de HTTP-respons vertrekken voordat het wifi-netwerk verdwijnt.
    vTaskDelay(pdMS_TO_TICKS(SLUIT_PORTAAL_NA_MS));

    if (mdns_gestart) 
    {
        mdns_free();
    }

    const esp_err_t portaal_fout = httpd_stop(server);
    const esp_err_t stop_fout = esp_wifi_stop();
    xEventGroupClearBits(s_events, SETUP_ACTIEF);
    s_setup_op_vraag = false;
    return portaal_fout != ESP_OK ? portaal_fout : stop_fout;
}

static void netwerktaak(void *arg)
{
    bool open_setup = s_setup_afdwingen;
    esp_err_t fout;

    for (;;) {
        if (open_setup) {
            fout = setup_wachten();
            if (fout != ESP_OK) {
                break;
            }
            open_setup = false;
        }

        brouw_instellingen_t instellingen;
        fout = brouw_instellingen_lees(&instellingen);
        if (fout == ESP_ERR_NVS_NOT_FOUND || fout == ESP_ERR_INVALID_SIZE ||
            fout == ESP_ERR_INVALID_VERSION || fout == ESP_ERR_INVALID_ARG) {
            open_setup = true;
            continue;
        }
        if (fout != ESP_OK) {
            break;
        }

        if ((xEventGroupGetBits(s_events) & SETUP_GEVRAAGD) != 0) {
            // Het scherm vroeg de configuratiemodus terwijl we nog niet verbonden waren.
            xEventGroupClearBits(s_events, SETUP_GEVRAAGD);
            open_setup = true;
            continue;
        }

        fout = station_start(&instellingen);
        if (fout == ESP_OK) {
            fout = station_onderhouden();
        }
        if (fout == ESP_ERR_NOT_FINISHED) {
            ESP_LOGI(TAG, "Configuratiemodus gevraagd. Setup gaat open.");
        } else {
            ESP_LOGW(TAG, "Station krijgt geen IP (%s). Setup gaat open.", esp_err_to_name(fout));
        }
        xEventGroupClearBits(s_events, STATION_ACTIEF | STATION_HEEFT_IP);
        fout = esp_wifi_stop();
        if (fout != ESP_OK) {
            break;
        }
        open_setup = true;
    }

    ESP_LOGE(TAG, "Netwerktaak gestopt: %s", esp_err_to_name(fout));
    xEventGroupSetBits(s_events, NETWERK_FOUT);
    vTaskDelete(NULL);
}

esp_err_t brouw_netwerk_start(const char* setup_wachtwoord, bool setup_afdwingen)
{
    if (setup_wachtwoord == NULL || !brouw_wpa_wachtwoord_geldig(setup_wachtwoord)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_events != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(nvs_flash_init(), TAG, "Initialiseren NVS");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "Initialiseren ESP-NETIF");
    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") != NULL ||
        esp_netif_get_handle_from_ifkey("WIFI_AP_DEF") != NULL) {
        ESP_LOGE(TAG, "Wifi-interface bestaat al. Laat één component wifi beheren.");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_init), TAG, "Initialiseren wifi-driver");

    esp_event_handler_instance_t wifi_handler = NULL;
    esp_event_handler_instance_t ip_handler = NULL;
    ESP_GOTO_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), fail, TAG, "Instellen wifi-opslag");
    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    s_station = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    if (s_station == NULL || s_ap == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    ESP_GOTO_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                      netwerk_event, NULL, &wifi_handler), fail, TAG, "Registreren wifi-events");
    ESP_GOTO_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                      netwerk_event, NULL, &ip_handler), fail, TAG, "Registreren IP-event");

    strcpy(s_setup_wachtwoord, setup_wachtwoord);
    s_setup_afdwingen = setup_afdwingen;
    if (xTaskCreate(netwerktaak, "brouw_netwerk", 4096, NULL, tskIDLE_PRIORITY + 2, &s_taak) == pdPASS) {
        return ESP_OK;
    }
    ret = ESP_ERR_NO_MEM;

fail:
    if (ip_handler != NULL) {
        esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, ip_handler);
    }
    if (wifi_handler != NULL) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler);
    }
    if (s_ap != NULL) {
        esp_netif_destroy_default_wifi(s_ap);
        s_ap = NULL;
    }
    if (s_station != NULL) {
        esp_netif_destroy_default_wifi(s_station);
        s_station = NULL;
    }
    if (s_events != NULL) {
        vEventGroupDelete(s_events);
        s_events = NULL;
    }
    esp_wifi_deinit();
    return ret;
}

brouw_netwerk_status_t brouw_netwerk_status(void)
{
    if (s_events == NULL) {
        return BROUW_NETWERK_UIT;
    }
    const EventBits_t bits = xEventGroupGetBits(s_events);
    if ((bits & NETWERK_FOUT) != 0) {
        return BROUW_NETWERK_FOUT;
    }
    if ((bits & SETUP_ACTIEF) != 0) {
        return BROUW_NETWERK_SETUP;
    }
    if ((bits & (STATION_ACTIEF | STATION_HEEFT_IP)) == (STATION_ACTIEF | STATION_HEEFT_IP)) {
        return BROUW_NETWERK_IP_GEREED;
    }
    return BROUW_NETWERK_WACHT_OP_IP;
}

void brouw_netwerk_setup(bool aan)
{
    if (s_events == NULL || s_taak == NULL) {
        return;
    }
    const EventBits_t bits = xEventGroupGetBits(s_events);
    if (aan) {
        if ((bits & SETUP_ACTIEF) != 0) {
            s_setup_op_vraag = true;        // stond al open, nu ook op vraag
            return;
        }
        s_setup_op_vraag = true;
        xEventGroupSetBits(s_events, SETUP_GEVRAAGD);
        return;
    }
    xEventGroupClearBits(s_events, SETUP_GEVRAAGD);
    // Alleen sluiten wat het scherm zelf opende. Staat de setup open omdat er
    // nog geen (werkende) wifi-instellingen zijn, dan blijft hij open.
    if ((bits & SETUP_ACTIEF) != 0 && s_setup_op_vraag) {
        s_setup_op_vraag = false;
        xTaskNotifyGive(s_taak);            // setup_wachten() gaat verder en sluit
    }
}

esp_err_t brouw_netwerk_lees_mqtt(brouw_netwerk_mqtt_t *mqtt)
{
    if (mqtt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    brouw_instellingen_t instellingen;
    const esp_err_t fout = brouw_instellingen_lees(&instellingen);
    if (fout == ESP_OK) {
        *mqtt = instellingen.mqtt;
    }
    return fout;
}
