/*
 * actions.cpp - wat er gebeurt als je op een knop drukt
 *
 * Elke functie action_...() hoort bij een actie uit EEZ Studio (src/ui/actions.h).
 * Het scherm bijwerken gebeurt in ui_update.cpp, het brouwproces zelf
 * (timers, stappen, hopalarmen) in brouwproces.cpp.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/actions.h" //lijst van acties die EEZ zelf verwacht
#include "ui/screens.h" //object... op screen...
#include "brouwer.h"    //eigen functies
#include "hardware.h"   //HW... net...

BrouwStatus brouw;  //het heilige brouwproces

ScreensEnum ui_vorig_scherm();   // ui_update.cpp

/* Stappen bij het instellen */
#define TEMP_MIN   20
#define TEMP_MAX   100
#define TEMP_STAP  1     // °C per klik
#define TIJD_MIN   1
#define TIJD_MAX   240
#define TIJD_STAP  1     // minuten per klik

/* Manuele modus: koken zonder hopschema */
#define MAN_KOOKTIJD_MIN 60
#define MAN_STAP_TEMP   65    // eerste stap als je manueel begint
#define MAN_STAP_MIN    60
#define MAN_EXTRA_TEMP  5     // nieuwe stap = vorige stap + 5 °C ...
#define MAN_EXTRA_MIN   15    // ... gedurende 15 min

/* Standaard PID-waarden (pas aan in Instellingen) */
#define PID_KP 20.0f
#define PID_KI 0.05f
#define PID_KD 10.0f

static int begrens(int waarde, int min, int max) {
    return waarde < min ? min : (waarde > max ? max : waarde);
}

static void ga_terug() { ui_ga_naar(ui_vorig_scherm()); }

/* ======================================================================
 * Opstarten
 * ====================================================================== */

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

/* ======================================================================
 * Startscherm
 * ====================================================================== */

void action_start_automatisch(lv_event_t *e) {
    brouw.automatisch = true;
    brouw.aantal_stappen = 0;   // wacht op het schema van de server
    ui_toon_modus();
    lv_label_set_text_fmt(objects.conn_step1_sub, "%s : %u", brouw.server_ip, brouw.server_poort);
    ui_verbind_stap(1, VS_BEZIG);
    ui_verbind_stap(2, VS_WACHT);
    ui_verbind_stap(3, VS_WACHT);
    ui_ga_naar(SCREEN_ID_VERBINDEN);
    net_verbind_server();
    // De netwerkcode zet daarna de stappen op VS_KLAAR (of VS_FOUT) met
    // ui_verbind_stap(), vult brouw.stappen / hops / recept zodra het schema
    // binnen is, en roept tot slot ui_toon_schema() + ui_ga_naar(SCREEN_ID_SCHEMA) aan.
}

/* Manueel: zelf de maischstappen instellen op het schemascherm,
 * daarna lopen ze net als bij automatisch na elkaar. */
void action_start_manueel(lv_event_t *e) {
    if (brouw.automatisch || brouw.aantal_stappen == 0) {   // vorig recept kwam van de server
        strcpy(brouw.recept, "Manueel");
        brouw.aantal_stappen = 1;
        brouw.stappen[0] = { MAN_STAP_TEMP, MAN_STAP_MIN, 0 };
    }
    brouw.automatisch = false;
    brouw.kooktijd_min = MAN_KOOKTIJD_MIN;
    brouw.aantal_hops = 0;
    ui_toon_modus();
    ui_toon_schema();
    ui_selecteer_schema_rij(-1);
    ui_ga_naar(SCREEN_ID_SCHEMA);
}

void action_open_webapp(lv_event_t *e) {
    char url[48];
    snprintf(url, sizeof url, "http://%s", brouw.server_ip);
    lv_qrcode_update(objects.web_qr, url, strlen(url));
    ui_ga_naar(SCREEN_ID_WEB_APP);
}

/* ======================================================================
 * Verbinden en schema controleren
 * ====================================================================== */

