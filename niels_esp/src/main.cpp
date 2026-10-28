/*
 * Niels ESP - eerste versie
 *
 * Doet het werk aan de ketel en praat met twee kanten:
 *   - het scherm (CrowPanel) via een seriële kabel, regels "onderwerp {json}"
 *   - de MQTT-server via wifi, zodat de webapp alles kan zien
 *
 * Aan de ketel: DS18B20 (temperatuur), SSR (verwarming), pomp, zoemer.
 * Het regelboekje in het project beschrijft alle berichten.
 *
 * Veiligheid: hoort Niels ESP NIELS_STIL_MAX_MS niets van het scherm
 * (geen brouw/stuur), dan gaan verwarming, pomp en zoemer uit.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "niels_uart.h"

#if __has_include("geheim.h")
#include "geheim.h"
#else
#warning "include/geheim.h ontbreekt: kopieer geheim.h.voorbeeld en vul je wifi in"
#define WIFI_NAAM       ""
#define WIFI_WACHTWOORD ""
#endif

// TODO: pas aan aan de bedrading van Niels ESP
#define PIN_SCHERM_TX  17   // naar RX van het scherm
#define PIN_SCHERM_RX  18   // naar TX van het scherm
#define PIN_SENSOR     4    // DS18B20 data (met 4,7 kOhm naar 3,3 V)
#define PIN_SSR        5
#define PIN_POMP       6
#define PIN_ZOEMER     7

#define SSR_VENSTER_MS   2000   // vermogen 40 % = 800 ms aan, 1200 ms uit
#define TEMP_MS          1000
#define NET_STATUS_MS    5000
#define VERBIND_MAX_MS   20000  // zo lang mag wifi + MQTT verbinden duren
#define MQTT_POGING_MS   2000

// Aanname: de server stuurt het recept op brouwketel/recept zodra
// Niels ESP iets publiceert op brouwketel/recept/vraag.
#define MQTT_RECEPT       MQTT_VOORVOEGSEL "recept"
#define MQTT_RECEPT_VRAAG MQTT_VOORVOEGSEL "recept/vraag"

static HardwareSerial scherm(1);
static OneWire onewire(PIN_SENSOR);
static DallasTemperature sensor(&onewire);
static WiFiClient wifi_client;
static PubSubClient mqtt(wifi_client);

// Wat het scherm wil
static uint8_t  vermogen = 0;
static bool     pomp = false, zoemer = false;
static uint32_t laatste_stuur = 0;
static bool     ooit_stuur = false;

// Verbinding met de server
enum NetToestand { NET_UIT, NET_BEZIG, NET_VERBONDEN, NET_FOUT };
static NetToestand net = NET_UIT;
static bool     net_gewenst = false;
static char     server[40] = "";
static uint16_t poort = 1883;
static uint32_t verbind_start = 0;
static uint32_t laatste_poging = 0;

// Meldingen die nog naar MQTT moeten (als de server even weg was)
#define MAX_BEWAARD 10
static String bewaard[MAX_BEWAARD];
static int    aantal_bewaard = 0;

// Laatst behandelde meldnummers, om herhalingen te herkennen
static uint16_t gezien[8];
static int      gezien_pos = 0;

/* ---------------- naar het scherm ---------------- */

static void naar_scherm(const char *onderwerp, JsonDocument &doc) {
    scherm.print(onderwerp);
    scherm.print(' ');
    serializeJson(doc, scherm);
    scherm.print('\n');
}

static void stuur_ack_nr(uint16_t nr) {
    JsonDocument doc;
    doc["nr"] = nr;
    naar_scherm(OND_ACK, doc);
}

static void stuur_ack_voor(const char *voor) {
    JsonDocument doc;
    doc["voor"] = voor;
    naar_scherm(OND_ACK, doc);
}

static void stuur_net_status(const char *reden = nullptr) {
    static const char *namen[] = { "uit", "bezig", "verbonden", "fout" };
    JsonDocument doc;
    doc["status"] = namen[net];
    if (reden) doc["reden"] = reden;
    naar_scherm(OND_NET_STATUS, doc);
}

