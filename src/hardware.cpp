/*
 * hardware.cpp - koppeling met Niels ESP via een seriële kabel (UART)
 *
 * Sensor, SSR, pomp, zoemer en de verbinding met de MQTT-server hangen
 * allemaal aan Niels ESP. Beide kanten sturen elkaar tekstregels:
 *     onderwerp {json}
 * De onderwerpen staan in include/niels_uart.h, het regelboekje in het project.
 *
 * hw_tick() (elke loop) leest binnenkomende regels, stuurt om de 200 ms
 * brouw/stuur en elke seconde brouw/status, en stuurt meldingen opnieuw
 * als er geen ack komt. De andere functies geven de laatst ontvangen
 * waarden terug, of sturen meteen een bericht.
 */
#include <Arduino.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <string.h>
#include "ui/screens.h"
#include "hardware.h"
#include "brouwer.h"
#include "pid.h"
#include "niels_uart.h"

// TODO: vul de twee vrije pinnen van het CrowPanel in.
//       TX van het scherm gaat naar RX van Niels ESP, RX naar TX. Zolang
//       hier -1 staat, praat het scherm niet met Niels ESP.
#define PIN_NIELS_TX  -1
#define PIN_NIELS_RX  -1

#define STUUR_MS         200    // zo vaak brouw/stuur (ook de hartslag)
#define STATUS_MS        1000   // zo vaak brouw/status voor de webapp
#define MELD_HERHAAL_MS  500    // zo lang wachten op een ack
#define MELD_POGINGEN    3
#define VERBIND_MAX_MS   30000  // zo lang mag verbinden + recept ophalen duren

static Preferences prefs;
static HardwareSerial niels(1);
static bool kabel_ok = false;     // pinnen ingevuld en poort gestart

// Wat het scherm wil (gaat mee in elke brouw/stuur)
static uint8_t gewenst_vermogen = 0;
static bool    gewenst_pomp = false;
static bool    gewenst_zoemer = false;

// Wat Niels ESP het laatst meldde
static float    temp = -127;
static bool     sensor_ok = false;
static bool     ooit_temp = false;
static uint32_t laatste_temp = 0;
static bool     online = false;

// Verbinden met de server: 0 = niet bezig, 1 = wacht op "verbonden",
// 2 = wacht op het recept, 3 = klaar, even het laatste vinkje tonen
static int      verbind_fase = 0;
static uint32_t verbind_tijd = 0;

// Meldingen die nog op een ack wachten
struct Melding {
    uint16_t nr;
    char     regel[160];
    uint32_t verstuurd;
    uint8_t  pogingen;
};
#define MAX_WACHTEND 8
static Melding  wachtend[MAX_WACHTEND];
static int      aantal_wachtend = 0;
static uint16_t volgnummer = 0;

/* ---------------- versturen ---------------- */

static void stuur(const char *onderwerp, JsonDocument &doc) {
    if (!kabel_ok) return;
    niels.print(onderwerp);
    niels.print(' ');
    serializeJson(doc, niels);
    niels.print('\n');
}

static void stuur_stuur() {
    JsonDocument doc;
    doc["vermogen"] = gewenst_vermogen;
    doc["pomp"] = gewenst_pomp;
    doc["zoemer"] = gewenst_zoemer;
    stuur(OND_STUUR, doc);
}

static const char *fase_naam(Fase f) {
    switch (f) {
    case FASE_MAISCHEN: return "maischen";
    case FASE_SPOELEN:  return "spoelen";
    case FASE_KOKEN:    return "koken";
    default:            return "geen";
    }
}

static void stuur_status() {
    JsonDocument doc;
    doc["fase"] = fase_naam(brouw.fase);
    doc["stap"] = brouw.fase == FASE_MAISCHEN ? brouw.actieve_stap + 1 : 0;
    doc["doel"] = brouwer_doel_temp();
    doc["rest"] = brouw.resterend_s;
    doc["pauze"] = brouw.gepauzeerd;
    stuur(OND_STATUS, doc);
}

