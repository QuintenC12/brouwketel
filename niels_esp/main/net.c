/*
 * net.c - MQTT voor Niels ESP (zie net.h)
 *
 * Alle verbinden en stoppen gebeurt in één taak (net_taak). net_start() en
 * net_stop() leggen alleen een vraag in een wachtrij, zodat de
 * ontvangsttaak nooit hoeft te wachten en brouw/stuur op tijd verwerkt
 * blijft (anders slaat de failsafe van 2 s aan).
 *
 * Verloop na net/start:
 *   1. net/status "bezig"
 *   2. wachten tot brouw_netwerk een IP-adres heeft (hooguit WIFI_MAX_MS)
 *   3. MQTT-client starten (hooguit MQTT_MAX_MS tot de broker antwoordt)
 *   4. net/status "verbonden", inschrijven op brouwketel/recept en het
 *      recept vragen met brouwketel/recept/vraag
 *   5. het recept van de server gaat als "recept {...}" naar het scherm
 * Lukt stap 2 of 3 niet: net/status "fout" met een reden.
 *
 * Er is maar één MQTT-client. Hij wordt één keer gemaakt en nooit
 * opgeruimd; bij een nieuwe net/start krijgt hij alleen een ander adres.
 */
#include "net.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brouw_netwerk.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"

#include "niels_uart.h"
#include "scherm_uart.h"

static const char *TAG = "net";

#define NET_STATUS_MS   5000    // zo vaak net/status herhalen
#define WIFI_MAX_MS     12000   // zo lang wachten op een IP-adres
#define MQTT_MAX_MS     12000   // zo lang wachten op de broker
#define MAX_BEWAARD     10      // meldingen bewaren als de server weg is

#define MQTT_RECEPT       MQTT_VOORVOEGSEL "recept"
#define MQTT_RECEPT_VRAAG MQTT_VOORVOEGSEL "recept/vraag"

typedef enum { NET_UIT, NET_BEZIG, NET_VERBONDEN, NET_FOUT } net_toestand_t;

// Wat net_taak aan het doen is
typedef enum { STAP_NIETS, STAP_WACHT_WIFI, STAP_WACHT_MQTT, STAP_VERBONDEN } stap_t;

typedef struct {
    bool start;              // true = net/start, false = net/stop
    char server[64];
    int  poort;
} vraag_t;

static QueueHandle_t vragen;
static SemaphoreHandle_t slot;
#define VERGRENDEL()   xSemaphoreTake(slot, portMAX_DELAY)
#define ONTGRENDEL()   xSemaphoreGive(slot)

// Alleen net_taak maakt of verandert de client
static esp_mqtt_client_handle_t mqtt = NULL;

// Achter het slot (de MQTT-events draaien in een andere taak)
static net_toestand_t net = NET_UIT;
static stap_t   stap = STAP_NIETS;
static uint32_t stap_sinds = 0;
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

/* Aanroepen met het slot vast. Stuurt net/status alleen bij een verandering. */
static void zet_net(net_toestand_t nieuw, const char *reden)
{
    if (nieuw == net) return;
    net = nieuw;
    ESP_LOGI(TAG, "net/status %d %s", net, reden ? reden : "");
    stuur_net_status(net, reden);
}

static void zet_stap(stap_t nieuw)
{
    stap = nieuw;
    stap_sinds = nu_ms();
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
    // Meldingen met QoS 1: de broker bevestigt ze. De rest met QoS 0.
    if (verbonden) esp_mqtt_client_publish(mqtt, volledig, json, 0, is_melding ? 1 : 0, 0);
}

/* ---------------- MQTT-events (draaien in de taak van esp-mqtt) ---------------- */

static void recept_doorgeven(const char *data, int lengte)
{
    // Op één regel zetten (zonder regeleinden) en naar het scherm.
    cJSON *recept = cJSON_ParseWithLength(data, lengte);
    char *regel = recept ? cJSON_PrintUnformatted(recept) : NULL;
    cJSON_Delete(recept);
    if (!regel) {
        ESP_LOGW(TAG, "recept is geen geldige JSON");
        return;
    }
    // Het scherm aanvaardt regels tot NIELS_MAX_REGEL tekens.
    esp_err_t err = scherm_uart_verstuur(OND_RECEPT, regel);
    if (err != ESP_OK) ESP_LOGW(TAG, "recept niet naar het scherm: %s", esp_err_to_name(err));
    free(regel);
}

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
        zet_stap(STAP_VERBONDEN);
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
        if (stap == STAP_VERBONDEN) {             // de client verbindt zelf opnieuw
            zet_stap(STAP_WACHT_MQTT);
            zet_net(NET_BEZIG, "verbinding verloren");
        }
        ONTGRENDEL();
        break;

    case MQTT_EVENT_DATA:
        if (ev->topic_len != (int)strlen(MQTT_RECEPT) ||
            strncmp(ev->topic, MQTT_RECEPT, ev->topic_len) != 0) break;
        if (ev->current_data_offset != 0 || ev->data_len != ev->total_data_len) {
            ESP_LOGW(TAG, "recept te groot voor de MQTT-buffer");
            break;
        }
        recept_doorgeven(ev->data, ev->data_len);
        break;

    default:
        break;
    }
}

