/*
 * Niels ESP - ESP-IDF
 *
 * Doet het werk aan de ketel en praat met twee kanten:
 *   - het scherm (CrowPanel) via een seriële kabel, regels "onderwerp {json}"
 *   - de MQTT-server via wifi, zodat de webapp alles kan zien
 *
 * Aan de ketel: DS18B20 (temperatuur), SSR (verwarming), pomp, zoemer.
 * Het regelboekje in het project beschrijft alle berichten.
 *
 * Taken:
 *   app_main (hoofdlus)  kabel lezen, uitgangen, temperatuur en status sturen
 *   sensor_taak          DS18B20 uitlezen (een meting duurt ~800 ms)
 *   wifi/MQTT-events     draaien in de taken van ESP-IDF zelf
 *
 * Veiligheid: hoort Niels ESP NIELS_STIL_MAX_MS niets van het scherm
 * (geen brouw/stuur), dan gaan verwarming, pomp en zoemer uit.
 *
 * Wifi en pinnen: idf.py menuconfig -> "Niels ESP".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "onewire_bus.h"
#include "ds18b20.h"
#include "niels_uart.h"

static const char *TAG = "niels";

#define SCHERM_UART      UART_NUM_1
#define SSR_VENSTER_MS   2000   // vermogen 40 % = 800 ms aan, 1200 ms uit
#define TEMP_MS          1000
#define NET_STATUS_MS    5000
#define VERBIND_MAX_MS   20000  // zo lang mag wifi + MQTT verbinden duren

// Aanname: de server stuurt het recept op brouwketel/recept zodra
// Niels ESP iets publiceert op brouwketel/recept/vraag.
#define MQTT_RECEPT       MQTT_VOORVOEGSEL "recept"
#define MQTT_RECEPT_VRAAG MQTT_VOORVOEGSEL "recept/vraag"

static uint32_t nu_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* Alles wat de hoofdlus, de wifi-events en de MQTT-events delen,
 * staat achter deze mutex. Functies van esp-mqtt worden nooit aangeroepen
 * terwijl het slot vast is: dat kan vastlopen met de eigen MQTT-taak. */
static SemaphoreHandle_t slot;
#define VERGRENDEL()   xSemaphoreTake(slot, portMAX_DELAY)
#define ONTGRENDEL()   xSemaphoreGive(slot)

// Wat het scherm wil (alleen de hoofdlus)
static int      vermogen = 0;
static bool     pomp = false, zoemer = false;
static uint32_t laatste_stuur = 0;
static bool     ooit_stuur = false;

// Laatste meting (sensor_taak schrijft, hoofdlus leest)
static volatile float temp = 0;
static volatile bool  temp_ok = false;

// Verbinding met de server (achter slot)
typedef enum { NET_UIT, NET_BEZIG, NET_VERBONDEN, NET_FOUT } net_toestand_t;
static net_toestand_t net = NET_UIT;
static bool     net_gewenst = false;
static uint32_t verbind_start = 0;
static esp_mqtt_client_handle_t mqtt = NULL;

// Meldingen die nog naar MQTT moeten (als de server even weg was, achter slot)
#define MAX_BEWAARD 10
static char *bewaard[MAX_BEWAARD];
static int   aantal_bewaard = 0;

// Laatst behandelde meldnummers, om herhalingen te herkennen (alleen hoofdlus)
static int gezien[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
static int gezien_pos = 0;

/* ---------------- naar het scherm ---------------- */

/* Eén regel "onderwerp {json}\n" in één keer versturen, zodat regels uit
 * verschillende taken nooit door elkaar komen. Geeft obj vrij. */
static void naar_scherm(const char *onderwerp, cJSON *obj) {
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!json) return;
    size_t n = strlen(onderwerp) + strlen(json) + 3;
    char *regel = malloc(n);
    if (regel) {
        snprintf(regel, n, "%s %s\n", onderwerp, json);
        uart_write_bytes(SCHERM_UART, regel, strlen(regel));
        free(regel);
    }
    free(json);
}