/* Melding versturen en bewaren tot de ack komt */
static void stuur_melding(JsonDocument &doc) {
    if (!kabel_ok) return;
    if (aantal_wachtend == MAX_WACHTEND) {                 // vol: oudste laten vallen
        Serial.printf("[uart] melding %u kreeg geen ack\n", wachtend[0].nr);
        memmove(&wachtend[0], &wachtend[1], sizeof(Melding) * (MAX_WACHTEND - 1));
        aantal_wachtend--;
    }
    Melding &m = wachtend[aantal_wachtend++];
    m.nr = doc["nr"];
    int n = snprintf(m.regel, sizeof m.regel, "%s ", OND_MELD);
    serializeJson(doc, m.regel + n, sizeof m.regel - n);
    m.verstuurd = millis();
    m.pogingen = 1;
    niels.print(m.regel);
    niels.print('\n');
}

static void meldingen_herhalen() {
    for (int i = 0; i < aantal_wachtend; ) {
        Melding &m = wachtend[i];
        if (millis() - m.verstuurd < MELD_HERHAAL_MS) { i++; continue; }
        if (m.pogingen >= MELD_POGINGEN) {
            Serial.printf("[uart] melding %u kreeg geen ack\n", m.nr);
            memmove(&wachtend[i], &wachtend[i + 1], sizeof(Melding) * (aantal_wachtend - i - 1));
            aantal_wachtend--;
            continue;
        }
        niels.print(m.regel);
        niels.print('\n');
        m.verstuurd = millis();
        m.pogingen++;
        i++;
    }
}

static void ack_ontvangen(uint16_t nr) {
    for (int i = 0; i < aantal_wachtend; i++) {
        if (wachtend[i].nr != nr) continue;
        memmove(&wachtend[i], &wachtend[i + 1], sizeof(Melding) * (aantal_wachtend - i - 1));
        aantal_wachtend--;
        return;
    }
}

/* ---------------- verbinden en recept ---------------- */

static bool contact() { return ooit_temp && millis() - laatste_temp < NIELS_STIL_MAX_MS; }

static void verbinden_mislukt(int stap) {
    Serial.printf("[net] verbinden mislukt bij stap %d\n", stap);
    ui_verbind_stap(stap, VS_FOUT);
    verbind_fase = 0;
}

/* Recept uit de JSON halen en controleren. Geeft de reden terug als het
 * niet klopt, anders nullptr. brouw wordt pas aangepast als alles klopt. */
static const char *recept_overnemen(JsonDocument &doc) {
    JsonArrayConst stappen = doc["stappen"];
    JsonArrayConst hops = doc["hops"];
    int kooktijd = doc["kooktijd"] | 0;
    if (stappen.size() < 1 || stappen.size() > MAX_STAPPEN) return "aantal stappen";
    if (hops.size() > MAX_HOPS) return "te veel hopgiften";
    if (kooktijd <= 0) return "kooktijd";

    MaischStap st[MAX_STAPPEN];
    int i = 0;
    for (JsonObjectConst s : stappen) {
        st[i] = { s["temp"] | 0, s["min"] | 0, 0 };
        if (st[i].temp < 20 || st[i].temp > 100) return "temperatuur van een stap";
        if (st[i].minuten <= 0) return "minuten van een stap";
        i++;
    }
    HopGift hp[MAX_HOPS];
    i = 0;
    for (JsonObjectConst h : hops) {
        strlcpy(hp[i].naam, h["naam"] | "", sizeof hp[i].naam);
        hp[i].minuten = h["min"] | -1;
        if (hp[i].minuten < 0 || hp[i].minuten > kooktijd) return "tijd van een hopgift";
        i++;
    }

    strlcpy(brouw.recept, doc["naam"] | "Recept", sizeof brouw.recept);
    brouw.aantal_stappen = stappen.size();
    memcpy(brouw.stappen, st, sizeof(MaischStap) * brouw.aantal_stappen);
    brouw.aantal_hops = hops.size();
    memcpy(brouw.hops, hp, sizeof(HopGift) * brouw.aantal_hops);
    brouw.kooktijd_min = kooktijd;
    return nullptr;
}

