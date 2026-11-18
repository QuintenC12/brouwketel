/*
 * net.c - vertaling tussen het scherm (UART) en de webapp (MQTT), zie net.h
 *
 * Volgt "MQTT-afspraken met elektronica" (back-end-team, 12 nov 2026).
 * Alle topics beginnen met brouwer/<id>/, met <id> = het wifi-MAC-adres in
 * kleine letters zonder dubbelpunten, bv. brouwer/a4cf12b3c8e0/.
 *
 *   Verbinden   client-id brouwer-<id>, keepalive 30 s, clean session,
 *               last will: status = "offline" (QoS 1, retained)
 *   Aanmelden   info (JSON, retained) en status = "online" (retained)
 *   Metingen    sensor/temperatuur  "66.8"   elke seconde (alleen een geldige meting)
 *               sensor/sg           "1.050"  als op het scherm een SG wordt ingevuld
 *               sensor/doeltemperatuur, sensor/resterend (s), sensor/maischstap
 *                                   elke seconde, uit brouw/status van het scherm
 *   Toestand    actuator/verwarming/state  "0".."100"  (vermogen van de PID, retained)
 *               actuator/pomp/state        "on"/"off"  (retained)
 *               actuator/zoemer/state      "on"/"off"  (retained)
 *   Opdrachten  cmd/recept  -> recept omzetten en naar het scherm, antwoord op ack
 *               cmd/<iets anders> -> ack met ok:false (het scherm regelt de ketel)
 *   Fouten      fout {"code":"sensor"|"recept","message":".."}
 *
 * Extra, nog niet in de afspraken (voorstel aan het back-end-team):
 *   fase   "geen" | "maischen" | "spoelen" | "koken"   (retained, bij een verandering)
 *   event  {"naam":"hop_alarm","tekst":"Hop toevoegen: Citra - 20 g","hop":"Citra - 20 g"}
 *          een melding van het scherm, met een tekst die de webapp zo kan tonen
 *
 * Wat het scherm ziet, blijft zoals in het regelboekje:
 *   net/start -> net/status "bezig", dan "verbonden" (of "fout"), dan recept {...}
 *
 * Het recept komt van de webapp (cmd/recept), niet op vraag. Niels ESP
 * bewaart het laatste recept. Zo kan je het recept eerst vanuit de webapp
 * sturen en daarna op het scherm op Automatisch klikken.
 */
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brouw_netwerk.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"

#include "niels_uart.h"
#include "recept.h"
#include "scherm_uart.h"

static const char *TAG = "net";

#define FIRMWARE        "0.4.0"
#define NET_STATUS_MS   5000    // zo vaak net/status herhalen naar het scherm
#define VERBIND_MAX_MS  25000   // na net/start: zo lang mag verbinden duren (scherm: 30 s)
#define ACK_MAX_MS      8000    // zo lang wachten op recept_ok/recept_fout (back-end: 10 s)
#define TOESTAND_MS     1000    // zo vaak net/toestand (wifi, MQTT, modus) naar het scherm

typedef enum { NET_UIT, NET_BEZIG, NET_VERBONDEN, NET_FOUT } net_toestand_t;

static SemaphoreHandle_t slot;
#define VERGRENDEL()   xSemaphoreTake(slot, portMAX_DELAY)
#define ONTGRENDEL()   xSemaphoreGive(slot)

static char id[13];                         // MAC-adres, bv. a4cf12b3c8e0
static char basis[24];                      // "brouwer/<id>/"

// Alleen net_taak maakt de client (één keer); daarna verandert hij niet meer
static esp_mqtt_client_handle_t mqtt = NULL;

