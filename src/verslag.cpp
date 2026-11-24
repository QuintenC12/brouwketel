/*
 * verslag.cpp - brouwverslag: wat er echt gebeurd is tijdens de brouwsessie
 *
 * Tijdens de sessie houdt het scherm hier bij:
 *   - het plan (het recept zoals het binnenkwam, of de stappen bij de start in manueel)
 *   - per maischstap hoe lang hij echt duurde (opwarmen en pauzes meegerekend)
 *   - hoe lang spoelen en koken duurden, en wanneer elke hop echt toegevoegd is
 *   - het aantal pauzes en sensorfouten
 * De echte temperaturen, tijden, SG en pH staan in brouw.stappen (die kan je
 * tijdens de pauze aanpassen). SG en pH na het koken staan hier.
 *
 * Op het einde (koken klaar of STOP) gaat alles als één JSON-bericht naar
 * Niels ESP (net_stuur_verslag), die het op MQTT zet (brouw/verslag).
 * Alles staat alleen in het geheugen: valt de stroom weg, dan is het verslag weg.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "brouwer.h"
#include "hardware.h"
#include "ui/screens.h"

struct Verslag {
    bool     actief;                     // er loopt een sessie waarvoor nog geen verslag verstuurd is
    char     recept[48];
    bool     automatisch;
    // plan
    MaischStap plan[MAX_STAPPEN];
    int      plan_aantal;
    int      plan_kooktijd;
    // wat er echt gebeurde
    uint32_t totaal_s;
    uint32_t stap_s[MAX_STAPPEN];
    uint32_t spoelen_s;
    uint32_t koken_s;
    uint32_t hop_s[MAX_HOPS];            // seconden na het begin van het koken (0 = niet toegevoegd)
    bool     koken_gestart;
    int      pauzes;
    int      sensorfouten;
    float    kook_sg, kook_ph;           // 0 = niet ingevuld
};

static Verslag v;

/* Het recept van de server: dit is het plan (ook als je het daarna nog aanpast) */
static Recept recept_plan;
static bool   recept_plan_ok = false;

void verslag_onthoud_recept(const Recept &r) {
    recept_plan = r;
    recept_plan_ok = true;
}

void verslag_begin() {
    memset(&v, 0, sizeof v);
    v.actief = true;
    v.automatisch = brouw.automatisch;
    strncpy(v.recept, brouw.recept, sizeof v.recept - 1);
    if (brouw.automatisch && recept_plan_ok) {
        memcpy(v.plan, recept_plan.stappen, sizeof v.plan);
        v.plan_aantal = recept_plan.aantal_stappen;
        v.plan_kooktijd = recept_plan.kooktijd_min;
    } else {
        // manueel: het plan is wat je op het schemascherm ingesteld hebt
        memcpy(v.plan, brouw.stappen, sizeof v.plan);
        v.plan_aantal = brouw.aantal_stappen;
        v.plan_kooktijd = 0;                 // komt bij het kookschema (verslag_koken_begin)
    }
}

void verslag_koken_begin() {
    v.koken_gestart = true;
    if (!v.automatisch || v.plan_kooktijd == 0) v.plan_kooktijd = brouw.kooktijd_min;
}

void verslag_seconde() {
    if (!v.actief || !brouw.bezig) return;
    v.totaal_s++;
    switch (brouw.fase) {
    case FASE_MAISCHEN:
        if (brouw.actieve_stap < MAX_STAPPEN) v.stap_s[brouw.actieve_stap]++;
        break;
    case FASE_SPOELEN: v.spoelen_s++; break;
    case FASE_KOKEN:   v.koken_s++;   break;
    default: break;
    }
}

void verslag_pauze()        { if (v.actief) v.pauzes++; }
void verslag_sensorfout()   { if (v.actief) v.sensorfouten++; }
void verslag_hop(int i)     { if (v.actief && i >= 0 && i < MAX_HOPS) v.hop_s[i] = v.koken_s > 0 ? v.koken_s : 1; }
void verslag_kook_sg(float sg) { v.kook_sg = sg; }
void verslag_kook_ph(float ph) { v.kook_ph = ph; }
bool verslag_actief()       { return v.actief; }