static void zet_net(NetToestand nieuw, const char *reden = nullptr) {
    if (nieuw == net) return;
    net = nieuw;
    Serial.printf("[net] %d %s\n", net, reden ? reden : "");
    stuur_net_status(reden);
}

/* ---------------- naar MQTT ---------------- */

static void publiceer(const char *onderwerp, const char *json) {
    String volledig = String(MQTT_VOORVOEGSEL) + onderwerp;
    if (net == NET_VERBONDEN && mqtt.publish(volledig.c_str(), json)) return;
    if (strcmp(onderwerp, OND_MELD) != 0) return;   // status/temp komt vanzelf opnieuw
    if (aantal_bewaard == MAX_BEWAARD) {              // vol: oudste laten vallen
        for (int i = 1; i < MAX_BEWAARD; i++) bewaard[i - 1] = bewaard[i];
        aantal_bewaard--;
    }
    bewaard[aantal_bewaard++] = json;
}

static void bewaarde_meldingen_sturen() {
    String onderwerp = String(MQTT_VOORVOEGSEL) + OND_MELD;
    for (int i = 0; i < aantal_bewaard; i++) mqtt.publish(onderwerp.c_str(), bewaard[i].c_str());
    aantal_bewaard = 0;
}

/* Recept van de server: compact maken (één regel) en naar het scherm */
static void mqtt_ontvangen(char *onderwerp, byte *data, unsigned int len) {
    if (strcmp(onderwerp, MQTT_RECEPT) != 0) return;
    JsonDocument doc;
    if (deserializeJson(doc, data, len)) {
        Serial.println("[mqtt] recept is geen geldige JSON");
        return;
    }
    naar_scherm(OND_RECEPT, doc);
}

/* ---------------- verbinden ---------------- */

static void net_starten() {
    net_gewenst = true;
    verbind_start = millis();
    laatste_poging = 0;
    zet_net(NET_BEZIG);
    if (WiFi.status() != WL_CONNECTED) WiFi.begin(WIFI_NAAM, WIFI_WACHTWOORD);
}

static void net_stoppen() {
    net_gewenst = false;
    mqtt.disconnect();
    zet_net(NET_UIT);
}

static void net_bijhouden() {
    if (!net_gewenst) return;
    if (net == NET_VERBONDEN) {
        if (mqtt.connected()) { mqtt.loop(); return; }
        verbind_start = millis();                     // verbinding weg: opnieuw proberen
        zet_net(NET_BEZIG, "verbinding verloren");
    }
    if (net == NET_FOUT) return;                      // wacht op een nieuwe net/start

    if (millis() - verbind_start > VERBIND_MAX_MS) {
        zet_net(NET_FOUT, WiFi.status() != WL_CONNECTED ? "wifi niet gevonden" : "server onbereikbaar");
        net_gewenst = false;
        return;
    }
    if (WiFi.status() != WL_CONNECTED) return;
    if (millis() - laatste_poging < MQTT_POGING_MS) return;
    laatste_poging = millis();

    mqtt.setServer(server, poort);
    if (!mqtt.connect("niels-esp")) return;
    mqtt.subscribe(MQTT_RECEPT);
    zet_net(NET_VERBONDEN);
    bewaarde_meldingen_sturen();
    mqtt.publish(MQTT_RECEPT_VRAAG, "{}");
}

/* ---------------- berichten van het scherm ---------------- */

static bool al_gezien(uint16_t nr) {
    for (uint16_t g : gezien) if (g == nr) return true;
    gezien[gezien_pos] = nr;
    gezien_pos = (gezien_pos + 1) % 8;
    return false;
}