// Achter het slot
static bool     verbonden = false;          // MQTT-broker heeft ons aanvaard
static bool     automatisch = false;        // scherm wacht na net/start op het recept
static uint32_t automatisch_sinds = 0;
static char     reserve_server[64] = "";    // uit net/start, als het formulier niets heeft
static int      reserve_poort = 0;
static net_toestand_t gemeld = NET_UIT;     // laatst naar het scherm gestuurd
static const char    *fout_reden = NULL;
static char    *recept_regel = NULL;        // laatste recept, al omgezet voor het scherm
static bool     recept_gestuurd = false;    // al naar het scherm in deze Automatisch-ronde
static char     ack_id[64] = "";            // message_id die nog een ack wacht
static uint32_t ack_sinds = 0;
static int      gevraagde_modus = -1;       // laatst toegepast uit net/modus: 1 = configuratie, -1 = nog niets

// Laatst gepubliceerde toestand (-1 = nog niet), alleen vanuit temp_task
static int  vorig_vermogen = -1, vorige_pomp = -1, vorige_zoemer = -1;
static bool vorige_sensor_ok = true;
static char vorige_fase[16] = "";          // laatst gepubliceerde fase, alleen receive_task

static uint32_t nu_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ---------------- MQTT-hulpjes ---------------- */

static void publiceer(const char *rest, const char *tekst, int qos, bool retained)
{
    char topic[96];
    snprintf(topic, sizeof topic, "%s%s", basis, rest);
    esp_mqtt_client_publish(mqtt, topic, tekst, 0, qos, retained);
}

static void stuur_ack(const char *message_id, bool ok, const char *fout)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "message_id", message_id);
    cJSON_AddBoolToObject(o, "ok", ok);
    if (!ok && fout) cJSON_AddStringToObject(o, "fout", fout);
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (json) {
        publiceer("ack", json, 1, false);
        free(json);
    }
    ESP_LOGI(TAG, "ack %s ok=%d %s", message_id, ok, fout ? fout : "");
}

static void stuur_fout(const char *code, const char *bericht)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "code", code);
    cJSON_AddStringToObject(o, "message", bericht);
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (json) {
        publiceer("fout", json, 1, false);
        free(json);
    }
}

static void stuur_info(void)
{
    brouw_netwerk_mqtt_t formulier;
    bool f_ok = brouw_netwerk_lees_mqtt(&formulier) == ESP_OK;

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", id);
    cJSON_AddStringToObject(o, "type", "ketel");
    cJSON_AddStringToObject(o, "name", f_ok ? formulier.ketel_id : "Brouwketel");
    cJSON_AddStringToObject(o, "firmware", FIRMWARE);

    cJSON *sensors = cJSON_AddArrayToObject(o, "sensors");
    const char *s_namen[] = { "temperatuur", "sg", "doeltemperatuur", "resterend", "maischstap" };
    const char *s_eenheid[] = { "°C", "SG", "°C", "s", "" };
    for (int i = 0; i < 5; i++) {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "name", s_namen[i]);
        cJSON_AddStringToObject(s, "unit", s_eenheid[i]);
        cJSON_AddItemToArray(sensors, s);
    }

    cJSON *actuators = cJSON_AddArrayToObject(o, "actuators");
    const char *aan_uit[] = { "on", "off" };
    const char *procent[] = { "0-100" };
    const char *a_namen[] = { "verwarming", "pomp", "zoemer" };
    for (int i = 0; i < 3; i++) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "name", a_namen[i]);
        cJSON_AddItemToObject(a, "values", i == 0 ? cJSON_CreateStringArray(procent, 1)
                                                  : cJSON_CreateStringArray(aan_uit, 2));
        cJSON_AddItemToArray(actuators, a);
    }

    // Alleen het recept: setpoint en actuator regelt het scherm zelf.
    const char *cmds[] = { "recept" };
    cJSON_AddItemToObject(o, "commands", cJSON_CreateStringArray(cmds, 1));

    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (json) {
        publiceer("info", json, 1, true);
        free(json);
    }
}

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