static void recept_ontvangen(JsonDocument &doc) {
    if (verbind_fase != 1 && verbind_fase != 2) return;   // niet om gevraagd
    ui_verbind_stap(1, VS_KLAAR);
    ui_verbind_stap(2, VS_KLAAR);
    ui_verbind_stap(3, VS_BEZIG);

    const char *reden = recept_overnemen(doc);
    JsonDocument antwoord;
    antwoord["ok"] = reden == nullptr;
    if (reden) antwoord["reden"] = reden;
    stuur(OND_RECEPT_ANTWOORD, antwoord);

    if (reden) {
        Serial.printf("[net] recept geweigerd: %s\n", reden);
        verbinden_mislukt(3);
        return;
    }
    Serial.printf("[net] recept ontvangen: %s\n", brouw.recept);
    ui_toon_schema();
    ui_selecteer_schema_rij(-1);
    ui_verbind_stap(3, VS_KLAAR);
    verbind_fase = 3;
    verbind_tijd = millis();
}

static void net_status_ontvangen(const char *status) {
    bool nu_online = strcmp(status, "verbonden") == 0;
    if (nu_online != online) {
        online = nu_online;
        ui_toon_verbinding(online);
    }
    if (verbind_fase == 1 && nu_online) {
        ui_verbind_stap(1, VS_KLAAR);
        ui_verbind_stap(2, VS_BEZIG);
        verbind_fase = 2;
    } else if ((verbind_fase == 1 || verbind_fase == 2) && strcmp(status, "fout") == 0) {
        verbinden_mislukt(verbind_fase);
    }
}

static void verbinden_volgen() {
    uint32_t verstreken = millis() - verbind_tijd;
    if (verbind_fase == 3) {
        if (verstreken > 1000) { verbind_fase = 0; ui_ga_naar(SCREEN_ID_SCHEMA); }
        return;
    }
    if (verstreken > VERBIND_MAX_MS || (verstreken > 3000 && !contact())) {
        verbinden_mislukt(verbind_fase);
        JsonDocument leeg;
        leeg.to<JsonObject>();
        stuur(OND_NET_STOP, leeg);
    }
}

/* ---------------- ontvangen ---------------- */

static void regel_verwerken(char *regel) {
    char *spatie = strchr(regel, ' ');
    const char *json = "{}";
    if (spatie) { *spatie = 0; json = spatie + 1; }
    const char *onderwerp = regel;

    JsonDocument doc;
    if (deserializeJson(doc, json)) return;    // geen geldige JSON: negeren

    if (strcmp(onderwerp, OND_TEMP) == 0) {
        sensor_ok = doc["ok"] | false;
        temp = doc["temp"] | -127.0f;
        laatste_temp = millis();
        ooit_temp = true;
    } else if (strcmp(onderwerp, OND_NET_STATUS) == 0) {
        net_status_ontvangen(doc["status"] | "");
    } else if (strcmp(onderwerp, OND_RECEPT) == 0) {
        recept_ontvangen(doc);
    } else if (strcmp(onderwerp, OND_ACK) == 0) {
        if (doc["nr"].is<int>()) ack_ontvangen(doc["nr"]);
    }
    // onbekend onderwerp: negeren
}

static void lees_kabel() {
    static char regel[NIELS_MAX_REGEL + 1];
    static size_t len = 0;
    static bool te_lang = false;
    while (niels.available()) {
        char c = niels.read();
        if (c == '\r') continue;
        if (c == '\n') {
            regel[len] = 0;
            if (!te_lang && len > 0) regel_verwerken(regel);
            len = 0;
            te_lang = false;
        } else if (len < NIELS_MAX_REGEL) {
            regel[len++] = c;
        } else {
            te_lang = true;    // rest van de regel weggooien
        }
    }
}

/* ---------------- hw_* ---------------- */

void hw_init() {
    if (PIN_NIELS_TX < 0 || PIN_NIELS_RX < 0) {
        Serial.println("[uart] PIN_NIELS_TX/RX niet ingevuld in hardware.cpp - geen verbinding met Niels ESP");
        return;
    }
    niels.setRxBufferSize(1024);
    niels.begin(NIELS_BAUD, SERIAL_8N1, PIN_NIELS_RX, PIN_NIELS_TX);
    kabel_ok = true;
}

void hw_tick() {
    if (!kabel_ok) return;
    lees_kabel();

    static uint32_t vorige_stuur = 0, vorige_status = 0;
    if (millis() - vorige_stuur >= STUUR_MS) {
        vorige_stuur = millis();
        stuur_stuur();
    }
    if (millis() - vorige_status >= STATUS_MS) {
        vorige_status = millis();
        stuur_status();
    }
    meldingen_herhalen();
    if (verbind_fase) verbinden_volgen();
}

