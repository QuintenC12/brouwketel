/*
 * ui_update.cpp - het scherm bijwerken
 *
 * Alles wat teksten, kleuren, zichtbaarheid en balken aanpast staat hier.
 * Kleuren zijn overgenomen uit het EEZ-ontwerp.
 */
#include <stdio.h>
#include <string.h>
#include "ui/screens.h"
#include "brouwer.h"
#include "hardware.h"

/* ---- Kleuren uit het ontwerp ---- */
#define KLEUR_ORANJE      0xf0a030
#define KLEUR_ORANJE_TXT  0xf6c46e
#define KLEUR_ORANJE_BG   0x3a2e17
#define KLEUR_GROEN       0x1f8a57
#define KLEUR_ROOD        0xc62f2f
#define KLEUR_RIJ_BG      0x1f242a
#define KLEUR_TXT         0xe8eaed
#define KLEUR_TXT_DONKER  0x15181c
#define KLEUR_TXT_GRIJS   0x7d868f
#define KLEUR_BALK_BG     0x2c333b

static void toon(lv_obj_t *obj, bool zichtbaar) {
    if (!obj) return;
    if (zichtbaar) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else           lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void bg(lv_obj_t *obj, uint32_t kleur)   { lv_obj_set_style_bg_color(obj, lv_color_hex(kleur), 0); }
static void txt(lv_obj_t *obj, uint32_t kleur)  { lv_obj_set_style_text_color(obj, lv_color_hex(kleur), 0); }

void tijd_tekst(char *buf, int n, uint32_t s) {
    snprintf(buf, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

/* ======================================================================
 * Navigatie
 * ====================================================================== */

static ScreensEnum scherm_huidig = SCREEN_ID_MAIN;
static ScreensEnum scherm_vorig  = SCREEN_ID_MAIN;

void ui_ga_naar(ScreensEnum scherm) {
    if (scherm == scherm_huidig) return;
    scherm_vorig  = scherm_huidig;
    scherm_huidig = scherm;
    loadScreen(scherm);
}

ScreensEnum ui_huidig_scherm() { return scherm_huidig; }
ScreensEnum ui_vorig_scherm()  { return scherm_vorig; }

/* ======================================================================
 * Zijbalk (6 schermen): modus, verbinding, temperatuur, verwarming, pomp
 * ====================================================================== */

struct Zijbalk {
    lv_obj_t *mode, *conn, *conn_dot, *temp, *target, *heat, *heat_bar, *pomp, *pomp_label;
};

static Zijbalk zijbalk(int i) {
    switch (i) {
    case 0: return { objects.mash_mode,   objects.mash_conn,   objects.mash_conn_dot,   objects.mash_temp,   objects.mash_target,   objects.mash_heat,   objects.mash_heat_bar,   objects.mash_pump,   objects.mash_pump_label };
    case 1: return { objects.mpause_mode, objects.mpause_conn, objects.mpause_conn_dot, objects.mpause_temp, objects.mpause_target, objects.mpause_heat, objects.mpause_heat_bar, objects.mpause_pump, objects.mpause_pump_label };
    case 2: return { objects.edit_mode,   objects.edit_conn,   objects.edit_conn_dot,   objects.edit_temp,   objects.edit_target,   objects.edit_heat,   objects.edit_heat_bar,   objects.edit_pump,   objects.edit_pump_label };
    case 3: return { objects.rinse_mode,  objects.rinse_conn,  objects.rinse_conn_dot,  objects.rinse_temp,  objects.rinse_target,  objects.rinse_heat,  objects.rinse_heat_bar,  objects.rinse_pump,  objects.rinse_pump_label };
    case 4: return { objects.boil_mode,   objects.boil_conn,   objects.boil_conn_dot,   objects.boil_temp,   objects.boil_target,   objects.boil_heat,   objects.boil_heat_bar,   objects.boil_pump,   objects.boil_pump_label };
    default:return { objects.err_mode,    objects.err_conn,    objects.err_conn_dot,    objects.err_temp,    objects.err_target,    objects.err_heat,    objects.err_heat_bar,    objects.err_pump,    objects.err_pump_label };
    }
}
#define AANTAL_ZIJBALKEN 6

void ui_toon_pomp() {
    for (int i = 0; i < AANTAL_ZIJBALKEN; i++) {
        Zijbalk z = zijbalk(i);
        lv_label_set_text(z.pomp_label, brouw.pomp_aan ? "AAN" : "UIT");
        bg(z.pomp, brouw.pomp_aan ? KLEUR_GROEN : KLEUR_ROOD);
    }
}

void ui_toon_modus() {
    for (int i = 0; i < AANTAL_ZIJBALKEN; i++)
        lv_label_set_text(zijbalk(i).mode, brouw.automatisch ? "Automatisch" : "Manueel");
}

void ui_toon_meting(float temp, float doel, int vermogen) {
    if (vermogen < 0) vermogen = 0;
    if (vermogen > 100) vermogen = 100;
    for (int i = 0; i < AANTAL_ZIJBALKEN - 1; i++) {   // sensorfout-scherm niet
        Zijbalk z = zijbalk(i);
        lv_label_set_text_fmt(z.temp, "%.1f °C", temp);
        if (doel > 0) lv_label_set_text_fmt(z.target, "%.1f °C", doel);
        else          lv_label_set_text(z.target, "-");
        lv_label_set_text_fmt(z.heat, "%d %%", vermogen);
        lv_bar_set_value(z.heat_bar, vermogen, LV_ANIM_OFF);
    }
    lv_label_set_text_fmt(objects.start_temp, "%.1f °C", temp);
}

void ui_toon_verbinding(bool online) {
    lv_color_t kleur = lv_color_hex(online ? 0x3fb950 : 0xf85149);
    for (int i = 0; i < AANTAL_ZIJBALKEN; i++) {
        Zijbalk z = zijbalk(i);
        lv_label_set_text(z.conn, online ? "Online" : "Offline");
        lv_obj_set_style_bg_color(z.conn_dot, kleur, 0);
    }
    lv_label_set_text(objects.start_conn, online ? "Verbonden" : "Niet verbonden");
    lv_obj_set_style_bg_color(objects.start_conn_dot, kleur, 0);
    lv_label_set_text(objects.set_conn, online ? "Verbonden" : "Niet verbonden");
    lv_obj_set_style_bg_color(objects.set_conn_dot, kleur, 0);
}

/* ======================================================================
 * Maischschema: lijsten op het schema-, maisch- en pauzescherm
 * ====================================================================== */

struct Rij { lv_obj_t *rij, *naam, *temp, *tijd, *potlood, *edit, *sg; };

static Rij schema_rij(int i) {
    switch (i) {
    case 0:  return { objects.schema_row_1, objects.schema_row_1_name, objects.schema_row_1_temp, objects.schema_row_1_time, objects.schema_row_1_pencil, objects.schema_row_1_edit };
    case 1:  return { objects.schema_row_2, objects.schema_row_2_name, objects.schema_row_2_temp, objects.schema_row_2_time, objects.schema_row_2_pencil, objects.schema_row_2_edit };
    case 2:  return { objects.schema_row_3, objects.schema_row_3_name, objects.schema_row_3_temp, objects.schema_row_3_time, objects.schema_row_3_pencil, objects.schema_row_3_edit };
    default: return { objects.schema_row_4, objects.schema_row_4_name, objects.schema_row_4_temp, objects.schema_row_4_time, objects.schema_row_4_pencil, objects.schema_row_4_edit };
    }
}

static Rij pauze_rij(int i) {
    switch (i) {
    case 0:  return { objects.mpause_row_1, objects.mpause_row_1_name, objects.mpause_row_1_temp, objects.mpause_row_1_time, objects.mpause_row_1_pencil, objects.mpause_row_1_edit };
    case 1:  return { objects.mpause_row_2, objects.mpause_row_2_name, objects.mpause_row_2_temp, objects.mpause_row_2_time, objects.mpause_row_2_pencil, objects.mpause_row_2_edit };
    case 2:  return { objects.mpause_row_3, objects.mpause_row_3_name, objects.mpause_row_3_temp, objects.mpause_row_3_time, objects.mpause_row_3_pencil, objects.mpause_row_3_edit };
    default: return { objects.mpause_row_4, objects.mpause_row_4_name, objects.mpause_row_4_temp, objects.mpause_row_4_time, objects.mpause_row_4_pencil, objects.mpause_row_4_edit };
    }
}

static Rij maisch_rij(int i) {
    switch (i) {
    case 0:  return { objects.mash_row_1, objects.mash_row_1_name, objects.mash_row_1_temp, objects.mash_row_1_time, NULL, NULL, objects.mash_row_1_sg };
    case 1:  return { objects.mash_row_2, objects.mash_row_2_name, objects.mash_row_2_temp, objects.mash_row_2_time, NULL, NULL, objects.mash_row_2_sg };
    case 2:  return { objects.mash_row_3, objects.mash_row_3_name, objects.mash_row_3_temp, objects.mash_row_3_time, NULL, NULL, objects.mash_row_3_sg };
    case 3:  return { objects.mash_row_4, objects.mash_row_4_name, objects.mash_row_4_temp, objects.mash_row_4_time, NULL, NULL, objects.mash_row_4_sg };
    case 4:  return { objects.mash_row_5, objects.mash_row_5_name, objects.mash_row_5_temp, objects.mash_row_5_time, NULL, NULL, objects.mash_row_5_sg };
    default: return { objects.mash_row_6, objects.mash_row_6_name, objects.mash_row_6_temp, objects.mash_row_6_time, NULL, NULL, objects.mash_row_6_sg };
    }
}

static Rij spoel_rij(int i) {
    switch (i) {
    case 0:  return { objects.rinse_row_1, objects.rinse_row_1_name, objects.rinse_row_1_temp, objects.rinse_row_1_time, NULL, NULL, objects.rinse_row_1_sg };
    case 1:  return { objects.rinse_row_2, objects.rinse_row_2_name, objects.rinse_row_2_temp, objects.rinse_row_2_time, NULL, NULL, objects.rinse_row_2_sg };
    case 2:  return { objects.rinse_row_3, objects.rinse_row_3_name, objects.rinse_row_3_temp, objects.rinse_row_3_time, NULL, NULL, objects.rinse_row_3_sg };
    default: return { objects.rinse_row_4, objects.rinse_row_4_name, objects.rinse_row_4_temp, objects.rinse_row_4_time, NULL, NULL, objects.rinse_row_4_sg };
    }
}

static void vul_rij(Rij r, int i) {
    bool actief = i < brouw.aantal_stappen;
    toon(r.rij, actief);
    if (!actief) return;
    lv_label_set_text_fmt(r.naam, "Stap %d", i + 1);
    lv_label_set_text_fmt(r.temp, "%d °C", brouw.stappen[i].temp);
    lv_label_set_text_fmt(r.tijd, "%d min", brouw.stappen[i].minuten);
}

/* Geselecteerde rij: oranje rand + lichtere kleur (EEZ-stijl "checked"),
 * "Aanpassen"-knop in plaats van het potloodje */
static void selecteer(Rij (*rij)(int), int keuze) {
    for (int i = 0; i < 4; i++) {
        Rij r = rij(i);
        bool gekozen = (i == keuze);
        if (gekozen) lv_obj_add_state(r.rij, LV_STATE_CHECKED);
        else         lv_obj_remove_state(r.rij, LV_STATE_CHECKED);
        toon(r.edit, gekozen);
        toon(r.potlood, !gekozen);
    }
}

void ui_selecteer_schema_rij(int stap) { selecteer(schema_rij, stap); }
void ui_selecteer_pauze_rij(int stap)  { selecteer(pauze_rij, stap); }

/* SG: geselecteerde stap op het maisch- en spoelscherm (oranje rand) */
static int sg_keuze = -1;

int ui_sg_doelstap() {
    if (brouw.aantal_stappen == 0) return -1;
    if (sg_keuze >= 0 && sg_keuze < brouw.aantal_stappen) return sg_keuze;
    return brouw.actieve_stap < brouw.aantal_stappen ? brouw.actieve_stap : brouw.aantal_stappen - 1;
}

void ui_selecteer_sg_rij(int stap) {
    sg_keuze = stap;
    ui_toon_maischen();
}

void ui_toon_schema() {
    // Automatisch: schema van de server controleren. Manueel: zelf stappen toevoegen/verwijderen.
    bool semi = !brouw.automatisch;
    lv_label_set_text(objects.schema_title, semi ? "Manueel: maischstappen" : "Maischschema controleren");
    if (semi) lv_label_set_text_fmt(objects.schema_recipe, "Daarna koken %d min", brouw.kooktijd_min);
    else      lv_label_set_text(objects.schema_recipe, brouw.recept);
    toon(objects.schema_hint, !semi);
    toon(objects.schema_after, !semi);
    toon(objects.schema_add, semi && brouw.aantal_stappen < MAX_STAPPEN);
    toon(objects.schema_remove, semi && brouw.aantal_stappen > 1);

    for (int i = 0; i < 4; i++) {
        vul_rij(schema_rij(i), i);
        vul_rij(pauze_rij(i), i);
        vul_rij(spoel_rij(i), i);
    }
    for (int i = 0; i < MAX_STAPPEN; i++) vul_rij(maisch_rij(i), i);
    lv_label_set_text_fmt(objects.schema_after, "Daarna: spoelen, koken %d min, %d hopgiften",
                          brouw.kooktijd_min, brouw.aantal_hops);
    ui_toon_maischen();
}

/* ======================================================================
 * Maischen (automatisch en manueel)
 * ====================================================================== */

void ui_toon_maischen() {
    if (brouw.aantal_stappen == 0) return;
    int a = brouw.actieve_stap < brouw.aantal_stappen ? brouw.actieve_stap : brouw.aantal_stappen - 1;
    MaischStap &s = brouw.stappen[a];

    // Fase en timer
    if (brouw.gepauzeerd)    lv_label_set_text(objects.mash_phase, "Gepauzeerd");
    else if (brouw.op_temp)  lv_label_set_text_fmt(objects.mash_phase, "Stabiel op %d °C", s.temp);
    else                     lv_label_set_text_fmt(objects.mash_phase, "Opwarmen naar %d °C", s.temp);

    char t[16];
    tijd_tekst(t, sizeof t, brouw.resterend_s);
    lv_label_set_text(objects.mash_timer, t);
    if (brouw.op_temp) lv_label_set_text(objects.mash_timer_info_1, "resterend");
    else               lv_label_set_text_fmt(objects.mash_timer_info_1, "start bij %d °C", s.temp);

    // Rijen: klaar = grijs + doorstreept, bezig = oranje balk, nog te doen = wit
    // (maischscherm, pauzescherm en spoelscherm)
    for (int i = 0; i < brouw.aantal_stappen && i < MAX_STAPPEN; i++) {
        bool klaar = i < brouw.actieve_stap, bezig = i == brouw.actieve_stap;
        lv_text_decor_t lijn = klaar ? LV_TEXT_DECOR_STRIKETHROUGH : LV_TEXT_DECOR_NONE;

        char sg[12] = "";                 // ingevuld = waarde, gestart = "-", later = leeg
        if (brouw.stappen[i].sg > 0) snprintf(sg, sizeof sg, "%.3f", brouw.stappen[i].sg);
        else if (i <= brouw.actieve_stap) strcpy(sg, "SG -");

        Rij lijsten[3] = { maisch_rij(i), spoel_rij(i), pauze_rij(i) };
        for (int l = 0; l < 3; l++) {
            Rij r = lijsten[l];
            uint32_t voor = klaar ? KLEUR_TXT_GRIJS : KLEUR_TXT;
            if (l == 0) {                 // maischscherm: lopende stap = oranje balk
                bg(r.rij, bezig ? KLEUR_ORANJE : KLEUR_RIJ_BG);
                if (bezig) voor = KLEUR_TXT_DONKER;
            }
            lv_obj_t *labels[4] = { r.naam, r.temp, r.tijd, r.sg };
            for (int k = 0; k < 4; k++) {
                if (!labels[k]) continue;
                txt(labels[k], voor);
                lv_obj_set_style_text_decor(labels[k], lijn, 0);
            }
            if (r.sg) {
                lv_label_set_text(r.sg, sg);
                lv_obj_set_style_text_decor(r.sg, LV_TEXT_DECOR_NONE, 0);   // SG-waarde zelf niet doorstrepen
            }
            if (l < 2) {                  // selectie voor SG: oranje rand (wit op de oranje balk)
                if (i == sg_keuze) lv_obj_add_state(r.rij, LV_STATE_CHECKED);
                else               lv_obj_remove_state(r.rij, LV_STATE_CHECKED);
                lv_obj_set_style_border_color(r.rij, lv_color_hex(l == 0 && bezig ? KLEUR_TXT : KLEUR_ORANJE), LV_PART_MAIN | LV_STATE_CHECKED);
            }
        }
    }
    int doel = ui_sg_doelstap();
    lv_label_set_text_fmt(objects.mash_sg_label, "SG invullen - stap %d", doel + 1);
    lv_label_set_text_fmt(objects.rinse_sg_label, "SG invullen - stap %d", doel + 1);

    // "Daarna"-regel met de resterende tijd van het hele maischschema
    uint32_t rest = brouw.resterend_s;
    for (int i = a + 1; i < brouw.aantal_stappen; i++) rest += brouw.stappen[i].minuten * 60;
    lv_label_set_text_fmt(objects.mash_next, "Spoelen over ca. %lu min",
                          (unsigned long)((rest + 59) / 60));
}

/* ======================================================================
 * Koken
 * ====================================================================== */

struct HopRij { lv_obj_t *rij, *naam, *tijd; };

static HopRij hop_rij(int i) {
    switch (i) {
    case 0:  return { objects.boil_hop_1, objects.boil_hop_1_name, objects.boil_hop_1_time };
    case 1:  return { objects.boil_hop_2, objects.boil_hop_2_name, objects.boil_hop_2_time };
    case 2:  return { objects.boil_hop_3, objects.boil_hop_3_name, objects.boil_hop_3_time };
    case 3:  return { objects.boil_hop_4, objects.boil_hop_4_name, objects.boil_hop_4_time };
    default: return { objects.boil_hop_5, objects.boil_hop_5_name, objects.boil_hop_5_time };
    }
}

static lv_obj_t *hop_balk[MAX_HOPS];      // voortgangsbalkje onderaan elke hoprij (boil_hop_N_bar)
static lv_obj_t *hop_streepje[MAX_HOPS];  // markering op de totale kookbalk (boil_hop_mark_N)

/* Moment (seconden sinds start koken) waarop hop i toegevoegd moet worden */
static int32_t hop_moment(int i) {
    return (int32_t)brouw.kooktijd_min * 60 - brouw.hops[i].minuten * 60;
}

void ui_toon_koken() {
    int32_t totaal   = brouw.kooktijd_min * 60;
    int32_t verlopen = totaal - (int32_t)brouw.resterend_s;
    char t[16];

    // Bovenaan: fase, totale timer en balk
    if (brouw.gepauzeerd)    lv_label_set_text(objects.boil_phase, "Gepauzeerd");
    else if (brouw.op_temp)  lv_label_set_text_fmt(objects.boil_phase, "Kookt - %d min totaal", brouw.kooktijd_min);
    else                     lv_label_set_text(objects.boil_phase, "Opwarmen tot kookpunt");
    tijd_tekst(t, sizeof t, brouw.resterend_s);
    lv_label_set_text(objects.boil_timer, t);
    lv_label_set_text(objects.boil_timer_info, brouw.op_temp ? "resterend" : "start bij kookpunt");
    lv_bar_set_value(objects.boil_bar, totaal > 0 ? verlopen * 100 / totaal : 0, LV_ANIM_OFF);
    lv_label_set_text(objects.boil_pause_label, brouw.gepauzeerd ? "HERVAT" : "PAUZE");

    // Markeringen op de kookbalk: wanneer elke hop erin moet
    // positie zoals ingesteld in EEZ (de echte layout is bij het opstarten nog niet berekend)
    int32_t bx = lv_obj_get_style_x(objects.boil_bar, 0), bw = lv_obj_get_style_width(objects.boil_bar, 0);
    for (int i = 0; i < MAX_HOPS; i++) {
        bool zichtbaar = i < brouw.aantal_hops && totaal > 0;
        toon(hop_streepje[i], zichtbaar);
        if (!zichtbaar) continue;
        lv_obj_set_x(hop_streepje[i], bx + hop_moment(i) * bw / totaal - 1);
        bg(hop_streepje[i], i < brouw.hop_volgende ? KLEUR_TXT_GRIJS : KLEUR_TXT);
    }

    // Kaart "volgende hopgift"
    int v = brouw.hop_volgende;
    if (v < brouw.aantal_hops) {
        if (brouw.hops[v].naam[0]) lv_label_set_text_fmt(objects.boil_next_hop, "Hop %d - %s", v + 1, brouw.hops[v].naam);
        else                       lv_label_set_text_fmt(objects.boil_next_hop, "Hop %d", v + 1);
        int32_t tot = (int32_t)brouw.resterend_s - brouw.hops[v].minuten * 60;
        if (tot < 0) tot = 0;
        tijd_tekst(t, sizeof t, tot);
        lv_label_set_text(objects.boil_next_time, brouw.hop_alarm_actief ? "NU" : t);
        lv_label_set_text(objects.boil_next_info, brouw.hop_alarm_actief ? "toevoegen" : "tot toevoegen");
    } else {
        lv_label_set_text(objects.boil_next_hop, brouw.aantal_hops ? "Alle hop toegevoegd" : "Geen hopschema");
        lv_label_set_text(objects.boil_next_time, "");
        lv_label_set_text(objects.boil_next_info, "");
    }

    // Hoplijst: toegevoegd = grijs, volgende = oranje met voortgangsbalkje, later = wit
    for (int i = 0; i < MAX_HOPS; i++) {
        HopRij r = hop_rij(i);
        bool zichtbaar = i < brouw.aantal_hops;
        toon(r.rij, zichtbaar);
        toon(hop_balk[i], zichtbaar && i == v);
        if (!zichtbaar) continue;

        lv_label_set_text_fmt(r.naam, "Hop %d - %d min kooktijd", i + 1, brouw.hops[i].minuten);
        if (i < v) lv_label_set_text(r.tijd, "toegevoegd");
        else       lv_label_set_text_fmt(r.tijd, "bij nog %d:00", brouw.hops[i].minuten);

        uint32_t achter = KLEUR_RIJ_BG, voor = KLEUR_TXT;
        if (i < v)       voor = KLEUR_TXT_GRIJS;
        else if (i == v) { achter = KLEUR_ORANJE_BG; voor = KLEUR_ORANJE_TXT; }
        bg(r.rij, achter);
        txt(r.naam, voor); txt(r.tijd, voor);

        if (i == v) {   // balkje: van vorige hop (of start) tot deze hop
            int32_t van = i > 0 ? hop_moment(i - 1) : 0, tot = hop_moment(i);
            int pct = tot > van ? (verlopen - van) * 100 / (tot - van) : 100;
            if (!brouw.op_temp) pct = 0;
            lv_bar_set_value(hop_balk[i], pct < 0 ? 0 : (pct > 100 ? 100 : pct), LV_ANIM_OFF);
        }
    }
}

/* ======================================================================
 * Kookschema (manueel): kooktijd en hopgiften instellen voor het koken
 * ====================================================================== */

void ui_toon_kookschema() {
    lv_label_set_text_fmt(objects.kook_tijd, "%d min", brouw.kooktijd_min);
    lv_obj_t *rij[MAX_HOPS]  = { objects.kook_hop_1, objects.kook_hop_2, objects.kook_hop_3, objects.kook_hop_4, objects.kook_hop_5 };
    lv_obj_t *tijd[MAX_HOPS] = { objects.kook_hop_1_time, objects.kook_hop_2_time, objects.kook_hop_3_time, objects.kook_hop_4_time, objects.kook_hop_5_time };
    for (int i = 0; i < MAX_HOPS; i++) {
        toon(rij[i], i < brouw.aantal_hops);
        if (i < brouw.aantal_hops) lv_label_set_text_fmt(tijd[i], "bij nog %d min", brouw.hops[i].minuten);
    }
    toon(objects.kook_leeg, brouw.aantal_hops == 0);
    toon(objects.kook_add, brouw.aantal_hops < MAX_HOPS);
    toon(objects.kook_remove, brouw.aantal_hops > 0);
}

/* ======================================================================
 * Alarmen en meldingen
 * ====================================================================== */

void ui_toon_sensorfout(float laatste) {
    lv_label_set_text_fmt(objects.err_last, "Laatste meting: %.1f °C", laatste);
    ui_ga_naar(SCREEN_ID_SENSOR_FOUT);
}

void ui_toon_hop_alarm(bool zichtbaar) {
    if (zichtbaar && brouw.hop_volgende < brouw.aantal_hops) {
        HopGift &h = brouw.hops[brouw.hop_volgende];
        char tekst[48], info[64];
        if (h.naam[0]) snprintf(tekst, sizeof tekst, "Hop %d - %s", brouw.hop_volgende + 1, h.naam);
        else           snprintf(tekst, sizeof tekst, "Hop %d", brouw.hop_volgende + 1);
        snprintf(info, sizeof info, "Nog %d:00 kooktijd - timer loopt door", h.minuten);
        lv_label_set_text(objects.boil_alarm_text, tekst);
        lv_label_set_text(objects.boil_alarm_sub, info);
        lv_obj_move_foreground(objects.boil_hop_alarm);   // boven de scheidingslijn
    }
    toon(objects.boil_hop_alarm, zichtbaar);
}

/* ======================================================================
 * Verbindingsscherm: bolletje / spinner / vinkje per stap
 * ====================================================================== */

struct VerbindStapUI { lv_obj_t *bol, *teken, *spinner, *tekst; };
static VerbindStapUI vstap[3];

static const char *VSTAP_BEZIG[3] = { "Verbinden met server...", "Wachten op maischschema...", "Schema laden..." };
static const char *VSTAP_KLAAR[3] = { "Verbonden met server", "Maischschema ontvangen", "Schema klaar om te starten" };

void ui_verbind_stap(int stap, VerbindToestand t) {
    if (stap < 1 || stap > 3) return;
    VerbindStapUI &s = vstap[stap - 1];

    toon(s.spinner, t == VS_BEZIG);
    toon(s.bol, t != VS_BEZIG);

    uint32_t kleur = KLEUR_RIJ_BG, rand = 2;        // leeg bolletje
    const char *teken = "";
    if (t == VS_KLAAR) { kleur = KLEUR_GROEN; rand = 0; teken = LV_SYMBOL_OK; }
    if (t == VS_FOUT)  { kleur = 0xc0392b;    rand = 0; teken = LV_SYMBOL_CLOSE; }
    bg(s.bol, kleur);
    lv_obj_set_style_bg_opa(s.bol, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s.bol, rand, 0);
    lv_obj_set_style_border_color(s.bol, lv_color_hex(0x3a434d), 0);
    lv_label_set_text(s.teken, teken);

    lv_label_set_text(s.tekst, t == VS_KLAAR ? VSTAP_KLAAR[stap - 1] : VSTAP_BEZIG[stap - 1]);
    if (stap == 3) lv_obj_set_y(s.tekst, 19);   // stap 3 heeft geen subregel

    const char *status = "Bezig...";
    uint32_t stip = KLEUR_ORANJE;
    if (t == VS_FOUT)                    { status = "Mislukt"; stip = 0xf85149; }
    else if (stap == 3 && t == VS_KLAAR) { status = "Klaar";   stip = 0x3fb950; }
    lv_label_set_text(objects.conn_status, status);
    bg(objects.conn_status_dot, stip);
}

/* ======================================================================
 * Koppelingen met de EEZ-objecten (één keer, na ui_init)
 *
 * Alle widgets (spinners, vinkjes, hopbalkjes, markeringen, de knop
 * "Naar spoelen") staan nu in het EEZ-ontwerp; hier worden ze alleen
 * in de lijsten gezet die de code hierboven gebruikt.
 * ====================================================================== */

void ui_init_extra() {
    vstap[0] = { objects.conn_step1_icon, objects.conn_step1_mark, objects.conn_step1_spinner, objects.conn_step1_text };
    vstap[1] = { objects.conn_step2_icon, objects.conn_step2_mark, objects.conn_step2_spinner, objects.conn_step2_text };
    vstap[2] = { objects.conn_step3_icon, objects.conn_step3_mark, objects.conn_step3_spinner, objects.conn_step3_text };

    lv_obj_t *balk[MAX_HOPS]  = { objects.boil_hop_1_bar, objects.boil_hop_2_bar, objects.boil_hop_3_bar, objects.boil_hop_4_bar, objects.boil_hop_5_bar };
    lv_obj_t *mark[MAX_HOPS]  = { objects.boil_hop_mark_1, objects.boil_hop_mark_2, objects.boil_hop_mark_3, objects.boil_hop_mark_4, objects.boil_hop_mark_5 };
    for (int i = 0; i < MAX_HOPS; i++) {
        hop_balk[i] = balk[i];
        hop_streepje[i] = mark[i];
    }
}
