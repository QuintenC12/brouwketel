/*
 * net.c - wifi en MQTT voor Niels ESP (zie net.h)
 *
 * Alles wat de net-taak, de wifi-events en de MQTT-events delen, staat
 * achter een mutex ("slot"). Functies van esp-mqtt worden nooit aangeroepen
 * terwijl het slot vast is: dat kan vastlopen met de eigen MQTT-taak.
 *
 * Recept: zodra de verbinding er is, publiceert Niels ESP iets op
 * brouwketel/recept/vraag. De server stuurt dan het recept op
 * brouwketel/recept, en dat gaat als "recept {...}" naar het scherm.
 */
#include "net.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "niels_uart.h"
#include "scherm_uart.h"

static const char *TAG = "net";

#define NET_STATUS_MS   5000    // zo vaak net/status herhalen
#define VERBIND_MAX_MS  20000   // zo lang mag wifi + MQTT verbinden duren
#define MAX_BEWAARD     10      // meldingen bewaren als de server weg is

#define MQTT_RECEPT       MQTT_VOORVOEGSEL "recept"
#define MQTT_RECEPT_VRAAG MQTT_VOORVOEGSEL "recept/vraag"

typedef enum { NET_UIT, NET_BEZIG, NET_VERBONDEN, NET_FOUT } net_toestand_t;

static SemaphoreHandle_t slot;
#define VERGRENDEL()   xSemaphoreTake(slot, portMAX_DELAY)
#define ONTGRENDEL()   xSemaphoreGive(slot)

// mqtt wordt alleen in net_start() gemaakt en daarna nooit meer veranderd
static esp_mqtt_client_handle_t mqtt = NULL;

// Achter het slot
static net_toestand_t net = NET_UIT;
static bool     net_gewenst = false;
static uint32_t verbind_start = 0;
static char    *bewaard[MAX_BEWAARD];
static int      aantal_bewaard = 0;

static uint32_t nu_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ---------------- naar het scherm ---------------- */

static void stuur_net_status(net_toestand_t toestand, const char *reden)
{
    static const char *namen[] = { "uit", "bezig", "verbonden", "fout" };
    char json[96];
    if (reden)
        snprintf(json, sizeof json, "{\"status\":\"%s\",\"reden\":\"%s\"}", namen[toestand], reden);
    else
        snprintf(json, sizeof json, "{\"status\":\"%s\"}", namen[toestand]);
    scherm_uart_verstuur(OND_NET_STATUS, json);
}

/* Aanroepen met het slot vast */
static void zet_net(net_toestand_t nieuw, const char *reden)
{
    if (nieuw == net) return;
    net = nieuw;
    ESP_LOGI(TAG, "toestand %d %s", net, reden ? reden : "");
    stuur_net_status(net, reden);
}

/* ---------------- naar MQTT ---------------- */

void net_publiceer(const char *onderwerp, const char *json)
{
    char volledig[64];
    snprintf(volledig, sizeof volledig, MQTT_VOORVOEGSEL "%s", onderwerp);
    bool is_melding = strcmp(onderwerp, OND_MELD) == 0;

    VERGRENDEL();
    bool verbonden = net == NET_VERBONDEN;
    if (!verbonden && is_melding) {
        if (aantal_bewaard == MAX_BEWAARD) {        // vol: oudste laten vallen
            free(bewaard[0]);
            memmove(&bewaard[0], &bewaard[1], sizeof(char *) * (MAX_BEWAARD - 1));
            aantal_bewaard--;
        }
        bewaard[aantal_bewaard++] = strdup(json);
    }
    ONTGRENDEL();
    if (verbonden) esp_mqtt_client_publish(mqtt, volledig, json, 0, is_melding ? 1 : 0, 0);
}

/* ---------------- MQTT-events ---------------- */

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        // bewaarde meldingen overnemen, en pas na het slot versturen
        char *te_sturen[MAX_BEWAARD];
        VERGRENDEL();
        int n = aantal_bewaard;
        memcpy(te_sturen, bewaard, sizeof(char *) * n);
        aantal_bewaard = 0;
        zet_net(NET_VERBONDEN, NULL);
        ONTGRENDEL();

        esp_mqtt_client_subscribe(ev->client, MQTT_RECEPT, 1);
        for (int i = 0; i < n; i++) {
            esp_mqtt_client_publish(ev->client, MQTT_VOORVOEGSEL OND_MELD, te_sturen[i], 0, 1, 0);
            free(te_sturen[i]);
        }
        esp_mqtt_client_publish(ev->client, MQTT_RECEPT_VRAAG, "{}", 0, 1, 0);
        break;
    }

    case MQTT_EVENT_DISCONNECTED:
        VERGRENDEL();
        if (net_gewenst && net == NET_VERBONDEN) {   // de client verbindt zelf opnieuw
            verbind_start = nu_ms();
            zet_net(NET_BEZIG, "verbinding verloren");
        }
        ONTGRENDEL();
        break;

    case MQTT_EVENT_DATA: {
        // Recept van de server: op één regel zetten en naar het scherm.
        if (ev->topic_len != (int)strlen(MQTT_RECEPT) ||
            strncmp(ev->topic, MQTT_RECEPT, ev->topic_len) != 0) break;
        if (ev->current_data_offset != 0 || ev->data_len != ev->total_data_len) {
            ESP_LOGW(TAG, "recept te groot voor de MQTT-buffer");
            break;
        }
        cJSON *recept = cJSON_ParseWithLength(ev->data, ev->data_len);
        char *regel = recept ? cJSON_PrintUnformatted(recept) : NULL;
        cJSON_Delete(recept);
        if (!regel) { ESP_LOGW(TAG, "recept is geen geldige JSON"); break; }
        // Het scherm aanvaardt regels tot NIELS_MAX_REGEL tekens.
        esp_err_t err = scherm_uart_verstuur(OND_RECEPT, regel);
        if (err != ESP_OK) ESP_LOGW(TAG, "recept niet naar het scherm: %s", esp_err_to_name(err));
        free(regel);
        break;
    }

    default:
        break;
    }
}