/* ---------------- JSON maken ---------------- */

static char  *pos;
static size_t vrij;

static void zet(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void zet(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(pos, vrij, fmt, args);
    va_end(args);
    if (n < 0) return;
    if ((size_t)n >= vrij) n = vrij > 0 ? (int)vrij - 1 : 0;   // afgekapt: wordt hieronder gemeld
    pos += n;
    vrij -= n;
}

/* Tekst tussen aanhalingstekens, met " en \ veilig gemaakt */
static void zet_tekst(const char *t) {
    zet("\"");
    for (; *t; t++) {
        if (*t == '"' || *t == '\\') zet("\\%c", *t);
        else if ((unsigned char)*t >= 0x20) zet("%c", *t);
    }
    zet("\"");
}

static void zet_meting(const char *naam, float waarde, int decimalen) {
    if (waarde > 0) zet(",\"%s\":%.*f", naam, decimalen, waarde);
}

void verslag_einde(bool afgebroken) {
    if (!v.actief) return;
    v.actief = false;

    static char json[1400];
    pos = json;
    vrij = sizeof json;

    zet("{\"recept\":");
    zet_tekst(v.recept);
    zet(",\"modus\":\"%s\",\"afgebroken\":%s,\"duur_s\":%lu,\"pauzes\":%d,\"sensorfouten\":%d",
        v.automatisch ? "automatisch" : "manueel", afgebroken ? "true" : "false",
        (unsigned long)v.totaal_s, v.pauzes, v.sensorfouten);

    // Maischstappen: plan en wat er echt gebeurde
    zet(",\"stappen\":[");
    int n = brouw.aantal_stappen > v.plan_aantal ? brouw.aantal_stappen : v.plan_aantal;
    for (int i = 0; i < n && i < MAX_STAPPEN; i++) {
        if (i) zet(",");
        zet("{\"stap\":%d", i + 1);
        if (i < v.plan_aantal) zet(",\"plan_temp\":%d,\"plan_min\":%d", v.plan[i].temp, v.plan[i].minuten);
        if (i < brouw.aantal_stappen) {
            const MaischStap &s = brouw.stappen[i];
            zet(",\"temp\":%d,\"min\":%d,\"duur_s\":%lu", s.temp, s.minuten, (unsigned long)v.stap_s[i]);
            zet_meting("sg", s.sg, 3);
            zet_meting("ph", s.ph, 2);
        }
        zet("}");
    }
    zet("],\"spoelen_s\":%lu", (unsigned long)v.spoelen_s);

    // Koken (alleen als het begonnen is)
    if (v.koken_gestart) {
        zet(",\"koken\":{\"plan_min\":%d,\"min\":%d,\"duur_s\":%lu",
            v.plan_kooktijd, brouw.kooktijd_min, (unsigned long)v.koken_s);
        zet_meting("sg", v.kook_sg, 3);
        zet_meting("ph", v.kook_ph, 2);
        zet(",\"hops\":[");
        for (int i = 0; i < brouw.aantal_hops; i++) {
            if (i) zet(",");
            zet("{\"naam\":");
            zet_tekst(brouw.hops[i].naam);
            zet(",\"min\":%d", brouw.hops[i].minuten);
            if (v.hop_s[i]) zet(",\"toegevoegd_s\":%lu", (unsigned long)v.hop_s[i]);
            zet("}");
        }
        zet("]}");
    }
    zet("}");

    if (vrij <= 1) {
        printf("[verslag] te lang, niet verstuurd\n");
        return;
    }
    net_stuur_verslag(json);
}

/* ---------------- Overzichtscherm (Brouwsessie klaar) ---------------- */

static void duur_tekst(char *buf, int n, uint32_t s) {
    uint32_t min = (s + 30) / 60;
    if (min >= 60) snprintf(buf, n, "%lu u %02lu min", (unsigned long)(min / 60), (unsigned long)(min % 60));
    else           snprintf(buf, n, "%lu min", (unsigned long)min);
}

static void meting_tekst(char *buf, int n, const char *voor, float w, int decimalen) {
    if (w > 0) snprintf(buf, n, "%s%.*f", voor, decimalen, w);
    else       snprintf(buf, n, "%s-", voor);
}

struct OvRij { lv_obj_t *rij, *naam, *temp, *tijd, *duur, *sg, *ph; };

static OvRij ov_rij(int i) {
    switch (i) {
    case 0:  return { objects.ov_row_1, objects.ov_row_1_name, objects.ov_row_1_temp, objects.ov_row_1_time, objects.ov_row_1_duur, objects.ov_row_1_sg, objects.ov_row_1_ph };
    case 1:  return { objects.ov_row_2, objects.ov_row_2_name, objects.ov_row_2_temp, objects.ov_row_2_time, objects.ov_row_2_duur, objects.ov_row_2_sg, objects.ov_row_2_ph };
    case 2:  return { objects.ov_row_3, objects.ov_row_3_name, objects.ov_row_3_temp, objects.ov_row_3_time, objects.ov_row_3_duur, objects.ov_row_3_sg, objects.ov_row_3_ph };
    default: return { objects.ov_row_4, objects.ov_row_4_name, objects.ov_row_4_temp, objects.ov_row_4_time, objects.ov_row_4_duur, objects.ov_row_4_sg, objects.ov_row_4_ph };
    }
}

static void vul(OvRij r, const char *naam, const char *temp, const char *tijd,
                const char *duur, const char *sg, const char *ph) {
    lv_label_set_text(r.naam, naam);
    lv_label_set_text(r.temp, temp);
    lv_label_set_text(r.tijd, tijd);
    lv_label_set_text(r.duur, duur);
    lv_label_set_text(r.sg, sg);
    lv_label_set_text(r.ph, ph);
}

/* Vult het overzicht met de echte waarden (ook al is het verslag al verstuurd) */
void ui_toon_overzicht() {
    char t1[16], t2[16], t3[24], sg[16], ph[16], info[96];
    duur_tekst(t3, sizeof t3, v.totaal_s);
    snprintf(info, sizeof info, "%s  |  totale duur %s", v.recept, t3);
    lv_label_set_text(objects.ov_info, info);

    for (int i = 0; i < MAX_STAPPEN; i++) {
        OvRij r = ov_rij(i);
        if (i >= brouw.aantal_stappen) { lv_obj_add_flag(r.rij, LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(r.rij, LV_OBJ_FLAG_HIDDEN);
        const MaischStap &s = brouw.stappen[i];
        char naam[12];
        snprintf(naam, sizeof naam, "Stap %d", i + 1);
        snprintf(t1, sizeof t1, "%d °C", s.temp);
        snprintf(t2, sizeof t2, "%d min", s.minuten);
        duur_tekst(t3, sizeof t3, v.stap_s[i]);
        meting_tekst(sg, sizeof sg, "", s.sg, 3);
        meting_tekst(ph, sizeof ph, "pH ", s.ph, 2);
        vul(r, naam, t1, t2, t3, sg, ph);
    }

    OvRij k = { objects.ov_kook, objects.ov_kook_name, objects.ov_kook_temp, objects.ov_kook_time,
                objects.ov_kook_duur, objects.ov_kook_sg, objects.ov_kook_ph };
    snprintf(t2, sizeof t2, "%d min", brouw.kooktijd_min);
    duur_tekst(t3, sizeof t3, v.koken_s);
    meting_tekst(sg, sizeof sg, "", v.kook_sg, 3);
    meting_tekst(ph, sizeof ph, "pH ", v.kook_ph, 2);
    vul(k, "Koken", "-", t2, t3, sg, ph);
}
