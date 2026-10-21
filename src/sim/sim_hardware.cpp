/*
 * sim_hardware.cpp - vervangt hardware.cpp in de simulator
 *
 * Speelt alleen de buitenwereld na:
 *  - een ketel die opwarmt naar de doeltemperatuur en daarna stabiel blijft
 *  - een server die na "Automatisch" verbindt en een demo-recept stuurt
 * Het brouwproces zelf (timers, stappen, hopalarmen) is de echte code uit
 * brouwproces.cpp, net zoals op de ESP32.
 */
#include <stdio.h>
#include <string.h>
#include <lvgl.h>
#include "ui/screens.h"
#include "brouwer.h"
#include "hardware.h"

/* ---------------- nep-ketel ---------------- */

static float ketel = 18.4f;

float hw_lees_temperatuur() { return ketel; }

/* Eén gesimuleerde seconde: opwarmen met 0,15 °C/s, daarna vasthouden */
int hw_regel_verwarming(float temp, float doel) {
    if (doel <= 0) {                       // verwarming uit: langzaam afkoelen
        if (ketel > 18.4f) ketel -= 0.02f;
        return 0;
    }
    if (temp < doel - 0.2f) { ketel += 0.15f; return 100; }
    ketel = doel >= 100 ? 99.9f : doel - 0.1f;
    return doel >= 100 ? 85 : 20;
}

void hw_init()                 { printf("[sim] gestart\n"); }
void hw_pomp(bool aan)         { printf("[hw] pomp %s\n", aan ? "AAN" : "UIT"); }
void hw_verwarming_uit()       { }
void hw_zoemer(bool aan)       { printf("[hw] zoemer %s\n", aan ? "AAN" : "UIT"); }
bool hw_sensor_ok()            { return true; }
void instellingen_opslaan()    { printf("[cfg] opgeslagen: %s:%u  PID %.2f / %.3f / %.2f\n", brouw.server_ip, brouw.server_poort, brouw.pid_kp, brouw.pid_ki, brouw.pid_kd); }
void instellingen_laden()      {}
void net_meld(const char *g)   { printf("[net] melding: %s\n", g); }
void net_meld_sg(int stap, float sg) { printf("[net] SG stap %d: %.3f\n", stap + 1, sg); }

/* ---------------- nep-server ---------------- */

static int verbind_fase = 0;      // 0 = niet bezig, 1..4 = volgende stap in de reeks
static uint32_t verbind_tijd = 0;

void net_verbind_server() {
    printf("[net] verbinden met %s:%u\n", brouw.server_ip, brouw.server_poort);
    verbind_fase = 1;
    verbind_tijd = lv_tick_get();
}

void net_verbreek() {
    printf("[net] verbinding verbroken\n");
    verbind_fase = 0;
}

static void demo_recept() {
    strcpy(brouw.recept, "Demo Tripel - 20 L");
    brouw.aantal_stappen = 4;
    brouw.stappen[0] = { 52, 5, 0 };
    brouw.stappen[1] = { 63, 5, 0 };
    brouw.stappen[2] = { 72, 5, 0 };
    brouw.stappen[3] = { 78, 5, 0 };
    brouw.kooktijd_min = 40;
    brouw.aantal_hops = 3;
    brouw.hops[0] = { "Magnum - 25 g", 30 };
    brouw.hops[1] = { "Cascade - 30 g", 20 };
    brouw.hops[2] = { "Saaz - 20 g", 10 };
    ui_toon_schema();
    ui_selecteer_schema_rij(-1);
    printf("[net] recept ontvangen\n");
}

/* Verbindingsreeks: elke stap draait even en krijgt dan een vinkje.
 * Tijden in ms sinds "Automatisch" werd ingedrukt. */
void sim_netwerk() {
    if (!verbind_fase) return;
    uint32_t verstreken = lv_tick_get() - verbind_tijd;
    if (verbind_fase == 1 && verstreken > 1500) {          // server bereikt
        ui_verbind_stap(1, VS_KLAAR);
        ui_verbind_stap(2, VS_BEZIG);
        ui_toon_verbinding(true);
        verbind_fase = 2;
    } else if (verbind_fase == 2 && verstreken > 4500) {   // recept ontvangen
        demo_recept();
        ui_verbind_stap(2, VS_KLAAR);
        ui_verbind_stap(3, VS_BEZIG);
        verbind_fase = 3;
    } else if (verbind_fase == 3 && verstreken > 6000) {   // recept gecontroleerd
        ui_verbind_stap(3, VS_KLAAR);
        verbind_fase = 4;
    } else if (verbind_fase == 4 && verstreken > 7000) {   // even het laatste vinkje tonen
        verbind_fase = 0;
        ui_ga_naar(SCREEN_ID_SCHEMA);
    }
}
