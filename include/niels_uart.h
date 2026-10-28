/*
 * niels_uart.h - afspraken voor de kabel tussen het scherm en Niels ESP
 *
 * Dit bestand is voor BEIDE kanten: het staat ook in niels_esp/include/.
 * Het volledige regelboekje (met voorbeelden) staat in het project.
 *
 * Kabel: TX van de ene kant naar RX van de andere, en GND naar GND.
 * Snelheid: NIELS_BAUD, 8N1.
 *
 * Elk bericht is één regel tekst:
 *     onderwerp {json}\n
 * bijvoorbeeld:
 *     ketel/temp {"temp":65.3,"ok":true}
 * Een regel die geen geldige JSON is, of een onderwerp dat je niet kent,
 * wordt gewoon genegeerd. Niels ESP zet er MQTT_VOORVOEGSEL voor als hij
 * een bericht naar MQTT doorstuurt.
 */
#pragma once

#define NIELS_BAUD        115200
#define NIELS_MAX_REGEL   512        // langste regel (het recept) in tekens
#define MQTT_VOORVOEGSEL  "brouwketel/"

/* ---- scherm -> Niels ESP ---- */
#define OND_STUUR           "brouw/stuur"      // {"vermogen":40,"pomp":true,"zoemer":false}   5x per seconde
#define OND_STATUS          "brouw/status"     // {"fase":"maischen","stap":2,"doel":63,"rest":754,"pauze":false}
#define OND_MELD            "brouw/meld"       // {"nr":7,"naam":"pauze"}  -> antwoord ack {"nr":7}
#define OND_NET_START       "net/start"        // {"server":"192.168.0.25","poort":1883} -> ack
#define OND_NET_STOP        "net/stop"         // {} -> ack
#define OND_RECEPT_ANTWOORD "recept/antwoord"  // {"ok":true}  of  {"ok":false,"reden":"..."}

/* ---- Niels ESP -> scherm ---- */
#define OND_TEMP            "ketel/temp"       // {"temp":65.3,"ok":true}   elke seconde
#define OND_NET_STATUS      "net/status"       // {"status":"uit|bezig|verbonden|fout","reden":"..."}
#define OND_RECEPT          "recept"           // {"naam":..,"kooktijd":40,"stappen":[{"temp":52,"min":5}],"hops":[{"naam":..,"min":30}]}
#define OND_ACK             "ack"              // {"nr":7}  of  {"voor":"net/start"}

/* Hoort een kant langer dan dit niets, dan gaat hij uit van een fout:
 * Niels ESP zet verwarming, pomp en zoemer uit; het scherm toont een sensorfout. */
#define NIELS_STIL_MAX_MS 2000
