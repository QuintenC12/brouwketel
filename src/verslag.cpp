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
