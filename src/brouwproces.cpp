/*
 * brouwproces.cpp - het eigenlijke brouwproces
 *
 * brouwer_seconde() wordt elke seconde aangeroepen (op de ESP32 vanuit
 * main.cpp, in de simulator vanuit sim/sim_main.cpp). Het telt timers af,
 * gaat naar de volgende maischstap, geeft hopalarmen en werkt het scherm bij.
 *
 * De timer van een stap start pas als de doeltemperatuur bereikt is.
 */
#include "ui/screens.h"
#include "brouwer.h"
#include "hardware.h"

#define MARGE_MAISCHEN 0.5f   // timer start vanaf doel - 0,5 °C
#define KOOKPUNT       98.5f  // vanaf hier telt het koken als "aan de kook"

float brouwer_doel_temp() {
    switch (brouw.fase) {
    case FASE_MAISCHEN:
        return brouw.actieve_stap < brouw.aantal_stappen ? brouw.stappen[brouw.actieve_stap].temp : 0;
    case FASE_KOKEN:   return 100;
    default:           return 0;   // spoelen / niets: verwarming uit
    }
}

void brouwer_start_fase(Fase f) {
    brouw.fase = f;
    brouw.op_temp = false;
    brouw.gepauzeerd = false;
    brouw.bezig = (f != FASE_GEEN);

    switch (f) {
    case FASE_MAISCHEN:
        brouw.actieve_stap = 0;
        brouw.resterend_s = brouw.stappen[0].minuten * 60;
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
        break;
    case FASE_GEEN:
        hw_verwarming_uit();
        break;
    }
}

static void koken_klaar() {
    net_meld("koken_klaar");
    hw_zoemer(true);
    brouwer_start_fase(FASE_GEEN);
    ui_ga_naar(SCREEN_ID_MAIN);
}

void brouwer_seconde(float temp, int vermogen) {
    // Sensorfout: verwarming uit en foutscherm (één keer)
    if (temp <= -100) {
        if (brouw.bezig && !brouw.sensor_fout) {
            brouw.sensor_fout = true;
            hw_verwarming_uit();
            hw_zoemer(true);
            net_meld("sensorfout");
            ui_toon_sensorfout(temp);
        }
        return;
    }

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
            hw_zoemer(true);
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
