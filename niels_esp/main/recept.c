/*
 * recept.c - recept van de webapp omzetten voor het scherm (zie recept.h)
 *
 * Grenzen van het scherm (brouwer.h, brouwproces.cpp):
 *   - 1 tot 4 maischstappen, hoogstens 5 hopgiften
 *   - receptnaam < 48 bytes, hopnaam < 32 bytes
 *   - temperatuur en minuten als geheel getal
 *   - de hele regel "recept {...}" hoogstens NIELS_MAX_REGEL tekens
 * Het scherm controleert het recept zelf nog eens en antwoordt met
 * brouw/meld recept_ok of recept_fout.
 *
 * mout: wat bij het begin van het maischen in de ketel gaat, als tekst,
 * bv. "4,2 kg Pale Ale". Het scherm toont het bij "Mout toevoegen". Uit het
 * recept van de webapp (Brouwerij-UCLL/web-back-end, GET /api/recipes/{id}):
 *   - "ingredients": de vergistbare ingrediënten, met "name" en "amount_g".
 *     Suikers gaan in de kook, niet in de maisch: die laten we weg (op naam,
 *     de webapp heeft geen soort, zie is_suiker()).
 *   - "extras" met "add_moment":"beslag", met "name", "amount" en "unit".
 * Samen hoogstens 5 regels.
 *
 * kooktijd: "boil_time":{"duration":90} uit het recept. Ontbreekt die, dan
 * de langste hoptijd, met een minimum van STANDAARD_KOOKTIJD minuten.
 */
#include "recept.h"

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "niels_uart.h"

#define MAX_STAPPEN        4
#define MAX_HOPS           5
#define RECEPT_NAAM_MAX    48
#define HOP_NAAM_MAX       32
#define MAX_MOUT           5
#define MOUT_NAAM_MAX      32
#define STANDAARD_KOOKTIJD 60

/* Tekst inkorten tot hoogstens max-1 bytes, zonder een UTF-8-teken te breken */
static void kort_in(char *uit, const char *in, size_t max)
{
    size_t n = strlen(in);
    if (n >= max) {
        n = max - 1;
        while (n > 0 && ((unsigned char)in[n] & 0xC0) == 0x80) n--;   // midden in een teken
    }
    memcpy(uit, in, n);
    uit[n] = 0;
}

static int getal(const cJSON *o, const char *naam, int standaard)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, naam);
    return cJSON_IsNumber(v) ? (int)lround(v->valuedouble) : standaard;
}

typedef struct { int nr, temp, min; } stap_t;
typedef struct { char naam[HOP_NAAM_MAX]; int min; } hop_t;

/* Hoeveelheid als tekst: "4,2 kg", "500 g" of "1 zak" */
static void hoeveelheid(char *uit, size_t n, const cJSON *m)
{
    const cJSON *kg = cJSON_GetObjectItemCaseSensitive(m, "amount_kg");
    const cJSON *g = cJSON_GetObjectItemCaseSensitive(m, "amount_g");
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(m, "amount");
    const char *eenheid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "unit"));
    double w; const char *e;
    if (cJSON_IsNumber(kg))     { w = kg->valuedouble; e = "kg"; }
    else if (cJSON_IsNumber(g)) { w = g->valuedouble;  e = "g"; }
    else if (cJSON_IsNumber(a)) { w = a->valuedouble;  e = eenheid ? eenheid : ""; }
    else { uit[0] = 0; return; }
    if (!strcmp(e, "g") && w >= 1000) { w /= 1000; e = "kg"; }
    snprintf(uit, n, "%g %s", round(w * 100) / 100, e);
    for (char *p = uit; *p; p++) if (*p == '.') *p = ',';   // 4,2 kg
    size_t l = strlen(uit);
    if (l && uit[l - 1] == ' ') uit[l - 1] = 0;
}

/* Tekst bevat een van de woorden (hoofdletters maken niet uit) */
static bool bevat(const char *tekst, const char *const woorden[])
{
    if (!tekst) return false;
    char klein[96];
    size_t i = 0;
    for (; tekst[i] && i < sizeof klein - 1; i++) klein[i] = (char)tolower((unsigned char)tekst[i]);
    klein[i] = 0;
    for (int w = 0; woorden[w]; w++)
        if (strstr(klein, woorden[w])) return true;
    return false;
}

/* Suiker gaat in de kook, niet in de maisch. De ingrediënten in de webapp
 * hebben geen soort, dus kijken we naar de naam. */
static bool is_suiker(const cJSON *m)
{
    static const char *const suiker[] = { "suiker", "sugar", "kandij", "candi", "honing", "honey",
                                          "dextrose", "glucose", "sucrose", "lactose", "stroop", "syrup", NULL };
    return bevat(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "name")), suiker);
}

/* Eén regel voor de pop-up: "4,2 kg Pale Ale" */
static void mout_regel(char *uit, const cJSON *m)
{
    const char *mnaam = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "name"));
    char hv[24], tekst[96];
    hoeveelheid(hv, sizeof hv, m);
    snprintf(tekst, sizeof tekst, "%s%s%s", hv, hv[0] ? " " : "", mnaam ? mnaam : "Mout");
    kort_in(uit, tekst, MOUT_NAAM_MAX);
}