/* net/toestand: wifi en MQTT apart, en of de configuratiemodus (hotspot) open staat */
static void stuur_toestand(bool wifi, bool mqtt_ok, bool configuratie)
{
    char json[80];
    snprintf(json, sizeof json, "{\"wifi\":%s,\"mqtt\":%s,\"modus\":\"%s\"}",
             wifi ? "true" : "false", mqtt_ok ? "true" : "false",
             configuratie ? "configuratie" : "werking");
    scherm_uart_verstuur(OND_NET_TOESTAND, json);
}

/* Met het slot vast: wacht het scherm en is er een recept? Dan sturen. */
static void recept_naar_scherm_als_klaar(void)
{
    if (!automatisch || !verbonden || !recept_regel || recept_gestuurd) return;
    esp_err_t err = scherm_uart_verstuur(OND_RECEPT, recept_regel);
    if (err != ESP_OK) ESP_LOGW(TAG, "recept niet naar het scherm: %s", esp_err_to_name(err));
    recept_gestuurd = true;
}

/* ---------------- MQTT-events (taak van esp-mqtt) ---------------- */

static void cmd_recept(const char *message_id, const cJSON *data)
{
    char *regel = NULL;
    const char *reden = recept_omzetten(data, &regel);
    if (reden) {
        stuur_ack(message_id, false, reden);
        return;
    }

    VERGRENDEL();
    free(recept_regel);
    recept_regel = regel;
    recept_gestuurd = false;
    bool scherm_wacht = automatisch;
    if (scherm_wacht) {
        // Het scherm wacht al: we antwoorden pas als het recept_ok/recept_fout meldt.
        strlcpy(ack_id, message_id, sizeof ack_id);
        ack_sinds = nu_ms();
        recept_naar_scherm_als_klaar();
    } else {
        ack_id[0] = 0;
    }
    ONTGRENDEL();

    // Het scherm wacht nog niet: het recept is geladen en gaat naar het
    // scherm bij de volgende klik op Automatisch.
    if (!scherm_wacht) stuur_ack(message_id, true, NULL);
}

static void opdracht_verwerken(const char *opdracht, const char *data, int lengte)
{
    cJSON *doc = cJSON_ParseWithLength(data, lengte);
    const cJSON *mid = cJSON_GetObjectItemCaseSensitive(doc, "message_id");
    char message_id[64] = "";
    if (cJSON_IsString(mid)) strlcpy(message_id, mid->valuestring, sizeof message_id);
    else if (cJSON_IsNumber(mid)) snprintf(message_id, sizeof message_id, "%.0f", mid->valuedouble);

    if (!doc || !message_id[0]) {
        ESP_LOGW(TAG, "cmd/%s zonder geldige JSON of message_id", opdracht);
    } else if (strcmp(opdracht, "recept") == 0) {
        cmd_recept(message_id, cJSON_GetObjectItemCaseSensitive(doc, "data"));
    } else {
        stuur_ack(message_id, false, "niet ondersteund: het scherm regelt de ketel");
    }
    cJSON_Delete(doc);
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t evid, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)evid) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "verbonden met de broker");
        char topic[64];
        snprintf(topic, sizeof topic, "%scmd/#", basis);
        esp_mqtt_client_subscribe(ev->client, topic, 2);   // recept komt met QoS 2
        stuur_info();
        publiceer("status", "online", 1, true);
        vorig_vermogen = vorige_pomp = vorige_zoemer = -1;  // toestand opnieuw publiceren
        vorige_fase[0] = 0;

        VERGRENDEL();
        verbonden = true;
        recept_naar_scherm_als_klaar();
        ONTGRENDEL();
        break;
    }

    case MQTT_EVENT_DISCONNECTED:
        // De client verbindt zelf opnieuw; de broker stuurt de last will.
        VERGRENDEL();
        verbonden = false;
        ONTGRENDEL();
        break;

    case MQTT_EVENT_DATA: {
        // brouwer/<id>/cmd/<opdracht>, in één stuk (buffer 4 kB)
        size_t b = strlen(basis);
        if (ev->topic_len <= (int)(b + 4) || strncmp(ev->topic, basis, b) != 0 ||
            strncmp(ev->topic + b, "cmd/", 4) != 0) break;
        if (ev->current_data_offset != 0 || ev->data_len != ev->total_data_len) {
            ESP_LOGW(TAG, "opdracht te groot voor de MQTT-buffer");
            break;
        }
        char opdracht[24];
        int n = ev->topic_len - (int)b - 4;
        if (n >= (int)sizeof opdracht) n = sizeof opdracht - 1;
        memcpy(opdracht, ev->topic + b + 4, n);
        opdracht[n] = 0;
        opdracht_verwerken(opdracht, ev->data, ev->data_len);
        break;
    }

    default:
        break;
    }
}

