/*
 * numpad.cpp - numpad-scherm om een waarde in te typen
 *
 * Gebruik: numpad_open("Kp aanpassen", "20.0", "Bereik 0 - 100", klaar);
 * klaar(tekst, fout, n) controleert de getypte tekst en zet de waarde;
 * bij false blijft het numpad open met de foutmelding.
 * Toetsen (EEZ-actie numpad_toets, userData): 0-9 = cijfer, 10 = punt, 11 = wissen.
 */
#include <stdio.h>
#include <string.h>
#include "ui/actions.h"
#include "ui/screens.h"
#include "brouwer.h"

#define NUMPAD_MAX 15   // genoeg voor een IP-adres

static char invoer[NUMPAD_MAX + 1];
static bool nieuw;                  // eerste toets vervangt de oude waarde
static char info_tekst[64];
static NumpadKlaar bij_ok;
static ScreensEnum terug_naar = SCREEN_ID_MAIN;

static void toon() {
    lv_label_set_text(objects.numpad_value, invoer[0] ? invoer : "-");
}

void numpad_open(const char *titel, const char *waarde, const char *info, NumpadKlaar klaar) {
    strncpy(invoer, waarde, NUMPAD_MAX); invoer[NUMPAD_MAX] = 0;
    strncpy(info_tekst, info, sizeof info_tekst - 1); info_tekst[sizeof info_tekst - 1] = 0;
    nieuw = true;
    bij_ok = klaar;
    terug_naar = ui_huidig_scherm();
    lv_label_set_text(objects.numpad_title, titel);
    lv_label_set_text(objects.numpad_info, info_tekst);
    toon();
    ui_ga_naar(SCREEN_ID_NUMPAD);
}

void action_numpad_toets(lv_event_t *e) {
    int toets = (int)(intptr_t)lv_event_get_user_data(e);
    if (nieuw) { invoer[0] = 0; nieuw = false; }
    size_t n = strlen(invoer);
    if (toets == 11) {                       // wissen
        if (n) invoer[n - 1] = 0;
    } else if (n < NUMPAD_MAX) {
        invoer[n] = toets == 10 ? '.' : (char)('0' + toets);
        invoer[n + 1] = 0;
    }
    lv_label_set_text(objects.numpad_info, info_tekst);
    toon();
}

void action_numpad_ok(lv_event_t *e) {
    char fout[64] = "";
    if (bij_ok && !bij_ok(invoer, fout, sizeof fout)) {
        lv_label_set_text(objects.numpad_info, fout);
        return;
    }
    ui_ga_naar(terug_naar);
}

void action_numpad_annuleren(lv_event_t *e) {
    ui_ga_naar(terug_naar);
}