void action_verbinden_annuleren(lv_event_t *e) {
    net_verbreek();
    ui_ga_naar(SCREEN_ID_MAIN);
}

static int schema_keuze = -1;   // geselecteerde rij op het schemascherm

void action_schema_stap_select(lv_event_t *e) {
    schema_keuze = (int)(intptr_t)lv_event_get_user_data(e);
    ui_selecteer_schema_rij(schema_keuze);
}

void action_schema_annuleren(lv_event_t *e) {
    if (brouw.automatisch) net_verbreek();
    ui_ga_naar(SCREEN_ID_MAIN);
}

void action_schema_start(lv_event_t *e) {
    for (int i = 0; i < brouw.aantal_stappen; i++) brouw.stappen[i].sg = 0;
    brouwer_start_fase(FASE_MAISCHEN);
    ui_selecteer_sg_rij(-1);
    net_meld("start");
    ui_ga_naar(SCREEN_ID_MAISCHEN);
}

/* ======================================================================
 * Stap bewerken (vanuit het schema of vanuit pauze)
 * ====================================================================== */

static int edit_stap = 0, edit_temp = 0, edit_min = 0;
static ScreensEnum edit_bron = SCREEN_ID_SCHEMA;

static bool edit_is_lopende_stap() {
    return edit_bron == SCREEN_ID_MAISCHEN_PAUZE && edit_stap == brouw.actieve_stap;
}

static void toon_edit_waarden() {
    lv_label_set_text_fmt(objects.edit_set_temp, "%d °C", edit_temp);
    lv_label_set_text_fmt(objects.edit_set_time, "%d min", edit_min);
}

static void open_edit(int stap, ScreensEnum bron) {
    if (stap < 0 || stap >= brouw.aantal_stappen) return;
    edit_stap = stap;
    edit_bron = bron;
    edit_temp = brouw.stappen[stap].temp;
    edit_min  = brouw.stappen[stap].minuten;
    lv_label_set_text_fmt(objects.edit_title, "Stap %d aanpassen", stap + 1);
    if (edit_is_lopende_stap()) {
        char t[16];
        tijd_tekst(t, sizeof t, brouw.resterend_s);
        lv_label_set_text_fmt(objects.edit_info, "Nog %s van %d:00", t, brouw.stappen[stap].minuten);
    } else if (bron == SCREEN_ID_MAISCHEN_PAUZE && stap < brouw.actieve_stap) {
        lv_label_set_text(objects.edit_info, "Deze stap is al voltooid");
    } else {
        lv_label_set_text(objects.edit_info, "Nog niet gestart");
    }
    toon_edit_waarden();
    if (bron == SCREEN_ID_MAISCHEN_PAUZE) lv_obj_remove_flag(objects.edit_pause_note, LV_OBJ_FLAG_HIDDEN);
    else                                  lv_obj_add_flag(objects.edit_pause_note, LV_OBJ_FLAG_HIDDEN);
    ui_ga_naar(SCREEN_ID_STAP_BEWERKEN);
}

void action_schema_stap_aanpassen(lv_event_t *e) {
    open_edit((int)(intptr_t)lv_event_get_user_data(e), SCREEN_ID_SCHEMA);
}

/* Manueel: stap toevoegen (meteen openen om aan te passen) of verwijderen */
void action_schema_stap_toevoegen(lv_event_t *e) {
    if (brouw.aantal_stappen >= MAX_STAPPEN) return;
    int n = brouw.aantal_stappen;
    MaischStap vorige = n > 0 ? brouw.stappen[n - 1] : MaischStap{ MAN_STAP_TEMP - MAN_EXTRA_TEMP, MAN_STAP_MIN, 0 };
    brouw.stappen[n] = { begrens(vorige.temp + MAN_EXTRA_TEMP, TEMP_MIN, TEMP_MAX), MAN_EXTRA_MIN, 0 };
    brouw.aantal_stappen++;
    schema_keuze = n;
    ui_toon_schema();
    ui_selecteer_schema_rij(n);
    open_edit(n, SCREEN_ID_SCHEMA);
}