/* ---------------- net_taak ---------------- */

/* MQTT-client maken, of (opnieuw = true) de bestaande client opnieuw starten
 * met de gegevens uit het formulier. De client zelf blijft bestaan, zodat
 * andere taken er veilig naar kunnen blijven publiceren. */
static void mqtt_maken(bool opnieuw)
{
    brouw_netwerk_mqtt_t formulier;
    char host[254] = "";
    int poort = 1883;
    if (brouw_netwerk_lees_mqtt(&formulier) == ESP_OK) {
        strlcpy(host, formulier.host, sizeof host);
        poort = formulier.poort;
    } else {
        VERGRENDEL();
        strlcpy(host, reserve_server, sizeof host);
        if (reserve_poort > 0) poort = reserve_poort;
        ONTGRENDEL();
    }
    if (!host[0]) return;                   // nog geen broker bekend

    char client_id[24], lw_topic[40];
    snprintf(client_id, sizeof client_id, "brouwer-%s", id);
    snprintf(lw_topic, sizeof lw_topic, "%sstatus", basis);
    esp_mqtt_client_config_t cfg = {
        .broker.address.hostname = host,
        .broker.address.port = poort,
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
        .credentials.client_id = client_id,
        .session.keepalive = 30,
        .session.disable_clean_session = false,
        .session.last_will = {
            .topic = lw_topic,
            .msg = "offline",
            .qos = 1,
            .retain = 1,
        },
        .buffer.size = 4096,                // een recept is ongeveer 1,5 kB
    };
    if (opnieuw && mqtt) {
        esp_mqtt_client_stop(mqtt);
        VERGRENDEL();
        verbonden = false;
        ONTGRENDEL();
        esp_mqtt_set_config(mqtt, &cfg);
        ESP_LOGI(TAG, "MQTT opnieuw naar %s:%d", host, poort);
        esp_mqtt_client_start(mqtt);
        return;
    }
    esp_mqtt_client_handle_t c = esp_mqtt_client_init(&cfg);
    if (!c) return;
    esp_mqtt_client_register_event(c, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    mqtt = c;
    ESP_LOGI(TAG, "MQTT naar %s:%d als %s", host, poort, client_id);
    esp_mqtt_client_start(c);
}

static void net_taak(void *parameters)
{
    uint32_t vorige_status = 0, vorige_toestand = 0;
    int vorige_wifi = -1, vorige_mqtt = -1, vorige_config = -1;
    bool was_setup = false;
    while (1) {
        brouw_netwerk_status_t netwerk = brouw_netwerk_status();

        // Na de configuratiemodus kan de broker in het formulier veranderd zijn:
        // zodra er weer wifi is, MQTT opnieuw opzetten met de nieuwe gegevens.
        bool is_setup = netwerk == BROUW_NETWERK_SETUP;
        if (is_setup) was_setup = true;

        // MQTT starten zodra er wifi is (en daarna nooit meer stoppen)
        if (netwerk == BROUW_NETWERK_IP_GEREED) {
            if (!mqtt) {
                mqtt_maken(false);
                was_setup = false;
            } else if (was_setup) {
                mqtt_maken(true);
                was_setup = false;
            }
        }

        char verlopen_ack[64] = "";
        VERGRENDEL();
        // Wat moet het scherm zien?
        net_toestand_t nu;
        const char *reden = NULL;
        if (verbonden) {
            nu = NET_VERBONDEN;
        } else if (!automatisch) {
            nu = NET_UIT;
        } else if (fout_reden) {
            nu = NET_FOUT;
            reden = fout_reden;
        } else if (brouw_netwerk_status() == BROUW_NETWERK_SETUP) {
            nu = NET_FOUT;
            reden = fout_reden = "wifi niet ingesteld";
        } else if (nu_ms() - automatisch_sinds > VERBIND_MAX_MS) {
            nu = NET_FOUT;
            reden = fout_reden = brouw_netwerk_status() == BROUW_NETWERK_IP_GEREED
                               ? "server onbereikbaar" : "wifi";
        } else {
            nu = NET_BEZIG;
        }
        bool melden = nu != gemeld || nu_ms() - vorige_status >= NET_STATUS_MS;
        gemeld = nu;

        // Recept naar het scherm gestuurd, maar geen antwoord van het scherm?
        if (ack_id[0] && nu_ms() - ack_sinds > ACK_MAX_MS) {
            strlcpy(verlopen_ack, ack_id, sizeof verlopen_ack);
            ack_id[0] = 0;
        }
        ONTGRENDEL();

        if (melden) {
            vorige_status = nu_ms();
            stuur_net_status(nu, reden);
        }

        // Wifi en MQTT apart voor het wifi-logo op het scherm
        VERGRENDEL();
        int mqtt_nu = verbonden;
        ONTGRENDEL();
        int wifi_nu = netwerk == BROUW_NETWERK_IP_GEREED;
        int config_nu = is_setup;
        if (wifi_nu != vorige_wifi || mqtt_nu != vorige_mqtt || config_nu != vorige_config ||
            nu_ms() - vorige_toestand >= TOESTAND_MS) {
            vorige_toestand = nu_ms();
            vorige_wifi = wifi_nu;
            vorige_mqtt = mqtt_nu;
            vorige_config = config_nu;
            stuur_toestand(wifi_nu, mqtt_nu, config_nu);
        }
        if (verlopen_ack[0] && mqtt)
            stuur_ack(verlopen_ack, false, "het scherm heeft het recept niet bevestigd");

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ---------------- van het scherm ---------------- */

void net_start(const char *server, int poort)
{
    VERGRENDEL();
    if (server) strlcpy(reserve_server, server, sizeof reserve_server);
    if (poort > 0) reserve_poort = poort;
    automatisch = true;
    automatisch_sinds = nu_ms();
    fout_reden = NULL;
    recept_gestuurd = false;
    gemeld = NET_UIT;                       // net/status meteen opnieuw sturen
    if (!verbonden) stuur_net_status(NET_BEZIG, NULL);
    recept_naar_scherm_als_klaar();
    ONTGRENDEL();
}

void net_stop(void)
{
    VERGRENDEL();
    automatisch = false;
    fout_reden = NULL;
    ONTGRENDEL();
}

void net_modus(bool configuratie)
{
    // net/modus komt elke seconde; alleen een verandering doet iets
    int nieuw = configuratie ? 1 : 0;
    if (nieuw == gevraagde_modus) return;
    gevraagde_modus = nieuw;
    ESP_LOGI(TAG, "scherm vraagt %s", configuratie ? "configuratiemodus" : "werkingsmodus");
    brouw_netwerk_setup(configuratie);
}

/* Melding van het scherm -> brouwer/<id>/event, met een tekst die de webapp
 * zo kan tonen. Het nummer (nr) is alleen voor de kabel en valt weg; de
 * extra velden (stap, hop, sg, reden) gaan mee.
 *   brouw/meld {"nr":5,"naam":"hop_alarm","hop":"Citra - 20 g"}
 *   -> event {"naam":"hop_alarm","tekst":"Hop toevoegen: Citra - 20 g","hop":"Citra - 20 g"} */
static void stuur_event(const cJSON *doc, const char *naam)
{
    const char *hop = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(doc, "hop"));
    const char *reden = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(doc, "reden"));
    const cJSON *stap = cJSON_GetObjectItemCaseSensitive(doc, "stap");
    const cJSON *sg = cJSON_GetObjectItemCaseSensitive(doc, "sg");
    int st = cJSON_IsNumber(stap) ? stap->valueint : 0;

    char tekst[96];
    if      (!strcmp(naam, "start"))            snprintf(tekst, sizeof tekst, "Brouwen gestart");
    else if (!strcmp(naam, "pauze"))            snprintf(tekst, sizeof tekst, "Gepauzeerd");
    else if (!strcmp(naam, "hervat"))           snprintf(tekst, sizeof tekst, "Hervat");
    else if (!strcmp(naam, "stop"))             snprintf(tekst, sizeof tekst, "Brouwen gestopt");
    else if (!strcmp(naam, "volgende_stap"))    snprintf(tekst, sizeof tekst, "Maischstap %d gestart", st);
    else if (!strcmp(naam, "maischen_klaar"))   snprintf(tekst, sizeof tekst, "Maischen klaar, spoelen");
    else if (!strcmp(naam, "start_koken"))      snprintf(tekst, sizeof tekst, "Koken gestart");
    else if (!strcmp(naam, "hop_alarm"))        snprintf(tekst, sizeof tekst, "Hop toevoegen: %s", hop ? hop : "?");
    else if (!strcmp(naam, "hop_toegevoegd"))   snprintf(tekst, sizeof tekst, "Hop toegevoegd: %s", hop ? hop : "?");
    else if (!strcmp(naam, "koken_klaar"))      snprintf(tekst, sizeof tekst, "Koken klaar");
    else if (!strcmp(naam, "schema_gewijzigd")) snprintf(tekst, sizeof tekst, "Brouwschema aangepast op het scherm");
    else if (!strcmp(naam, "sensorfout"))       snprintf(tekst, sizeof tekst, "Sensorfout: verwarming uit");
    else if (!strcmp(naam, "sg"))               snprintf(tekst, sizeof tekst, "SG na maischstap %d: %.3f", st,
                                                         cJSON_IsNumber(sg) ? sg->valuedouble : 0.0);
    else if (!strcmp(naam, "recept_ok"))        snprintf(tekst, sizeof tekst, "Recept geladen op het scherm");
    else if (!strcmp(naam, "recept_fout"))      snprintf(tekst, sizeof tekst, "Recept geweigerd: %s", reden ? reden : "?");
    else                                        snprintf(tekst, sizeof tekst, "%s", naam);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "naam", naam);
    cJSON_AddStringToObject(o, "tekst", tekst);
    if (cJSON_IsNumber(stap)) cJSON_AddNumberToObject(o, "stap", st);
    if (hop) cJSON_AddStringToObject(o, "hop", hop);
    if (cJSON_IsNumber(sg)) cJSON_AddNumberToObject(o, "sg", sg->valuedouble);
    if (reden) cJSON_AddStringToObject(o, "reden", reden);
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (json) {
        publiceer("event", json, 1, false);
        free(json);
    }
}

