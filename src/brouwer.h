/*
 * brouwer.h - gedeelde toestand van de brouwinstallatie + functies
 *
 * Indeling van de code:
 *   actions.cpp      wat er gebeurt als je op een knop drukt
 *   ui_update.cpp    het scherm bijwerken (teksten, kleuren, balken)
 *   brouwproces.cpp  het brouwproces: timers, stappen, hopgiften, alarmen
 *   numpad.cpp       numpad-scherm om waarden in te typen
 *   verslag.cpp      brouwverslag: wat er echt gebeurd is, op het einde naar de server
 *   uart.cpp         koppeling met Niels ESP (sensor, verwarming, pomp, zoemer, netwerk)
 *   instellingen.cpp IP, poort en PID-waarden in flash
 *   sim/             vervangt uart.cpp en instellingen.cpp in de simulator
 */
#pragma once
#include <stdint.h>
#include "ui/ui.h"

#define MAX_STAPPEN 4
#define MAX_HOPS    10   // hop + extra's tijdens het koken (scrollbare lijst op het kookscherm)
#define MAN_MAX_HOPS 5   // manueel kookschema: 5 rijen
#define MAX_MOUT    5    // regels mout op de pop-up "Mout toevoegen"
#define MOUT_NAAM   32   // "4,2 kg Pale Ale"

#define TEMP_MIN    10     // grenzen voor de temperatuur van een maischstap (°C)
#define TEMP_MAX    100
#define MAN_KOOKTIJD_MIN 60   // kooktijd als er geen recept is

struct MaischStap {
    int temp;      // doeltemperatuur in °C
    int minuten;   // rusttijd
    float sg;      // gemeten soortelijk gewicht na deze stap (0 = nog niet ingevuld)
    float ph;      // gemeten pH na deze stap (0 = nog niet ingevuld)
};

struct HopGift {
    char naam[32]; // bijv. "Cascade - 30 g"
    int minuten;   // toevoegen wanneer er nog zoveel minuten kooktijd over zijn
    bool extra;    // geen hop maar een extra (kruiden, suiker, ...): zelfde alarm, andere tekst
};

/* Een recept zoals het binnenkomt, nog niet gecontroleerd */
struct Recept {
    char naam[48];
    MaischStap stappen[MAX_STAPPEN];
    int aantal_stappen;
    HopGift hops[MAX_HOPS];
    int aantal_hops;
    int kooktijd_min;
    char mouten[MAX_MOUT][MOUT_NAAM];   // bv. "4,2 kg Pale Ale" (mag leeg zijn)
    int aantal_mouten;
};

enum Fase { FASE_GEEN, FASE_MAISCHEN, FASE_SPOELEN, FASE_KOKEN };

/* Begin van het maischen (opgave, wizard stap 4): eerst opwarmen tot stap 1,
 * dan mout toevoegen, dan ketel sluiten en pomp aansluiten. Pas na
 * "Start maischen" loopt de timer van stap 1 en draait de pomp. */
enum MoutStap { MOUT_OPWARMEN, MOUT_TOEVOEGEN, MOUT_SLUITEN, MOUT_KLAAR };

struct BrouwStatus {
    // Recept
    char recept[48];                    // "Demo Tripel - 20 L"
    MaischStap stappen[MAX_STAPPEN];    // tot 4 maischstappen
    int aantal_stappen;
    HopGift hops[MAX_HOPS];         // tot 10 hopgiften en extra's
    int aantal_hops;
    int kooktijd_min;
    char mouten[MAX_MOUT][MOUT_NAAM];     // mout om toe te voegen bij stap 1 (leeg = niet bekend)
    int aantal_mouten;

    // Procestoestand
    Fase fase;
    bool bezig;            // er loopt een brouwsessie
    bool automatisch;      // true = schema van server, false = manueel (zelf ingesteld)
    bool gepauzeerd;       // timer staat stil
    bool op_temp;          // doeltemperatuur bereikt: de timer loopt
    MoutStap mout;         // begin van het maischen: wachten op mout en "Start maischen"
    bool pomp_aan;
    int  actieve_stap;     // index in stappen[] 0 = stap 1
    uint32_t resterend_s;  // resterende tijd van de actieve stap / het koken
    int  hop_volgende;     // index van de eerstvolgende hopgift
    bool hop_alarm_actief;
    bool sensor_fout;

    // Wifi-netwerk en broker zoals Niels ESP die meldt in net/toestand (ingevuld via
    // zijn hotspot). Het scherm toont ze alleen; leeg/0 zolang Niels ESP niets gemeld heeft.
    char wifi_ssid[33];
    char server_ip[64];
    uint16_t server_poort;

    // Instellingen
    float pid_kp, pid_ki, pid_kd;   // PID-regelaar van de verwarming (draait op Niels ESP)
};