float hw_lees_temperatuur() {
    return hw_sensor_ok() ? temp : -127.0f;
}

int hw_regel_verwarming(float t, float doel) {
    // PID-regelaar (pid.cpp) met de waarden uit Instellingen. Niels ESP zet
    // dit percentage om in een venster van 2 s waarin de SSR zoveel % aan staat.
    int v = t <= -100 ? 0 : pid_bereken(t, doel, 1.0f);
    gewenst_vermogen = v;
    return v;
}

void hw_pomp(bool aan) {
    Serial.printf("[hw] pomp %s\n", aan ? "AAN" : "UIT");
    gewenst_pomp = aan;
    stuur_stuur();
}

void hw_verwarming_uit() {
    Serial.println("[hw] verwarming UIT");
    pid_reset();
    gewenst_vermogen = 0;
    stuur_stuur();
}

void hw_zoemer(bool aan) {
    Serial.printf("[hw] zoemer %s\n", aan ? "AAN" : "UIT");
    gewenst_zoemer = aan;
    stuur_stuur();
}

bool hw_sensor_ok() {
    return contact() && sensor_ok;
}

/* ---------------- net_* (Niels ESP praat met de server) ---------------- */

void net_verbind_server() {
    Serial.printf("[net] verbinden met %s:%u\n", brouw.server_ip, brouw.server_poort);
    if (!kabel_ok) { verbinden_mislukt(1); return; }
    JsonDocument doc;
    doc["server"] = brouw.server_ip;
    doc["poort"] = brouw.server_poort;
    stuur(OND_NET_START, doc);
    verbind_fase = 1;
    verbind_tijd = millis();
}

void net_verbreek() {
    Serial.println("[net] verbinding verbroken");
    verbind_fase = 0;
    JsonDocument leeg;
    leeg.to<JsonObject>();
    stuur(OND_NET_STOP, leeg);
}

void net_meld(const char *gebeurtenis) {
    Serial.printf("[net] melding: %s\n", gebeurtenis);
    JsonDocument doc;
    doc["nr"] = ++volgnummer;
    doc["naam"] = gebeurtenis;
    // extra gegevens voor de webapp, zie hoofdstuk 4 van het regelboekje
    if (strcmp(gebeurtenis, "volgende_stap") == 0) {
        doc["stap"] = brouw.actieve_stap + 1;
    } else if (strcmp(gebeurtenis, "hop_alarm") == 0 && brouw.hop_volgende < brouw.aantal_hops) {
        doc["hop"] = brouw.hops[brouw.hop_volgende].naam;
    } else if (strcmp(gebeurtenis, "hop_toegevoegd") == 0 && brouw.hop_volgende > 0) {
        doc["hop"] = brouw.hops[brouw.hop_volgende - 1].naam;   // hop_volgende is al opgeschoven
    }
    stuur_melding(doc);
}

void net_meld_sg(int stap, float sg) {
    Serial.printf("[net] SG stap %d: %.3f\n", stap + 1, sg);
    JsonDocument doc;
    doc["nr"] = ++volgnummer;
    doc["naam"] = "sg";
    doc["stap"] = stap + 1;
    doc["sg"] = serialized(String(sg, 3));
    stuur_melding(doc);
}

void instellingen_opslaan() {
    prefs.begin("brouwer", false);
    prefs.putString("ip", brouw.server_ip);
    prefs.putUShort("poort", brouw.server_poort);
    prefs.putFloat("kp", brouw.pid_kp);
    prefs.putFloat("ki", brouw.pid_ki);
    prefs.putFloat("kd", brouw.pid_kd);
    prefs.end();
    Serial.println("[cfg] instellingen opgeslagen");
}

void instellingen_laden() {
    prefs.begin("brouwer", true);
    String ip = prefs.getString("ip", brouw.server_ip);
    brouw.server_poort = prefs.getUShort("poort", brouw.server_poort);
    brouw.pid_kp = prefs.getFloat("kp", brouw.pid_kp);
    brouw.pid_ki = prefs.getFloat("ki", brouw.pid_ki);
    brouw.pid_kd = prefs.getFloat("kd", brouw.pid_kd);
    prefs.end();
    strncpy(brouw.server_ip, ip.c_str(), sizeof(brouw.server_ip) - 1);
}