static void stuur_ack_nr(int nr) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "nr", nr);
    naar_scherm(OND_ACK, o);
}

static void stuur_ack_voor(const char *voor) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "voor", voor);
    naar_scherm(OND_ACK, o);
}

static void stuur_net_status(net_toestand_t toestand, const char *reden) {
    static const char *namen[] = { "uit", "bezig", "verbonden", "fout" };
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", namen[toestand]);
    if (reden) cJSON_AddStringToObject(o, "reden", reden);
    naar_scherm(OND_NET_STATUS, o);
}

/* Aanroepen met slot vast */
static void zet_net(net_toestand_t nieuw, const char *reden) {
    if (nieuw == net) return;
    net = nieuw;
    ESP_LOGI(TAG, "net %d %s", net, reden ? reden : "");
    stuur_net_status(net, reden);
}

/* ---------------- naar MQTT ---------------- */

static void publiceer(const char *onderwerp, const char *json) {
    char volledig[64];
    snprintf(volledig, sizeof volledig, MQTT_VOORVOEGSEL "%s", onderwerp);
    bool is_melding = strcmp(onderwerp, OND_MELD) == 0;

    // mqtt wordt alleen in de hoofdlus aangepast, en publiceer() draait daar ook
    VERGRENDEL();
    bool verbonden = net == NET_VERBONDEN && mqtt;
    if (!verbonden && is_melding) {                // status/temp komt vanzelf opnieuw
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

/* ---------------- MQTT ---------------- */

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
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

    case MQTT_EVENT_DATA:
        // Recept van de server: compact maken (één regel) en naar het scherm.
        // Alleen berichten die in één stuk binnenkomen (buffer = 2048 bytes).
        if (ev->topic_len != (int)strlen(MQTT_RECEPT) ||
            strncmp(ev->topic, MQTT_RECEPT, ev->topic_len) != 0) break;
        if (ev->current_data_offset != 0 || ev->data_len != ev->total_data_len) {
            ESP_LOGW(TAG, "recept te groot");
            break;
        }
        cJSON *recept = cJSON_ParseWithLength(ev->data, ev->data_len);
        if (!recept) { ESP_LOGW(TAG, "recept is geen geldige JSON"); break; }
        naar_scherm(OND_RECEPT, recept);
        break;

    default:
        break;
    }
}

/* ---------------- wifi ---------------- */

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
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
        ESP_LOGI(TAG, "wifi verbonden");
        // MQTT-client verbindt vanzelf; zie mqtt_event
    }
}