static void regel_verwerken(char *regel) {
    char *spatie = strchr(regel, ' ');
    const char *json = "{}";
    if (spatie) { *spatie = 0; json = spatie + 1; }
    const char *onderwerp = regel;

    JsonDocument doc;
    if (deserializeJson(doc, json)) return;    // geen geldige JSON: negeren

    if (strcmp(onderwerp, OND_STUUR) == 0) {
        vermogen = constrain((int)(doc["vermogen"] | 0), 0, 100);
        pomp = doc["pomp"] | false;
        zoemer = doc["zoemer"] | false;
        laatste_stuur = millis();
        ooit_stuur = true;
    } else if (strcmp(onderwerp, OND_STATUS) == 0 || strcmp(onderwerp, OND_RECEPT_ANTWOORD) == 0) {
        publiceer(onderwerp, json);
    } else if (strcmp(onderwerp, OND_MELD) == 0) {
        uint16_t nr = doc["nr"] | 0;
        if (!al_gezien(nr)) publiceer(onderwerp, json);
        stuur_ack_nr(nr);                     // ook bij een herhaling: het scherm wacht erop
    } else if (strcmp(onderwerp, OND_NET_START) == 0) {
        strlcpy(server, doc["server"] | "", sizeof server);
        poort = doc["poort"] | 1883;
        stuur_ack_voor(OND_NET_START);
        net_starten();
    } else if (strcmp(onderwerp, OND_NET_STOP) == 0) {
        stuur_ack_voor(OND_NET_STOP);
        net_stoppen();
    }
    // onbekend onderwerp: negeren
}

static void lees_kabel() {
    static char regel[NIELS_MAX_REGEL + 1];
    static size_t len = 0;
    static bool te_lang = false;
    while (scherm.available()) {
        char c = scherm.read();
        if (c == '\r') continue;
        if (c == '\n') {
            regel[len] = 0;
            if (!te_lang && len > 0) regel_verwerken(regel);
            len = 0;
            te_lang = false;
        } else if (len < NIELS_MAX_REGEL) {
            regel[len++] = c;
        } else {
            te_lang = true;
        }
    }
}

/* ---------------- ketel ---------------- */

static void uitgangen_bijwerken() {
    // Veiligheid: geen hartslag van het scherm = alles uit
    if (!ooit_stuur || millis() - laatste_stuur > NIELS_STIL_MAX_MS) {
        vermogen = 0;
        pomp = false;
        zoemer = false;
    }
    bool ssr_aan = (millis() % SSR_VENSTER_MS) < (uint32_t)vermogen * SSR_VENSTER_MS / 100;
    digitalWrite(PIN_SSR, ssr_aan ? HIGH : LOW);
    digitalWrite(PIN_POMP, pomp ? HIGH : LOW);
    digitalWrite(PIN_ZOEMER, zoemer ? HIGH : LOW);
}

static void temperatuur_sturen() {
    float t = sensor.getTempCByIndex(0);
    sensor.requestTemperatures();             // meting voor de volgende keer starten
    bool ok = t != DEVICE_DISCONNECTED_C && t > -50 && t < 150;

    JsonDocument doc;
    if (ok) doc["temp"] = serialized(String(t, 1));
    else    doc["temp"] = nullptr;
    doc["ok"] = ok;
    naar_scherm(OND_TEMP, doc);

    String json;
    serializeJson(doc, json);
    publiceer(OND_TEMP, json.c_str());
}

/* ---------------- setup / loop ---------------- */

void setup() {
    Serial.begin(115200);
    pinMode(PIN_SSR, OUTPUT);    digitalWrite(PIN_SSR, LOW);
    pinMode(PIN_POMP, OUTPUT);   digitalWrite(PIN_POMP, LOW);
    pinMode(PIN_ZOEMER, OUTPUT); digitalWrite(PIN_ZOEMER, LOW);

    scherm.setRxBufferSize(1024);
    scherm.begin(NIELS_BAUD, SERIAL_8N1, PIN_SCHERM_RX, PIN_SCHERM_TX);

    sensor.begin();
    sensor.setWaitForConversion(false);       // niet wachten: de loop blijft vlot
    sensor.requestTemperatures();

    WiFi.mode(WIFI_STA);
    mqtt.setBufferSize(1024);                 // het recept is een lang bericht
    mqtt.setCallback(mqtt_ontvangen);

    Serial.println("Niels ESP gestart");
    stuur_net_status();                       // scherm ziet zo dat we (weer) leven
}

void loop() {
    lees_kabel();
    uitgangen_bijwerken();
    net_bijhouden();

    static uint32_t vorige_temp = 0, vorige_net = 0;
    if (millis() - vorige_temp >= TEMP_MS) {
        vorige_temp = millis();
        temperatuur_sturen();
    }
    if (millis() - vorige_net >= NET_STATUS_MS) {
        vorige_net = millis();
        stuur_net_status();
    }
}
