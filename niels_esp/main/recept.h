#pragma once

/*
 * recept.h - recept van de webapp omzetten naar het recept-bericht voor het scherm
 *
 * Van (GET /api/recipes/{id}, ook de "data" van cmd/recept):
 *   {"name":"Citra Breeze IPA",
 *    "hops":[{"name":"Citra","amount_g":20,"boil_time_min":60}],
 *    "mash_schedule":{"steps":[{"step_nr":1,"temperature_c":65,"duration_min":60}]}, ...}
 * Naar (regelboekje, onderwerp "recept"):
 *   {"naam":"Citra Breeze IPA","kooktijd":60,
 *    "stappen":[{"temp":65,"min":60}],"hops":[{"naam":"Citra - 20 g","min":60}],
 *    "mout":["4,2 kg Pale Ale","0,5 kg Cara"]}   (mout alleen als de webapp ze meestuurt)
 */
#include "cJSON.h"

// Geeft NULL terug als het lukt; *uit is dan een nieuwe tekst (zelf free()).
// Anders de reden waarom het niet lukt (voor de ack naar de back-end).
const char *recept_omzetten(const cJSON *data, char **uit);