const char *recept_omzetten(const cJSON *data, char **uit)
{
    *uit = NULL;
    if (!cJSON_IsObject(data)) return "recept ontbreekt";

    // Maischstappen, gesorteerd op step_nr
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(data, "mash_schedule");
    const cJSON *steps = cJSON_GetObjectItemCaseSensitive(schema, "steps");
    int n_stappen = cJSON_GetArraySize(steps);
    if (n_stappen < 1) return "recept heeft geen maischstappen";
    if (n_stappen > MAX_STAPPEN) return "te veel maischstappen (het scherm kan er 4)";
    stap_t stappen[MAX_STAPPEN];
    int i = 0;
    const cJSON *s;
    cJSON_ArrayForEach(s, steps) {
        stappen[i].nr = getal(s, "step_nr", i + 1);
        stappen[i].temp = getal(s, "temperature_c", -1);
        stappen[i].min = getal(s, "duration_min", -1);
        if (stappen[i].temp <= 0 || stappen[i].min <= 0) return "maischstap zonder temperatuur of duur";
        i++;
    }
    for (int a = 1; a < n_stappen; a++)              // invoegsortering op step_nr
        for (int b = a; b > 0 && stappen[b].nr < stappen[b - 1].nr; b--) {
            stap_t t = stappen[b]; stappen[b] = stappen[b - 1]; stappen[b - 1] = t;
        }

    // Hopgiften, langste kooktijd eerst (zoals het scherm ze afwerkt)
    const cJSON *hops = cJSON_GetObjectItemCaseSensitive(data, "hops");
    int n_hops = cJSON_GetArraySize(hops);
    if (n_hops > MAX_HOPS) return "te veel hopgiften (het scherm kan er 5)";
    hop_t hp[MAX_HOPS];
    int kooktijd = STANDAARD_KOOKTIJD;
    i = 0;
    const cJSON *h;
    cJSON_ArrayForEach(h, hops) {
        const char *naam = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(h, "name"));
        const cJSON *g = cJSON_GetObjectItemCaseSensitive(h, "amount_g");
        char tekst[96];
        if (cJSON_IsNumber(g))
            snprintf(tekst, sizeof tekst, "%s - %g g", naam ? naam : "Hop", g->valuedouble);
        else
            snprintf(tekst, sizeof tekst, "%s", naam ? naam : "Hop");
        kort_in(hp[i].naam, tekst, sizeof hp[i].naam);
        hp[i].min = getal(h, "boil_time_min", -1);
        if (hp[i].min < 0) return "hopgift zonder kooktijd";
        if (hp[i].min > kooktijd) kooktijd = hp[i].min;
        i++;
    }
    for (int a = 1; a < n_hops; a++)
        for (int b = a; b > 0 && hp[b].min > hp[b - 1].min; b--) {
            hop_t t = hp[b]; hp[b] = hp[b - 1]; hp[b - 1] = t;
        }

    // Mout: "4,2 kg Pale Ale" (ontbreekt de lijst, dan toont het scherm een algemene tekst)
    char mout[MAX_MOUT][MOUT_NAAM_MAX];
    int n_mout = 0;
    const cJSON *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItemCaseSensitive(data, "ingredients")) {
        if (n_mout >= MAX_MOUT) break;   // de rest past niet op het scherm
        if (is_suiker(m)) continue;      // gaat pas in de kook
        mout_regel(mout[n_mout++], m);
    }
    cJSON_ArrayForEach(m, cJSON_GetObjectItemCaseSensitive(data, "extras")) {
        if (n_mout >= MAX_MOUT) break;
        const char *moment = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "add_moment"));
        if (moment && !strcmp(moment, "beslag")) mout_regel(mout[n_mout++], m);
    }

    // Kooktijd uit het recept, als die er is
    const cJSON *kook = cJSON_GetObjectItemCaseSensitive(data, "boil_time");
    int duur = getal(kook, "duration", -1);
    if (duur > 0) {
        for (i = 0; i < n_hops; i++)
            if (hp[i].min > duur) return "hop kookt langer dan de kooktijd";
        kooktijd = duur;
    }

    // Bericht voor het scherm opbouwen
    char naam[RECEPT_NAAM_MAX];
    const char *rnaam = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data, "name"));
    kort_in(naam, rnaam ? rnaam : "Recept", sizeof naam);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "naam", naam);
    cJSON_AddNumberToObject(o, "kooktijd", kooktijd);
    cJSON *st = cJSON_AddArrayToObject(o, "stappen");
    for (i = 0; i < n_stappen; i++) {
        cJSON *x = cJSON_CreateObject();
        cJSON_AddNumberToObject(x, "temp", stappen[i].temp);
        cJSON_AddNumberToObject(x, "min", stappen[i].min);
        cJSON_AddItemToArray(st, x);
    }
    cJSON *hj = cJSON_AddArrayToObject(o, "hops");
    for (i = 0; i < n_hops; i++) {
        cJSON *x = cJSON_CreateObject();
        cJSON_AddStringToObject(x, "naam", hp[i].naam);
        cJSON_AddNumberToObject(x, "min", hp[i].min);
        cJSON_AddItemToArray(hj, x);
    }
    if (n_mout > 0) {
        cJSON *mj = cJSON_AddArrayToObject(o, "mout");
        for (i = 0; i < n_mout; i++) cJSON_AddItemToArray(mj, cJSON_CreateString(mout[i]));
    }
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!json) return "geen geheugen";

    // "recept " + json + "\n" moet in één regel van het scherm passen
    if (strlen(OND_RECEPT) + 1 + strlen(json) + 1 > NIELS_MAX_REGEL) {
        free(json);
        return "recept te lang voor het scherm";
    }
    *uit = json;
    return NULL;
}
