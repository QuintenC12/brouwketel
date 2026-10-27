/*
 * hardware.cpp - koppeling met Niels ESP via I2C
 *
 * Sensor, SSR, pomp, zoemer en de verbinding met de server hangen allemaal
 * aan Niels ESP. Het scherm is I2C-master en Niels ESP slave op NIELS_ADRES.
 * De berichten staan beschreven in include/niels_i2c.h.
 *
 * hw_tick() (elke loop) stuurt om de 200 ms de gewenste toestand (vermogen,
 * pomp, zoemer) en leest de status terug. De andere functies geven alleen die
 * laatst gelezen waarden terug, of sturen meteen een bericht.
 */
#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include <string.h>
#include "ui/screens.h"
#include "hardware.h"
#include "brouwer.h"
#include "pid.h"
#include "niels_i2c.h"

#define TICK_MS          200    // zo vaak NB_STUUR + status
#define CONTACT_MAX_MS   2000   // langer geen status = Niels ESP weg -> sensorfout
#define VERBIND_MAX_MS   30000  // zo lang mag verbinden + recept ophalen duren

static Preferences prefs;

// Wat het scherm wil (gaat elke tick mee in NB_STUUR)
static uint8_t gewenst_vermogen = 0;
static bool    gewenst_pomp = false;
static bool    gewenst_zoemer = false;

// Wat Niels ESP het laatst meldde
static float    temp = -127;
static bool     sensor_ok = false;
static uint8_t  net = NET_UIT;
static uint32_t laatste_contact = 0;
static bool     getoond_online = false;

// Verbinden met de server: 0 = niet bezig, 1 = wacht op server,
// 2 = wacht op recept, 3 = klaar, even het laatste vinkje tonen
static int      verbind_fase = 0;
static uint32_t verbind_tijd = 0;

/* ---------------- I2C ---------------- */

static bool stuur(uint8_t soort, const void *inhoud, uint8_t n) {
    uint8_t buf[NIELS_MAX_BERICHT + 3];
    size_t len = niels_pak_in(buf, soort, inhoud, n);
    for (int poging = 0; poging < 3; poging++) {
        Wire.beginTransmission(NIELS_ADRES);
        Wire.write(buf, len);
        if (Wire.endTransmission() == 0) return true;
    }
    Serial.printf("[i2c] bericht 0x%02X niet aangekomen\n", soort);
    return false;
}

static bool lees(uint8_t soort, void *inhoud, uint8_t n) {
    uint8_t buf[NIELS_MAX_BERICHT + 3];
    size_t len = Wire.requestFrom((uint8_t)NIELS_ADRES, (uint8_t)(n + 3));
    if (len > sizeof buf) len = sizeof buf;
    for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
    if (!niels_pak_uit(buf, len, soort, n)) return false;
    memcpy(inhoud, buf + 2, n);
    return true;
}

/* Eén stuk van het recept ophalen: eerst kiezen, dan lezen */
static bool lees_deel(uint8_t deel, uint8_t soort, void *inhoud, uint8_t n) {
    NielsKiesDeel k = { deel };
    if (!stuur(NB_KIES_DEEL, &k, sizeof k)) return false;
    delay(2);   // Niels ESP de tijd geven om zijn antwoord klaar te zetten
    return lees(soort, inhoud, n);
}

static bool contact() { return millis() - laatste_contact < CONTACT_MAX_MS; }

/* ---------------- recept ---------------- */

static bool recept_ophalen() {
    NielsReceptKop kop;
    NielsReceptStappen st;
    if (!lees_deel(0, NB_RECEPT_KOP, &kop, sizeof kop)) return false;
    if (kop.aantal_stappen < 1 || kop.aantal_stappen > MAX_STAPPEN || kop.aantal_hops > MAX_HOPS) return false;
    if (!lees_deel(1, NB_RECEPT_STAPPEN, &st, sizeof st)) return false;
    NielsReceptHop hops[MAX_HOPS];
    for (int i = 0; i < kop.aantal_hops; i++)
        if (!lees_deel(2 + i, NB_RECEPT_HOP, &hops[i], sizeof hops[i])) return false;

    // Pas alles overnemen als alle stukken goed binnen zijn
    kop.naam[sizeof kop.naam - 1] = 0;
    strncpy(brouw.recept, kop.naam, sizeof brouw.recept - 1);
    brouw.recept[sizeof brouw.recept - 1] = 0;
    brouw.aantal_stappen = kop.aantal_stappen;
    for (int i = 0; i < kop.aantal_stappen; i++)
        brouw.stappen[i] = { st.stap[i].temp, st.stap[i].minuten, 0 };
    brouw.kooktijd_min = kop.kooktijd_min;
    brouw.aantal_hops = kop.aantal_hops;
    for (int i = 0; i < kop.aantal_hops; i++) {
        hops[i].naam[sizeof hops[i].naam - 1] = 0;
        strncpy(brouw.hops[i].naam, hops[i].naam, sizeof brouw.hops[i].naam - 1);
        brouw.hops[i].naam[sizeof brouw.hops[i].naam - 1] = 0;
        brouw.hops[i].minuten = hops[i].minuten;
    }
    ui_toon_schema();
    ui_selecteer_schema_rij(-1);
    Serial.printf("[net] recept ontvangen: %s\n", brouw.recept);
    return true;
}

