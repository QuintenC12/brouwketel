/*
 * brouwproces.cpp - het eigenlijke brouwproces
 *
 * brouwer_seconde() wordt elke seconde aangeroepen (op de ESP32 vanuit
 * main.cpp, in de simulator vanuit sim/sim_main.cpp). Het telt timers af,
 * gaat naar de volgende maischstap, geeft hopalarmen en werkt het scherm bij.
 *
 * De timer van een stap start pas als de doeltemperatuur bereikt is.
 *
 * Hier staat ook de gedeelde toestand (brouw) en brouwer_init().
 */
#include <string.h>
#include "ui/screens.h"
#include "brouwer.h"
#include "hardware.h"

#define MARGE_MAISCHEN 0.5f   // timer start vanaf doel - 0,5 °C
#define KOOKPUNT       98.5f  // vanaf hier telt het koken als "aan de kook"

/* Standaard PID-waarden (pas aan in Instellingen) */
#define PID_KP 20.0f
#define PID_KI 0.05f
#define PID_KD 10.0f

BrouwStatus brouw;  //het heilige brouwproces

void brouwer_init() {
    memset(&brouw, 0, sizeof brouw);    //alles op 0 zetten
    strcpy(brouw.recept, "Geen recept geladen");
    strcpy(brouw.server_ip, "192.168.0.25");
    brouw.server_poort = 1200;
    brouw.kooktijd_min = MAN_KOOKTIJD_MIN;
    brouw.pid_kp = PID_KP;
    brouw.pid_ki = PID_KI;
    brouw.pid_kd = PID_KD;
    instellingen_laden();   // overschrijft ip/poort/PID als er iets opgeslagen is

    ui_init_extra();            //spinners, hopbalkjes en streepjes uit EEZ koppelen
    ui_toon_schema();           //lege lijsten en rijen verbergen
    ui_selecteer_schema_rij(-1);    //geen rij selecteren
    ui_selecteer_pauze_rij(-1);
    ui_toon_pomp();     //zet pomp op rood UIT
    ui_toon_verbinding(false);  //zeg geen verbinding
    ui_toon_hop_alarm(false);   //alarm pop-up niet laten zien
}

float brouwer_doel_temp() {
    if (brouw.sensor_fout) return 0;   // verwarming uit tot de fout bevestigd is
    switch (brouw.fase) {
    case FASE_MAISCHEN:
        return brouw.actieve_stap < brouw.aantal_stappen ? brouw.stappen[brouw.actieve_stap].temp : 0;
    case FASE_KOKEN:   return 100;
    default:           return 0;   // spoelen / niets: verwarming uit
    }
}

void brouwer_start_fase(Fase f) {
    brouw.fase = f;
    brouw.sensor_fout = false;
    brouw.op_temp = false;
    brouw.gepauzeerd = false;
    brouw.bezig = (f != FASE_GEEN);

    switch (f) {
    case FASE_MAISCHEN:
        brouw.actieve_stap = 0;
        brouw.resterend_s = brouw.stappen[0].minuten * 60;
        brouw.pomp_aan = true;      // tijdens het maischen moet de pomp draaien
        hw_pomp(true);              // naar Niels ESP sturen
        ui_toon_pomp();             // de pompknop in de zijbalk groen maken
        ui_toon_schema();
        break;
    case FASE_KOKEN:
        brouw.resterend_s = brouw.kooktijd_min * 60;
        brouw.hop_volgende = 0;
        brouw.hop_alarm_actief = false;
        ui_toon_hop_alarm(false);
        ui_toon_koken();
        break;
    case FASE_SPOELEN:
        ui_selecteer_sg_rij(-1);   // SG-knop wijst nu naar de laatste stap
        hw_verwarming_uit();
        brouw.pomp_aan = false;      // na maishen pomp uit
        hw_pomp(false);              // naar Niels ESP sturen
        ui_toon_pomp();             // de pompknop in de zijbalk rood maken
        break;
    case FASE_GEEN:
        hw_verwarming_uit();
        break;
    }
}