void net_van_scherm(const char *onderwerp, const char *json)
{
    bool is_meld = strcmp(onderwerp, OND_MELD) == 0;
    bool mqtt_ok;
    VERGRENDEL();
    mqtt_ok = verbonden;
    ONTGRENDEL();

    if (!is_meld) {                          // brouw/status
        if (!mqtt_ok) return;
        // {"fase":"maischen","stap":2,"doel":63,"rest":754,"pauze":false}
        // De getallen als sensor, de fase als tekst, zodat de webapp ze zo toont.
        cJSON *st = cJSON_Parse(json);
        const char *fase = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(st, "fase"));
        if (fase && strcmp(fase, vorige_fase) != 0) {
            strlcpy(vorige_fase, fase, sizeof vorige_fase);
            publiceer("fase", fase, 1, true);       // alleen bij een verandering
        }
        static const char *velden[][2] = {
            { "doel", "sensor/doeltemperatuur" },
            { "rest", "sensor/resterend" },
            { "stap", "sensor/maischstap" },
        };
        for (int i = 0; i < 3; i++) {
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(st, velden[i][0]);
            if (cJSON_IsNumber(v)) {
                char t[16];
                snprintf(t, sizeof t, "%g", v->valuedouble);
                publiceer(velden[i][1], t, 0, false);
            }
        }
        cJSON_Delete(st);
        return;
    }

    cJSON *doc = cJSON_Parse(json);
    const char *naam = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(doc, "naam"));
    const char *reden = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(doc, "reden"));

    if (naam && (strcmp(naam, "recept_ok") == 0 || strcmp(naam, "recept_fout") == 0)) {
        bool ok = strcmp(naam, "recept_ok") == 0;
        char wachtend[64];
        VERGRENDEL();
        strlcpy(wachtend, ack_id, sizeof wachtend);
        ack_id[0] = 0;
        if (ok) automatisch = false;         // het scherm heeft zijn recept
        ONTGRENDEL();
        if (mqtt_ok && wachtend[0]) {
            stuur_ack(wachtend, ok, reden ? reden : "recept geweigerd door het scherm");
        } else if (mqtt_ok && !ok) {
            char tekst[96];
            snprintf(tekst, sizeof tekst, "recept geweigerd door het scherm: %s", reden ? reden : "?");
            stuur_fout("recept", tekst);
        }
    } else if (naam && strcmp(naam, "sg") == 0 && mqtt_ok) {
        // {"nr":..,"naam":"sg","stap":2,"sg":1.050} -> sensor/sg "1.050"
        const cJSON *sg = cJSON_GetObjectItemCaseSensitive(doc, "sg");
        if (cJSON_IsNumber(sg)) {
            char t[16];
            snprintf(t, sizeof t, "%.3f", sg->valuedouble);
            publiceer("sensor/sg", t, 0, false);
        }
    }
    if (naam && mqtt_ok) stuur_event(doc, naam);
    cJSON_Delete(doc);
}