/* ---------------- wifi ---------------- */

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        VERGRENDEL();
        bool opnieuw = net_gewenst;
        if (net_gewenst && net == NET_VERBONDEN) {
            verbind_start = nu_ms();
            zet_net(NET_BEZIG, "wifi weg");
        }
        ONTGRENDEL();
        if (opnieuw) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "wifi verbonden");     // de MQTT-client verbindt nu vanzelf
    }
}

static void wifi_starten(void)
{
    esp_err_t err = nvs_flash_init();         // nodig voor wifi
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, CONFIG_NIELS_WIFI_NAAM, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, CONFIG_NIELS_WIFI_WACHTWOORD, sizeof wc.sta.password);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ---------------- verbinden en stoppen ---------------- */

/* Er is maar één MQTT-client, die nooit wordt opgeruimd. Bij een nieuwe
 * net/start krijgt hij alleen een ander adres. Zo kan een andere taak
 * nooit publiceren met een client die net verdwenen is. */
static void client_stoppen(void)
{
    if (mqtt) esp_mqtt_client_stop(mqtt);     // al gestopt? dan doet dit niets
}

void net_start(const char *server, int poort)
{
    char uri[80];
    snprintf(uri, sizeof uri, "mqtt://%s:%d", server, poort);
    ESP_LOGI(TAG, "verbinden met %s", uri);

    client_stoppen();
    VERGRENDEL();
    net_gewenst = true;
    verbind_start = nu_ms();
    net = NET_UIT;                            // zodat "bezig" zeker verstuurd wordt
    zet_net(NET_BEZIG, NULL);
    ONTGRENDEL();

    if (!mqtt) {
        esp_mqtt_client_config_t cfg = {
            .broker.address.uri = uri,
            .credentials.client_id = "niels-esp",
            .buffer.size = 2048,              // het recept is een lang bericht
        };
        mqtt = esp_mqtt_client_init(&cfg);
        if (mqtt) esp_mqtt_client_register_event(mqtt, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    } else if (esp_mqtt_client_set_uri(mqtt, uri) != ESP_OK) {
        ESP_LOGW(TAG, "adres niet aanvaard: %s", uri);
    }

    if (!mqtt || esp_mqtt_client_start(mqtt) != ESP_OK) {
        VERGRENDEL();
        net_gewenst = false;
        zet_net(NET_FOUT, "MQTT-client start niet");
        ONTGRENDEL();
        return;
    }
    esp_wifi_connect();                       // al verbonden? dan doet dit niets
}

void net_stop(void)
{
    VERGRENDEL();
    net_gewenst = false;
    zet_net(NET_UIT, NULL);
    ONTGRENDEL();
    client_stoppen();
}

/* Geen verbinding binnen VERBIND_MAX_MS: fout melden en stoppen.
 * Daarnaast elke NET_STATUS_MS de toestand herhalen voor het scherm. */
static void net_taak(void *parameters)
{
    uint32_t vorige_status = nu_ms();
    while (1) {
        bool stoppen = false;
        VERGRENDEL();
        if (net_gewenst && net == NET_BEZIG && nu_ms() - verbind_start > VERBIND_MAX_MS) {
            wifi_ap_record_t ap;
            bool wifi_ok = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
            stoppen = true;
            net_gewenst = false;
            zet_net(NET_FOUT, wifi_ok ? "server onbereikbaar" : "wifi niet gevonden");
        }
        net_toestand_t nu = net;
        ONTGRENDEL();
        if (stoppen) client_stoppen();

        if (nu_ms() - vorige_status >= NET_STATUS_MS) {
            vorige_status = nu_ms();
            stuur_net_status(nu, NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void net_init(void)
{
    slot = xSemaphoreCreateMutex();
    wifi_starten();
    stuur_net_status(NET_UIT, NULL);          // het scherm ziet zo dat we (weer) leven
    xTaskCreate(net_taak, "net", 4096, NULL, 4, NULL);
}