void action_schema_stap_verwijderen(lv_event_t *e) {
    if (brouw.aantal_stappen <= 1) return;
    int weg = (schema_keuze >= 0 && schema_keuze < brouw.aantal_stappen) ? schema_keuze : brouw.aantal_stappen - 1;
    for (int i = weg; i < brouw.aantal_stappen - 1; i++) brouw.stappen[i] = brouw.stappen[i + 1];
    brouw.aantal_stappen--;
    schema_keuze = -1;
    ui_toon_schema();
    ui_selecteer_schema_rij(-1);
}

void action_edit_temp_min(lv_event_t *e)  { edit_temp = begrens(edit_temp - TEMP_STAP, TEMP_MIN, TEMP_MAX); toon_edit_waarden(); }
void action_edit_temp_plus(lv_event_t *e) { edit_temp = begrens(edit_temp + TEMP_STAP, TEMP_MIN, TEMP_MAX); toon_edit_waarden(); }
void action_edit_tijd_min(lv_event_t *e)  { edit_min  = begrens(edit_min - TIJD_STAP, TIJD_MIN, TIJD_MAX); toon_edit_waarden(); }
void action_edit_tijd_plus(lv_event_t *e) { edit_min  = begrens(edit_min + TIJD_STAP, TIJD_MIN, TIJD_MAX); toon_edit_waarden(); }

void action_edit_annuleren(lv_event_t *e) {
    ui_ga_naar(edit_bron);
}

void action_edit_opslaan(lv_event_t *e) {
    MaischStap &s = brouw.stappen[edit_stap];

    // Lopende stap: de al verstreken tijd blijft meetellen
    if (edit_is_lopende_stap() && brouw.op_temp) {
        int32_t verstreken = s.minuten * 60 - (int32_t)brouw.resterend_s;
        int32_t nieuw = edit_min * 60 - verstreken;
        brouw.resterend_s = nieuw > 0 ? nieuw : 0;
    } else if (edit_is_lopende_stap()) {
        brouw.resterend_s = edit_min * 60;   // nog aan het opwarmen: volledige tijd
    }
    s.temp = edit_temp;
    s.minuten = edit_min;

    net_meld("schema_gewijzigd");
    ui_toon_schema();
    if (edit_bron == SCREEN_ID_MAISCHEN_PAUZE) ui_selecteer_pauze_rij(edit_stap);
    else                                       ui_selecteer_schema_rij(edit_stap);
    ui_ga_naar(edit_bron);
}

/* ======================================================================
 * Knoppen in de zijbalk
 * ====================================================================== */

void action_pomp_toggle(lv_event_t *e) {
    brouw.pomp_aan = !brouw.pomp_aan;
    hw_pomp(brouw.pomp_aan);
    ui_toon_pomp();
}

void action_pauze(lv_event_t *e) {
    if (ui_huidig_scherm() == SCREEN_ID_MAISCHEN) {
        // Maischen: timer stil, naar pauzescherm waar je stappen kan aanpassen
        brouw.gepauzeerd = true;
        net_meld("pauze");
        ui_toon_maischen();
        ui_selecteer_pauze_rij(brouw.actieve_stap);
        ui_ga_naar(SCREEN_ID_MAISCHEN_PAUZE);
    } else {
        // Koken: knop wisselt tussen PAUZE en HERVAT
        brouw.gepauzeerd = !brouw.gepauzeerd;
        net_meld(brouw.gepauzeerd ? "pauze" : "hervat");
        ui_toon_koken();
    }
}

void action_stop(lv_event_t *e) {
    ui_ga_naar(SCREEN_ID_STOP_BEVESTIGEN);
}

void action_stop_annuleren(lv_event_t *e) {
    ga_terug();
}

