/*
 * hardware.cpp - voorlopig enkel stubs die naar de seriële monitor schrijven.
 * Vervang de TODO's door je echte code (pinnen, sensor, wifi/server, opslag).
 */
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>
#include "hardware.h"
#include "brouwer.h"
#include "pid.h"

// TODO: pas aan aan je bedrading
#define PIN_POMP    -1
#define PIN_ZOEMER  -1

static Preferences prefs;

void hw_init() {
    if (PIN_POMP >= 0)   { pinMode(PIN_POMP, OUTPUT);   digitalWrite(PIN_POMP, LOW); }
    if (PIN_ZOEMER >= 0) { pinMode(PIN_ZOEMER, OUTPUT); digitalWrite(PIN_ZOEMER, LOW); }
    // TODO: verwarming (SSR) en temperatuursensor initialiseren
}

float hw_lees_temperatuur() {
    // TODO: temperatuursensor uitlezen (bijv. DS18B20 via DallasTemperature)
    return 20.0f;
}

int hw_regel_verwarming(float temp, float doel) {
    // PID-regelaar (pid.cpp) met de waarden uit Instellingen.
    // TODO: SSR aansturen met dit percentage, bijv. een venster van 2 s
    //       waarin de SSR vermogen% van de tijd aan staat.
    return pid_bereken(temp, doel, 1.0f);
}

void hw_pomp(bool aan) {
    Serial.printf("[hw] pomp %s\n", aan ? "AAN" : "UIT");
    if (PIN_POMP >= 0) digitalWrite(PIN_POMP, aan ? HIGH : LOW);
}

void hw_verwarming_uit() {
    Serial.println("[hw] verwarming UIT");
    pid_reset();
    // TODO: SSR uitzetten en regelaar stoppen
}

void hw_zoemer(bool aan) {
    Serial.printf("[hw] zoemer %s\n", aan ? "AAN" : "UIT");
    if (PIN_ZOEMER >= 0) digitalWrite(PIN_ZOEMER, aan ? HIGH : LOW);
}

bool hw_sensor_ok() {
    // TODO: sensor opnieuw uitlezen (een DS18B20 geeft -127 bij een fout)
    return false;
}

void net_verbind_server() {
    Serial.printf("[net] verbinden met %s:%u\n", brouw.server_ip, brouw.server_poort);
    // TODO: verbinding opzetten. Zie sim/sim_hardware.cpp voor de volgorde:
    //   ui_verbind_stap(1, VS_KLAAR); ui_verbind_stap(2, VS_BEZIG);   // server bereikt
    //   recept invullen: brouw.stappen / aantal_stappen / hops / aantal_hops /
    //                    kooktijd_min / recept, daarna ui_toon_schema();
    //   ui_verbind_stap(2, VS_KLAAR); ... ui_verbind_stap(3, VS_KLAAR);
    //   ui_ga_naar(SCREEN_ID_SCHEMA);
    // Mislukt? ui_verbind_stap(1, VS_FOUT);
}

void net_verbreek() {
    Serial.println("[net] verbinding verbroken");
}

void net_meld(const char *gebeurtenis) {
    Serial.printf("[net] melding: %s\n", gebeurtenis);
}

void net_meld_sg(int stap, float sg) {
    Serial.printf("[net] SG stap %d: %.3f\n", stap + 1, sg);
    // TODO: naar de server sturen zodat het in de database bij de brouwsessie komt
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