static void koken_klaar() {
    net_meld("koken_klaar");
    hw_zoemer(ZOEMER_MELDING);   // stopt bij de eerste knop op het startscherm
    brouwer_start_fase(FASE_GEEN);
    ui_ga_naar(SCREEN_ID_MAIN);
}

/* Recept controleren en overnemen (bijv. van de server via Niels ESP) */
const char *brouwer_recept_overnemen(const Recept &r) {
    if (r.aantal_stappen < 1 || r.aantal_stappen > MAX_STAPPEN) return "aantal stappen";
    if (r.aantal_hops > MAX_HOPS) return "te veel hopgiften";
    if (r.kooktijd_min <= 0) return "kooktijd";
    for (int i = 0; i < r.aantal_stappen; i++) {
        if (r.stappen[i].temp < TEMP_MIN || r.stappen[i].temp > TEMP_MAX) return "temperatuur van een stap";
        if (r.stappen[i].minuten <= 0) return "minuten van een stap";
    }
    for (int i = 0; i < r.aantal_hops; i++)
        if (r.hops[i].minuten < 0 || r.hops[i].minuten > r.kooktijd_min) return "tijd van een hopgift";

    strcpy(brouw.recept, r.naam);
    memcpy(brouw.stappen, r.stappen, sizeof(MaischStap) * r.aantal_stappen);
    brouw.aantal_stappen = r.aantal_stappen;
    memcpy(brouw.hops, r.hops, sizeof(HopGift) * r.aantal_hops);
    brouw.aantal_hops = r.aantal_hops;
    brouw.kooktijd_min = r.kooktijd_min;
    return nullptr;
}

void brouwer_seconde(float temp, int vermogen) {
    // Sensorfout: verwarming uit en foutscherm (één keer)
    if (temp <= -100) {
        if (brouw.bezig && !brouw.sensor_fout) {
            brouw.sensor_fout = true;
            hw_verwarming_uit();
            hw_zoemer(ZOEMER_ALARM);
            net_meld("sensorfout");
            ui_toon_sensorfout(temp);
        }
        return;
    }
    // Sensor werkt weer: toch wachten tot iemand "Opnieuw proberen" drukt
    if (brouw.sensor_fout) return;

    float doel = brouwer_doel_temp();
    ui_toon_meting(temp, doel, vermogen);

    if (brouw.fase != FASE_MAISCHEN && brouw.fase != FASE_KOKEN) return;

    // Doeltemperatuur bereikt? Dan begint de timer van deze stap te lopen.
    float drempel = brouw.fase == FASE_KOKEN ? KOOKPUNT : doel - MARGE_MAISCHEN;
    if (!brouw.op_temp && temp >= drempel) brouw.op_temp = true;

    if (brouw.op_temp && !brouw.gepauzeerd && brouw.resterend_s > 0) brouw.resterend_s--;

    switch (brouw.fase) {
    case FASE_MAISCHEN:
        if (brouw.op_temp && brouw.resterend_s == 0) {
            brouw.actieve_stap++;
            if (brouw.actieve_stap < brouw.aantal_stappen) {
                brouw.resterend_s = brouw.stappen[brouw.actieve_stap].minuten * 60;
                brouw.op_temp = false;
                net_meld("volgende_stap");
            } else {
                net_meld("maischen_klaar");
                brouwer_start_fase(FASE_SPOELEN);
                ui_ga_naar(SCREEN_ID_SPOELEN);
                return;
            }
        }
        ui_toon_maischen();
        break;

    case FASE_KOKEN: {
        int v = brouw.hop_volgende;
        if (brouw.op_temp && !brouw.hop_alarm_actief && v < brouw.aantal_hops &&
            (int32_t)brouw.resterend_s <= brouw.hops[v].minuten * 60) {
            brouw.hop_alarm_actief = true;
            hw_zoemer(ZOEMER_MELDING);
            net_meld("hop_alarm");
            ui_toon_hop_alarm(true);
        }
        if (brouw.op_temp && brouw.resterend_s == 0) { koken_klaar(); return; }
        ui_toon_koken();
        break;
    }
    default: break;
    }
}