void action_stop_bevestigen(lv_event_t *e) {
    brouwer_start_fase(FASE_GEEN);
    brouw.pomp_aan = false;
    hw_verwarming_uit();
    hw_pomp(false);
    hw_zoemer(false);
    net_meld("stop");
    ui_toon_pomp();
    ui_toon_hop_alarm(false);
    ui_ga_naar(SCREEN_ID_MAIN);
}

/* ======================================================================
 * Maischen gepauzeerd
 * ====================================================================== */

void action_mpause_select(lv_event_t *e) {
    ui_selecteer_pauze_rij((int)(intptr_t)lv_event_get_user_data(e));
}

void action_mpause_aanpassen(lv_event_t *e) {
    open_edit((int)(intptr_t)lv_event_get_user_data(e), SCREEN_ID_MAISCHEN_PAUZE);
}

/* ======================================================================
 * SG per maischstap: rij aantikken op het maischscherm
 * ====================================================================== */

static int sg_stap = 0;

static bool sg_klaar(const char *t, char *fout, int n) {
    if (!*t) {                       // leeg = wissen
        brouw.stappen[sg_stap].sg = 0;
        ui_toon_maischen();
        return true;
    }
    float sg = atof(t);
    if (sg > 2) sg /= 1000;          // 1052 mag ook
    if (sg < 0.990f || sg > 1.200f) { snprintf(fout, n, "Geen geldig SG - typ bijv. 1.052"); return false; }
    brouw.stappen[sg_stap].sg = sg;
    net_meld_sg(sg_stap, sg);
    ui_toon_maischen();
    return true;
}

/* Stap aantikken (maisch- of spoelscherm) = selecteren; alleen gestarte stappen */
void action_stap_select(lv_event_t *e) {
    int stap = (int)(intptr_t)lv_event_get_user_data(e);
    if (stap >= brouw.aantal_stappen || stap > brouw.actieve_stap) return;   // nog niet begonnen
    ui_selecteer_sg_rij(stap);
}

/* Knop "SG invullen": voor de geselecteerde stap, anders de lopende (of laatste) stap */
void action_sg_invullen(lv_event_t *e) {
    int stap = ui_sg_doelstap();
    if (stap < 0) return;
    sg_stap = stap;
    char titel[32], waarde[12] = "";
    snprintf(titel, sizeof titel, "SG na stap %d", stap + 1);
    if (brouw.stappen[stap].sg > 0) snprintf(waarde, sizeof waarde, "%.3f", brouw.stappen[stap].sg);
    numpad_open(titel, waarde, "Bijv. 1.052 of 1052 - leeg = wissen", sg_klaar);
}

void action_hervatten(lv_event_t *e) {
    brouw.gepauzeerd = false;
    net_meld("hervat");
    ui_toon_maischen();
    ui_ga_naar(SCREEN_ID_MAISCHEN);
}

/* ======================================================================
 * Spoelen en koken
 * ====================================================================== */

void action_start_koken(lv_event_t *e) {
    if (!brouw.automatisch) {          // manueel: eerst kooktijd en hop instellen
        ui_toon_kookschema();
        ui_ga_naar(SCREEN_ID_KOOKSCHEMA);
        return;
    }
    brouwer_start_fase(FASE_KOKEN);
    net_meld("start_koken");
    ui_ga_naar(SCREEN_ID_KOKEN);
}

/* ======================================================================
 * Kookschema (manueel): kooktijd en tot 5 hopgiften met + en -
 * Hoptijd = minuten voor het einde van het koken (zoals bij automatisch).
 * ====================================================================== */

#define KOOK_STAP 5      // minuten per klik
#define KOOK_MIN  10
#define KOOK_MAX  180

