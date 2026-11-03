/*
 * instellingen.cpp - IP, poort en PID-waarden bewaren in het flashgeheugen
 *
 * Gebruikt Preferences van Arduino-ESP32: blijft bewaard als de stroom uitvalt.
 * De simulator en de demo hebben geen echte opslag; daar staan deze twee
 * functies in sim/sim_hardware.cpp.
 */
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>
#include "hardware.h"
#include "brouwer.h"

static Preferences prefs;

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