static void wifi_starten(void) {
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

/* ---------------- verbinden met de server ---------------- */

static void net_starten(const char *server, int poort) {
    char uri[80];
    snprintf(uri, sizeof uri, "mqtt://%s:%d", server, poort);
    ESP_LOGI(TAG, "verbinden met %s", uri);

    VERGRENDEL();
    esp_mqtt_client_handle_t oud = mqtt;
    mqtt = NULL;
    net_gewenst = true;
    verbind_start = nu_ms();
    net = NET_UIT;                                // zodat "bezig" zeker verstuurd wordt
    zet_net(NET_BEZIG, NULL);
    ONTGRENDEL();
    if (oud) esp_mqtt_client_destroy(oud);        // vorige verbinding opruimen

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .credentials.client_id = "niels-esp",
        .buffer.size = 2048,                      // het recept is een lang bericht
    };
    esp_mqtt_client_handle_t nieuw = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(nieuw, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    VERGRENDEL();
    mqtt = nieuw;
    ONTGRENDEL();
    esp_mqtt_client_start(nieuw);

    esp_wifi_connect();                           // al verbonden? dan doet dit niets
}

static void net_stoppen(void) {
    VERGRENDEL();
    esp_mqtt_client_handle_t oud = mqtt;
    mqtt = NULL;
    net_gewenst = false;
    zet_net(NET_UIT, NULL);
    ONTGRENDEL();
    if (oud) esp_mqtt_client_destroy(oud);
}

/* Geen verbinding binnen VERBIND_MAX_MS? Dan fout en stoppen. */
static void net_bijhouden(void) {
    esp_mqtt_client_handle_t oud = NULL;
    VERGRENDEL();
    if (net_gewenst && net == NET_BEZIG && nu_ms() - verbind_start > VERBIND_MAX_MS) {
        wifi_ap_record_t ap;
        bool wifi_ok = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
        oud = mqtt;
        mqtt = NULL;
        net_gewenst = false;
        zet_net(NET_FOUT, wifi_ok ? "server onbereikbaar" : "wifi niet gevonden");
    }
    ONTGRENDEL();
    if (oud) esp_mqtt_client_destroy(oud);
}

/* ---------------- berichten van het scherm ---------------- */

static bool al_gezien(int nr) {
    for (int i = 0; i < 8; i++) if (gezien[i] == nr) return true;
    gezien[gezien_pos] = nr;
    gezien_pos = (gezien_pos + 1) % 8;
    return false;
}

static int getal(const cJSON *o, const char *naam, int standaard) {
    const cJSON *v = cJSON_GetObjectItem(o, naam);
    return cJSON_IsNumber(v) ? v->valueint : standaard;
}

static void regel_verwerken(char *regel) {
    char *spatie = strchr(regel, ' ');
    const char *json = "{}";
    if (spatie) { *spatie = 0; json = spatie + 1; }
    const char *onderwerp = regel;

    cJSON *doc = cJSON_Parse(json);
    if (!doc) return;                             // geen geldige JSON: negeren

    if (strcmp(onderwerp, OND_STUUR) == 0) {
        vermogen = getal(doc, "vermogen", 0);
        if (vermogen < 0) vermogen = 0;
        if (vermogen > 100) vermogen = 100;
        pomp = cJSON_IsTrue(cJSON_GetObjectItem(doc, "pomp"));
        zoemer = cJSON_IsTrue(cJSON_GetObjectItem(doc, "zoemer"));
        laatste_stuur = nu_ms();
        ooit_stuur = true;
    } else if (strcmp(onderwerp, OND_STATUS) == 0 || strcmp(onderwerp, OND_RECEPT_ANTWOORD) == 0) {
        publiceer(onderwerp, json);
    } else if (strcmp(onderwerp, OND_MELD) == 0) {
        int nr = getal(doc, "nr", 0);
        if (!al_gezien(nr)) publiceer(onderwerp, json);
        stuur_ack_nr(nr);                         // ook bij een herhaling: het scherm wacht erop
    } else if (strcmp(onderwerp, OND_NET_START) == 0) {
        const cJSON *server = cJSON_GetObjectItem(doc, "server");
        stuur_ack_voor(OND_NET_START);
        if (cJSON_IsString(server)) net_starten(server->valuestring, getal(doc, "poort", 1883));
    } else if (strcmp(onderwerp, OND_NET_STOP) == 0) {
        stuur_ack_voor(OND_NET_STOP);
        net_stoppen();
    }
    // onbekend onderwerp: negeren
    cJSON_Delete(doc);
}

static void lees_kabel(void) {
    static char regel[NIELS_MAX_REGEL + 1];
    static size_t len = 0;
    static bool te_lang = false;
    uint8_t buf[128];
    int n = uart_read_bytes(SCHERM_UART, buf, sizeof buf, pdMS_TO_TICKS(10));
    for (int i = 0; i < n; i++) {
        char c = (char)buf[i];
        if (c == '\r') continue;
        if (c == '\n') {
            regel[len] = 0;
            if (!te_lang && len > 0) regel_verwerken(regel);
            len = 0;
            te_lang = false;
        } else if (len < NIELS_MAX_REGEL) {
            regel[len++] = c;
        } else {
            te_lang = true;                       // rest van de regel weggooien
        }
    }
}

/* ---------------- ketel ---------------- */

static void uitgangen_bijwerken(void) {
    // Veiligheid: geen hartslag van het scherm = alles uit
    if (!ooit_stuur || nu_ms() - laatste_stuur > NIELS_STIL_MAX_MS) {
        vermogen = 0;
        pomp = false;
        zoemer = false;
    }
    bool ssr_aan = (nu_ms() % SSR_VENSTER_MS) < (uint32_t)vermogen * SSR_VENSTER_MS / 100;
    gpio_set_level(CONFIG_NIELS_PIN_SSR, ssr_aan);
    gpio_set_level(CONFIG_NIELS_PIN_POMP, pomp);
    gpio_set_level(CONFIG_NIELS_PIN_ZOEMER, zoemer);
}

static void sensor_taak(void *arg) {
    onewire_bus_handle_t bus = NULL;
    ds18b20_device_handle_t ds = NULL;
    onewire_bus_config_t bus_cfg = { .bus_gpio_num = CONFIG_NIELS_PIN_SENSOR };
    onewire_bus_rmt_config_t rmt_cfg = { .max_rx_bytes = 10 };
    ESP_ERROR_CHECK(onewire_new_bus_rmt(&bus_cfg, &rmt_cfg, &bus));

    ds18b20_config_t ds_cfg = {};
    while (true) {
        if (!ds && ds18b20_new_device_from_bus(bus, &ds_cfg, &ds) != ESP_OK) {
            ds = NULL;                            // geen sensor gevonden: straks opnieuw
            temp_ok = false;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        float t;
        if (ds18b20_trigger_temperature_conversion(ds) == ESP_OK &&   // wacht ~800 ms
            ds18b20_get_temperature(ds, &t) == ESP_OK && t > -50 && t < 150) {
            temp = t;
            temp_ok = true;
        } else {
            temp_ok = false;
            ds18b20_del_device(ds);
            ds = NULL;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void temperatuur_sturen(void) {
    cJSON *o = cJSON_CreateObject();
    if (temp_ok) {
        char t[16];
        snprintf(t, sizeof t, "%.1f", temp);
        cJSON_AddRawToObject(o, "temp", t);       // met één decimaal
    } else {
        cJSON_AddNullToObject(o, "temp");
    }
    cJSON_AddBoolToObject(o, "ok", temp_ok);

    char *json = cJSON_PrintUnformatted(o);
    if (json) {
        publiceer(OND_TEMP, json);
        free(json);
    }
    naar_scherm(OND_TEMP, o);                     // geeft o vrij
}

/* ---------------- start ---------------- */

void app_main(void) {
    slot = xSemaphoreCreateMutex();

    gpio_config_t uit = {
        .pin_bit_mask = (1ULL << CONFIG_NIELS_PIN_SSR) | (1ULL << CONFIG_NIELS_PIN_POMP) |
                        (1ULL << CONFIG_NIELS_PIN_ZOEMER),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&uit);
    uitgangen_bijwerken();                        // alles uit

    uart_config_t uc = {
        .baud_rate = NIELS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(SCHERM_UART, 2048, 2048, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(SCHERM_UART, &uc));
    ESP_ERROR_CHECK(uart_set_pin(SCHERM_UART, CONFIG_NIELS_PIN_SCHERM_TX, CONFIG_NIELS_PIN_SCHERM_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    esp_err_t err = nvs_flash_init();             // nodig voor wifi
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    wifi_starten();

    xTaskCreate(sensor_taak, "sensor", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Niels ESP gestart");
    stuur_net_status(NET_UIT, NULL);              // scherm ziet zo dat we (weer) leven

    uint32_t vorige_temp = 0, vorige_net = 0;
    while (true) {
        lees_kabel();                             // wacht hooguit 10 ms op data
        uitgangen_bijwerken();
        net_bijhouden();

        if (nu_ms() - vorige_temp >= TEMP_MS) {
            vorige_temp = nu_ms();
            temperatuur_sturen();
        }
        if (nu_ms() - vorige_net >= NET_STATUS_MS) {
            vorige_net = nu_ms();
            VERGRENDEL();
            net_toestand_t n = net;
            ONTGRENDEL();
            stuur_net_status(n, NULL);
        }
    }
}