void action_kook_tijd_min(lv_event_t *e) {
    brouw.kooktijd_min = begrens(brouw.kooktijd_min - KOOK_STAP, KOOK_MIN, KOOK_MAX);
    for (int i = 0; i < brouw.aantal_hops; i++)   // hop kan niet vroeger dan de start
        if (brouw.hops[i].minuten > brouw.kooktijd_min) brouw.hops[i].minuten = brouw.kooktijd_min;
    ui_toon_kookschema();
}
void action_kook_tijd_plus(lv_event_t *e) {
    brouw.kooktijd_min = begrens(brouw.kooktijd_min + KOOK_STAP, KOOK_MIN, KOOK_MAX);
    ui_toon_kookschema();
}
void action_kook_hop_min(lv_event_t *e) {
    HopGift &h = brouw.hops[(int)(intptr_t)lv_event_get_user_data(e)];
    h.minuten = begrens(h.minuten - KOOK_STAP, 0, brouw.kooktijd_min);
    ui_toon_kookschema();
}
void action_kook_hop_plus(lv_event_t *e) {
    HopGift &h = brouw.hops[(int)(intptr_t)lv_event_get_user_data(e)];
    h.minuten = begrens(h.minuten + KOOK_STAP, 0, brouw.kooktijd_min);
    ui_toon_kookschema();
}
void action_kook_hop_toevoegen(lv_event_t *e) {
    if (brouw.aantal_hops >= MAX_HOPS) return;
    int n = brouw.aantal_hops;
    int vorige = n > 0 ? brouw.hops[n - 1].minuten : brouw.kooktijd_min + 15;   // eerste hop: 15 min na de start
    brouw.hops[n] = HopGift{};
    brouw.hops[n].minuten = begrens(vorige - 15, 0, brouw.kooktijd_min);
    brouw.aantal_hops++;
    ui_toon_kookschema();
}
void action_kook_hop_verwijderen(lv_event_t *e) {
    if (brouw.aantal_hops > 0) brouw.aantal_hops--;
    ui_toon_kookschema();
}
void action_kook_annuleren(lv_event_t *e) {
    ui_ga_naar(SCREEN_ID_SPOELEN);
}
void action_kook_start(lv_event_t *e) {
    // volgorde zoals het koken ze verwacht: eerst de hop met de meeste minuten
    for (int i = 1; i < brouw.aantal_hops; i++)
        for (int j = i; j > 0 && brouw.hops[j].minuten > brouw.hops[j - 1].minuten; j--) {
            HopGift t = brouw.hops[j]; brouw.hops[j] = brouw.hops[j - 1]; brouw.hops[j - 1] = t;
        }
    brouwer_start_fase(FASE_KOKEN);
    net_meld("start_koken");
    ui_ga_naar(SCREEN_ID_KOKEN);
}

void action_hop_toegevoegd(lv_event_t *e) {
    brouw.hop_alarm_actief = false;
    brouw.hop_volgende++;
    hw_zoemer(false);
    net_meld("hop_toegevoegd");
    ui_toon_hop_alarm(false);
    ui_toon_koken();
}

/* ======================================================================
 * Sensorfout
 * ====================================================================== */

void action_alarm_stil(lv_event_t *e) {
    hw_zoemer(false);
}

void action_sensor_opnieuw(lv_event_t *e) {
    if (hw_sensor_ok()) {
        brouw.sensor_fout = false;
        hw_zoemer(false);
        ga_terug();
    } else {
        lv_label_set_text(objects.err_last, "Sensor reageert nog steeds niet");
    }
}

/* ======================================================================
 * Instellingen en web-app
 * ====================================================================== */

/* Wijzigingen gaan eerst naar een kopie; pas bij Opslaan naar brouw + flash */
static char     inst_ip[16];
static uint16_t inst_poort;
static float    inst_pid[3];   // kp, ki, kd

static void getal_tekst(char *buf, int n, float v) {
    snprintf(buf, n, "%.3f", v);                     // 20.000 -> 20.0, 0.050 -> 0.05
    char *p = buf + strlen(buf) - 1;
    while (*p == '0' && p[-1] != '.') *p-- = 0;
}

static void toon_instellingen() {
    char t[16];
    lv_label_set_text(objects.set_ip_label, inst_ip);
    lv_label_set_text_fmt(objects.set_port_label, "%u", inst_poort);
    lv_obj_t *pid[3] = { objects.set_kp_label, objects.set_ki_label, objects.set_kd_label };
    for (int i = 0; i < 3; i++) { getal_tekst(t, sizeof t, inst_pid[i]); lv_label_set_text(pid[i], t); }
}