/* Stap 3 op het verbindingsscherm: zijn de waarden zinnig? */
static bool recept_ok() {
    for (int i = 0; i < brouw.aantal_stappen; i++)
        if (brouw.stappen[i].temp < 20 || brouw.stappen[i].temp > 100 || brouw.stappen[i].minuten <= 0) return false;
    for (int i = 0; i < brouw.aantal_hops; i++)
        if (brouw.hops[i].minuten > brouw.kooktijd_min) return false;
    return brouw.kooktijd_min > 0;
}

static void verbinden_mislukt(int stap) {
    Serial.printf("[net] verbinden mislukt bij stap %d\n", stap);
    ui_verbind_stap(stap, VS_FOUT);
    verbind_fase = 0;
}

/* Verbindingsscherm volgen aan de hand van de status van Niels ESP */
static void verbinden_volgen() {
    uint32_t verstreken = millis() - verbind_tijd;
    if (verbind_fase == 3) {
        if (verstreken > 1000) { verbind_fase = 0; ui_ga_naar(SCREEN_ID_SCHEMA); }
        return;
    }
    if (verstreken > VERBIND_MAX_MS || (verstreken > 3000 && !contact())) {
        verbinden_mislukt(verbind_fase); return;
    }
    if (verstreken < 500) return;   // status kan nog van voor NB_VERBIND zijn
    if (net == NET_FOUT) { verbinden_mislukt(verbind_fase); return; }

    if (verbind_fase == 1 && (net == NET_VERBONDEN || net == NET_RECEPT_KLAAR)) {
        ui_verbind_stap(1, VS_KLAAR);
        ui_verbind_stap(2, VS_BEZIG);
        verbind_fase = 2;
    }
    if (verbind_fase == 2 && net == NET_RECEPT_KLAAR) {
        if (!recept_ophalen()) { verbinden_mislukt(2); return; }
        ui_verbind_stap(2, VS_KLAAR);
        ui_verbind_stap(3, VS_BEZIG);
        if (!recept_ok()) { verbinden_mislukt(3); return; }
        ui_verbind_stap(3, VS_KLAAR);
        verbind_fase = 3;
        verbind_tijd = millis();
    }
}

/* ---------------- hw_* ---------------- */

void hw_init() {
    // Wire.begin() gebeurt in board_init(): de touch zit op dezelfde bus
}

void hw_tick() {
    static uint32_t vorige = 0;
    static bool eerste = true;
    if (millis() - vorige < TICK_MS) return;
    vorige = millis();
    if (eerste) {
        Wire.setTimeOut(20);   // een hangende Niels ESP mag het scherm niet bevriezen
        eerste = false;
    }

    NielsStuur s = { gewenst_vermogen, gewenst_pomp, gewenst_zoemer };
    stuur(NB_STUUR, &s, sizeof s);

    NielsStatus st;
    if (lees(NB_STATUS, &st, sizeof st)) {
        laatste_contact = millis();
        temp = st.temp_x10 / 10.0f;
        sensor_ok = st.sensor_ok;
        net = st.net;
    }

    bool online = contact() && (net == NET_VERBONDEN || net == NET_RECEPT_KLAAR);
    if (online != getoond_online) {
        getoond_online = online;
        ui_toon_verbinding(online);
    }
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
    NielsStuur s = { gewenst_vermogen, gewenst_pomp, gewenst_zoemer };
    stuur(NB_STUUR, &s, sizeof s);
}

void hw_verwarming_uit() {
    Serial.println("[hw] verwarming UIT");
    pid_reset();
    gewenst_vermogen = 0;
    NielsStuur s = { gewenst_vermogen, gewenst_pomp, gewenst_zoemer };
    stuur(NB_STUUR, &s, sizeof s);
}

void hw_zoemer(bool aan) {
    Serial.printf("[hw] zoemer %s\n", aan ? "AAN" : "UIT");
    gewenst_zoemer = aan;
    NielsStuur s = { gewenst_vermogen, gewenst_pomp, gewenst_zoemer };
    stuur(NB_STUUR, &s, sizeof s);
}

bool hw_sensor_ok() {
    return contact() && sensor_ok;
}

/* ---------------- net_* (Niels ESP praat met de server) ---------------- */

void net_verbind_server() {
    Serial.printf("[net] verbinden met %s:%u\n", brouw.server_ip, brouw.server_poort);
    NielsVerbind v = {};
    strncpy(v.server_ip, brouw.server_ip, sizeof v.server_ip - 1);
    v.server_poort = brouw.server_poort;
    verbind_tijd = millis();
    if (!stuur(NB_VERBIND, &v, sizeof v)) { verbinden_mislukt(1); return; }
    verbind_fase = 1;
}

void net_verbreek() {
    Serial.println("[net] verbinding verbroken");
    verbind_fase = 0;
    stuur(NB_VERBREEK, nullptr, 0);
}

void net_meld(const char *gebeurtenis) {
    Serial.printf("[net] melding: %s\n", gebeurtenis);
    NielsMeld m = {};
    strncpy(m.tekst, gebeurtenis, sizeof m.tekst - 1);
    stuur(NB_MELD, &m, sizeof m);
}

void net_meld_sg(int stap, float sg) {
    Serial.printf("[net] SG stap %d: %.3f\n", stap + 1, sg);
    NielsMeldSG m = { (uint8_t)stap, (uint16_t)(sg * 1000 + 0.5f) };
    stuur(NB_MELD_SG, &m, sizeof m);
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
