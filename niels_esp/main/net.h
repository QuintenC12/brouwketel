#pragma once

/*
 * net.h - vertaling tussen het scherm (UART) en de webapp (MQTT)
 *
 * Het UART-protocol met het scherm blijft zoals in het regelboekje.
 * Dit deel zet het om naar de MQTT-afspraken van het back-end-team
 * ("MQTT-afspraken met elektronica"): topics brouwer/<mac>/...
 *
 * Wifi zelf regelt de component brouw_netwerk (hotspot + formulier).
 * MQTT verbindt vanzelf zodra er wifi is, met de broker uit dat formulier,
 * zodat de webapp de ketel altijd als online ziet.
 */
#include <stdbool.h>

#include "cJSON.h"

// Eenmaal aanroepen na scherm_uart_init() en brouw_netwerk_start().
void net_init(void);

/* ---- van het scherm (receive_task) ---- */

// net/start: het scherm wacht op net/status "verbonden" en op het recept.
// server/poort uit het bericht worden alleen gebruikt als het formulier
// (nog) geen broker heeft.
void net_start(const char *server, int poort);

// net/stop: het scherm wacht niet meer op een recept. MQTT blijft verbonden.
void net_stop(void);

// net/modus: configuratie = hotspot + formulier openen (wifi en MQTT gaan uit),
// anders gewoon werken. Komt elke seconde; alleen een verandering doet iets.
void net_modus(bool configuratie);

// Een nieuwe brouw/meld (al zonder herhalingen) of een brouw/status.
void net_van_scherm(const char *onderwerp, const char *json);

// net/vraag: stuur meteen net/toestand naar het scherm.
void net_vraag(void);

// Een nieuw brouwverslag (al zonder nr en zonder herhalingen) -> MQTT verslag.
// Zonder MQTT wordt het bewaard en verstuurd zodra de broker er weer is.
void net_verslag(const cJSON *verslag);

/* ---- van de ketel (temp_task, elke seconde) ---- */

// temp in °C (alleen geldig als ok), vermogen 0..100, pomp en zoemer aan/uit
void net_ketel(float temp, bool ok, int vermogen, bool pomp, bool zoemer);