void action_open_instellingen(lv_event_t *e) {
    strcpy(inst_ip, brouw.server_ip);
    inst_poort = brouw.server_poort;
    inst_pid[0] = brouw.pid_kp; inst_pid[1] = brouw.pid_ki; inst_pid[2] = brouw.pid_kd;
    toon_instellingen();
    ui_ga_naar(SCREEN_ID_INSTELLINGEN);
}

static bool ip_klaar(const char *t, char *fout, int n) {
    int a, b, c, d; char rest;
    if (sscanf(t, "%d.%d.%d.%d%c", &a, &b, &c, &d, &rest) != 4 ||
        a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) {
        snprintf(fout, n, "Geen geldig IP-adres - bijv. 192.168.0.25"); return false;
    }
    snprintf(inst_ip, sizeof inst_ip, "%d.%d.%d.%d", a, b, c, d);
    toon_instellingen(); return true;
}
static bool poort_klaar(const char *t, char *fout, int n) {
    long v = atol(t);
    if (!*t || strchr(t, '.') || v < 1 || v > 65535) { snprintf(fout, n, "Kies een poort tussen 1 en 65535"); return false; }
    inst_poort = (uint16_t)v; toon_instellingen(); return true;
}
static const float PID_MAX[3] = { 1000, 100, 10000 };
static int pid_keuze = 0;
static bool pid_klaar(const char *t, char *fout, int n) {
    float v = atof(t);
    if (!*t || v < 0 || v > PID_MAX[pid_keuze]) { snprintf(fout, n, "Kies tussen 0 en %g", PID_MAX[pid_keuze]); return false; }
    inst_pid[pid_keuze] = v; toon_instellingen(); return true;
}

/* userData: 0 = IP, 1 = poort, 2 = Kp, 3 = Ki, 4 = Kd */
void action_instelling_numpad(lv_event_t *e) {
    int keuze = (int)(intptr_t)lv_event_get_user_data(e);
    char waarde[16], info[40];
    if (keuze == 0) { numpad_open("Server IP", inst_ip, "Bijv. 192.168.0.25", ip_klaar); return; }
    if (keuze == 1) { snprintf(waarde, sizeof waarde, "%u", inst_poort); numpad_open("Poort", waarde, "Bereik 1 - 65535", poort_klaar); return; }
    static const char *naam[3] = { "Kp (proportioneel)", "Ki (integraal)", "Kd (differentieel)" };
    pid_keuze = keuze - 2;
    getal_tekst(waarde, sizeof waarde, inst_pid[pid_keuze]);
    snprintf(info, sizeof info, "Bereik 0 - %g", PID_MAX[pid_keuze]);
    numpad_open(naam[pid_keuze], waarde, info, pid_klaar);
}

void action_instellingen_terug(lv_event_t *e) {
    ui_ga_naar(SCREEN_ID_MAIN);    // niet opgeslagen wijzigingen vervallen
}

void action_instellingen_opslaan(lv_event_t *e) {
    strcpy(brouw.server_ip, inst_ip);
    brouw.server_poort = inst_poort;
    brouw.pid_kp = inst_pid[0]; brouw.pid_ki = inst_pid[1]; brouw.pid_kd = inst_pid[2];
    instellingen_opslaan();
    ui_ga_naar(SCREEN_ID_MAIN);
}

void action_qr_terug(lv_event_t *e) {
    ui_ga_naar(SCREEN_ID_MAIN);
}

/* ---------- Gedeclareerd in EEZ, maar aan geen enkele knop gekoppeld ----------
 * Ruim deze op in EEZ Studio (tabblad Actions) of koppel ze alsnog. */

void action_zoemer_toggle(lv_event_t *e) {}
void action_alarm_test(lv_event_t *e)    { hw_zoemer(true); }
void action_start_action1(lv_event_t *e) {}