extern BrouwStatus brouw; //iedereen kan het aanpassen

/* ---- Opstarten: na ui_init() aanroepen ---- */
void brouwer_init();

/* ---- Brouwproces (brouwproces.cpp) ----
 * Roep brouwer_seconde() elke seconde aan met de gemeten temperatuur en
 * het verwarmingsvermogen. Het regelt timers, stapovergangen, hopalarmen
 * en werkt het scherm bij. */
void  brouwer_seconde(float temp, int vermogen_pct);
float brouwer_doel_temp();          // doeltemperatuur voor de verwarmingsregeling (0 = uit)
void  brouwer_start_fase(Fase f);   // door de acties gebruikt, nieuwe fase beginnen
void  brouwer_mout_bevestigen();     // knop op de pop-up: mout toegevoegd, daarna start maischen
const char *brouwer_recept_overnemen(const Recept &r);  // nullptr = ok, anders de reden; brouw verandert alleen als alles klopt

/* ---- Navigatie ---- */
void ui_ga_naar(enum ScreensEnum scherm);
enum ScreensEnum ui_huidig_scherm();

/* ---- Scherm bijwerken (ui_update.cpp) ---- */
void ui_init_extra();                                   // koppelt EEZ-objecten aan de lijsten in ui_update.cpp
void ui_toon_meting(float temp, float doel, int vermogen_pct);
void ui_toon_netwerk(bool wifi, bool mqtt, bool server, bool configuratie);   // wifi-logo, bolletjes en modus (uit net/toestand)
void ui_toon_server();                                  // server-IP en poort op Instellingen (uit net/toestand)
void ui_zet_modus_schakelaar(bool configuratie);        // schakelaar op Instellingen aan/uit zetten
void ui_toon_pomp();
void ui_toon_modus();
void ui_toon_schema();                                  // schema-, maisch- en pauzelijsten vullen
void ui_selecteer_schema_rij(int stap);                 // -1 = geen
void ui_selecteer_pauze_rij(int stap);
void ui_selecteer_sg_rij(int stap);                     // maisch- en spoelscherm: stap kiezen voor SG (-1 = geen)
int  ui_sg_doelstap();                                  // geselecteerde stap, anders lopende/laatste stap
void ui_toon_maischen();                                // fase, timer, actieve stap, SG
void ui_toon_koken();                                   // timer, volgende hop, hoplijst, balken
void ui_toon_kookschema();                              // manueel: kooktijd + hopgiften instellen
void ui_toon_kookoverzicht(bool zichtbaar);             // automatisch: overzicht van het koken op het spoelscherm
void ui_toon_sensorfout(float laatste_meting);
void ui_toon_hop_alarm(bool zichtbaar);
void ui_toon_mout();                                    // pop-up "Mout toevoegen" / "Ketel sluiten" op het maischscherm

/* Verbindingsscherm: elke stap (1..3) toont een bolletje, spinner, vinkje of kruisje */
enum VerbindToestand { VS_WACHT, VS_BEZIG, VS_KLAAR, VS_FOUT };
void ui_verbind_stap(int stap, VerbindToestand toestand);

/* ---- Numpad (numpad.cpp) ----
 * Opent het numpad-scherm. Bij OK wordt klaar() aangeroepen met de getypte
 * tekst; geeft die false terug (ongeldige waarde), dan blijft het numpad open
 * en komt de foutmelding in de info-regel. Daarna terug naar het vorige scherm. */
typedef bool (*NumpadKlaar)(const char *tekst, char *fout, int fout_n);
void numpad_open(const char *titel, const char *waarde, const char *info, NumpadKlaar klaar);
/* Optioneel, meteen na numpad_open: wat er moet gebeuren bij Annuleren.
 * klaar() en annuleren() mogen zelf een nieuw numpad openen (vragen na elkaar). */
void numpad_bij_annuleren(void (*annuleren)());

/* ---- Brouwverslag (verslag.cpp) ---- */
void verslag_onthoud_recept(const Recept &r);   // recept van de server = het plan
void verslag_begin();                           // bij de start van de sessie
void verslag_koken_begin();
void verslag_seconde();                         // elke seconde (vanuit brouwer_seconde)
void verslag_pauze();
void verslag_sensorfout();
void verslag_hop(int hop);                      // hop is net toegevoegd
void verslag_kook_sg(float sg);
void verslag_kook_ph(float ph);
bool verslag_actief();
void verslag_einde(bool afgebroken);            // verslag versturen naar Niels ESP
void meting_na_koken();                         // actions.cpp: SG en pH na het koken vragen, daarna het verslag
void ui_toon_overzicht();                       // overzichtscherm "Brouwsessie klaar" vullen

/* seconden als "m:ss" */
void tijd_tekst(char *buf, int n, uint32_t s);