/* ---------------- van de ketel ---------------- */

void net_ketel(float temp, bool ok, int vermogen, bool pomp, bool zoemer)
{
    bool mqtt_ok;
    VERGRENDEL();
    mqtt_ok = verbonden;
    ONTGRENDEL();
    if (!mqtt_ok) {
        vorige_sensor_ok = ok;
        return;
    }

    if (ok) {
        char t[16];
        snprintf(t, sizeof t, "%.1f", temp);
        publiceer("sensor/temperatuur", t, 0, false);
    } else if (vorige_sensor_ok) {
        stuur_fout("sensor", "temperatuursensor geeft geen waarde, verwarming uitgezet");
    }
    vorige_sensor_ok = ok;

    // Toestand alleen bij een verandering (retained)
    if (vermogen != vorig_vermogen) {
        char v[8];
        snprintf(v, sizeof v, "%d", vermogen);
        publiceer("actuator/verwarming/state", v, 1, true);
        vorig_vermogen = vermogen;
    }
    if ((int)pomp != vorige_pomp) {
        publiceer("actuator/pomp/state", pomp ? "on" : "off", 1, true);
        vorige_pomp = pomp;
    }
    if ((int)zoemer != vorige_zoemer) {
        publiceer("actuator/zoemer/state", zoemer ? "on" : "off", 1, true);
        vorige_zoemer = zoemer;
    }
}

/* ---------------- start ---------------- */

void net_init(void)
{
    slot = xSemaphoreCreateMutex();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(id, sizeof id, "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(basis, sizeof basis, "brouwer/%s/", id);
    ESP_LOGI(TAG, "MQTT-topics: %s...", basis);

    stuur_net_status(NET_UIT, NULL);         // het scherm ziet zo dat we (weer) leven
    xTaskCreate(net_taak, "net", 4096, NULL, 4, NULL);
}
