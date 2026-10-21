/*
 * brouwer.h - gedeelde toestand van de brouwinstallatie + functies
 *
 * Indeling van de code:
 *   actions.cpp      wat er gebeurt als je op een knop drukt
 *   ui_update.cpp    het scherm bijwerken (teksten, kleuren, balken)
 *   brouwproces.cpp  het brouwproces: timers, stappen, hopgiften, alarmen
 *   numpad.cpp       numpad-scherm om waarden in te typen
 *   pid.cpp          PID-regelaar voor de verwarming
 *   hardware.cpp     sensor, verwarming, pomp, zoemer, netwerk (ESP32)
 *   sim/             vervangt hardware.cpp in de simulator
 */
#pragma once
#include <stdint.h>
#include "ui/ui.h"

#define MAX_STAPPEN 4
#define MAX_HOPS    5

struct MaischStap {
    int temp;      // doeltemperatuur in °C
    int minuten;   // rusttijd
    float sg;      // gemeten soortelijk gewicht na deze stap (0 = nog niet ingevuld)
};

struct HopGift {
    char naam[32]; // bijv. "Cascade - 30 g"
    int minuten;   // toevoegen wanneer er nog zoveel minuten kooktijd over zijn
};

enum Fase { FASE_GEEN, FASE_MAISCHEN, FASE_SPOELEN, FASE_KOKEN };

struct BrouwStatus {
    // Recept (komt van de server, of zelf ingesteld in manuele modus)
    char recept[48];                    // "Demo Tripel - 20 L"
    MaischStap stappen[MAX_STAPPEN];    // tot 4 maischstappen
    int aantal_stappen;
    HopGift hops[MAX_HOPS];         // tot 5 hopgiften
    int aantal_hops;
    int kooktijd_min;

    // Procestoestand
    Fase fase;
    bool bezig;            // er loopt een brouwsessie
    bool automatisch;      // true = schema van server, false = manueel (zelf ingesteld)
    bool gepauzeerd;       // timer staat stil
    bool op_temp;          // doeltemperatuur bereikt: de timer loopt
    bool pomp_aan;
    int  actieve_stap;     // index in stappen[] 0 = stap 1
    uint32_t resterend_s;  // resterende tijd van de actieve stap / het koken
    int  hop_volgende;     // index van de eerstvolgende hopgift
    bool hop_alarm_actief;
    bool sensor_fout;

    // Instellingen
    char server_ip[16];
    uint16_t server_poort;
    float pid_kp, pid_ki, pid_kd;   // PID-regelaar van de verwarming
};

extern BrouwStatus brouw;

/* ---- Opstarten: na ui_init() aanroepen ---- */
void brouwer_init();

/* ---- Brouwproces (brouwproces.cpp) ----
 * Roep brouwer_seconde() elke seconde aan met de gemeten temperatuur en
 * het verwarmingsvermogen. Het regelt timers, stapovergangen, hopalarmen
 * en werkt het scherm bij. */
void  brouwer_seconde(float temp, int vermogen_pct);
float brouwer_doel_temp();          // doeltemperatuur voor de verwarmingsregeling (0 = uit)
void  brouwer_start_fase(Fase f);   // door de acties gebruikt, nieuwe fase beginnen

/* ---- Navigatie ---- */
void ui_ga_naar(enum ScreensEnum scherm);
enum ScreensEnum ui_huidig_scherm();

/* ---- Scherm bijwerken (ui_update.cpp) ---- */
void ui_init_extra();                                   // koppelt EEZ-objecten aan de lijsten in ui_update.cpp
void ui_toon_meting(float temp, float doel, int vermogen_pct);
void ui_toon_verbinding(bool online);
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
void ui_toon_sensorfout(float laatste_meting);
void ui_toon_hop_alarm(bool zichtbaar);

/* Verbindingsscherm: elke stap (1..3) toont een bolletje, spinner, vinkje of kruisje */
enum VerbindToestand { VS_WACHT, VS_BEZIG, VS_KLAAR, VS_FOUT };
void ui_verbind_stap(int stap, VerbindToestand toestand);

/* ---- Numpad (numpad.cpp) ----
 * Opent het numpad-scherm. Bij OK wordt klaar() aangeroepen met de getypte
 * tekst; geeft die false terug (ongeldige waarde), dan blijft het numpad open
 * en komt de foutmelding in de info-regel. Daarna terug naar het vorige scherm. */
typedef bool (*NumpadKlaar)(const char *tekst, char *fout, int fout_n);
void numpad_open(const char *titel, const char *waarde, const char *info, NumpadKlaar klaar);

/* seconden als "m:ss" */
void tijd_tekst(char *buf, int n, uint32_t s);