/* ---------------- net_taak ---------------- */

/* MQTT-client starten met het adres van het scherm, of anders dat uit het
 * wifi-formulier. Geeft een reden terug als het niet lukt, anders NULL. */
static const char *mqtt_starten(const vraag_t *v)
{
    brouw_netwerk_mqtt_t formulier;
    bool formulier_ok = brouw_netwerk_lees_mqtt(&formulier) == ESP_OK;

    const char *host = v->server[0] ? v->server : (formulier_ok ? formulier.host : NULL);
    int poort = v->poort > 0 ? v->poort : (formulier_ok ? formulier.poort : 1883);
    if (!host || !host[0]) return "geen serveradres";

    char uri[280];                            // host mag tot 253 tekens zijn
    snprintf(uri, sizeof uri, "mqtt://%s:%d", host, poort);
    ESP_LOGI(TAG, "verbinden met %s", uri);

    if (!mqtt) {
        esp_mqtt_client_config_t cfg = {
            .broker.address.uri = uri,
            // de ketel-ID uit het formulier, anders een vaste naam
            .credentials.client_id = formulier_ok ? formulier.ketel_id : "niels-esp",
            .buffer.size = 2048,                  // het recept is een lang bericht
        };
        mqtt = esp_mqtt_client_init(&cfg);
        if (!mqtt) return "MQTT-client";
        esp_mqtt_client_register_event(mqtt, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    } else if (esp_mqtt_client_set_uri(mqtt, uri) != ESP_OK) {
        return "ongeldig serveradres";
    }
    return esp_mqtt_client_start(mqtt) == ESP_OK ? NULL : "MQTT-client";
}

static void mqtt_stoppen(void)
{
    if (mqtt) esp_mqtt_client_stop(mqtt);     // al gestopt? dan doet dit niets
}

static void net_taak(void *parameters)
{
    vraag_t huidige = { 0 };
    uint32_t vorige_status = nu_ms();

    while (1) {
        vraag_t v;
        if (xQueueReceive(vragen, &v, pdMS_TO_TICKS(200)) == pdTRUE) {
            mqtt_stoppen();                       // eerst een vorige verbinding stoppen
            VERGRENDEL();
            if (v.start) {
                huidige = v;
                zet_stap(STAP_WACHT_WIFI);
                net = NET_UIT;                    // zodat "bezig" zeker verstuurd wordt
                zet_net(NET_BEZIG, NULL);
            } else {
                zet_stap(STAP_NIETS);
                zet_net(NET_UIT, NULL);
            }
            ONTGRENDEL();
        }

        VERGRENDEL();
        stap_t nu = stap;
        uint32_t duur = nu_ms() - stap_sinds;
        ONTGRENDEL();

        const char *fout = NULL;
        bool stoppen = false;
        if (nu == STAP_WACHT_WIFI) {
            brouw_netwerk_status_t w = brouw_netwerk_status();
            if (w == BROUW_NETWERK_IP_GEREED) {
                fout = mqtt_starten(&huidige);
                if (!fout) {
                    VERGRENDEL();
                    if (stap == STAP_WACHT_WIFI) zet_stap(STAP_WACHT_MQTT);
                    ONTGRENDEL();
                }
            } else if (w == BROUW_NETWERK_SETUP) {
                fout = "wifi niet ingesteld";     // eerst het formulier invullen
            } else if (w == BROUW_NETWERK_FOUT || w == BROUW_NETWERK_UIT) {
                fout = "wifi";
            } else if (duur > WIFI_MAX_MS) {
                fout = "wifi";
            }
        } else if (nu == STAP_WACHT_MQTT && duur > MQTT_MAX_MS) {
            fout = "server onbereikbaar";
            stoppen = true;
        }

        if (fout) {
            VERGRENDEL();
            bool nog_zo = stap == nu;             // intussen niet toch verbonden?
            if (nog_zo) {
                zet_stap(STAP_NIETS);
                zet_net(NET_FOUT, fout);
            }
            ONTGRENDEL();
            if (nog_zo && stoppen) mqtt_stoppen();
        }

        if (nu_ms() - vorige_status >= NET_STATUS_MS) {
            vorige_status = nu_ms();
            VERGRENDEL();
            net_toestand_t t = net;
            ONTGRENDEL();
            stuur_net_status(t, NULL);
        }
    }
}

/* ---------------- voor main.c ---------------- */

void net_start(const char *server, int poort)
{
    vraag_t v = { .start = true, .poort = poort };
    if (server) strlcpy(v.server, server, sizeof v.server);
    if (xQueueSend(vragen, &v, 0) != pdTRUE) ESP_LOGW(TAG, "net/start verloren");
}

void net_stop(void)
{
    vraag_t v = { .start = false };
    if (xQueueSend(vragen, &v, 0) != pdTRUE) ESP_LOGW(TAG, "net/stop verloren");
}

void net_init(void)
{
    slot = xSemaphoreCreateMutex();
    vragen = xQueueCreate(4, sizeof(vraag_t));
    stuur_net_status(NET_UIT, NULL);              // het scherm ziet zo dat we (weer) leven
    xTaskCreate(net_taak, "net", 4096, NULL, 4, NULL);
}
