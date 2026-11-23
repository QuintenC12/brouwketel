/*
 * niels_uart.h - afspraken voor de kabel tussen het scherm en Niels ESP
 *
 * Dit bestand is voor BEIDE kanten: het staat ook in niels_esp/main/.
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

/* Twee soorten berichten:
 *  - toestand: wordt steeds opnieuw gestuurd, dus een verloren bericht is geen probleem
 *    (behalve net/toestand: alleen bij een verandering, het scherm vraagt hem op met net/vraag)
 *  - gebeurtenis (brouw/meld): gebeurt maar één keer, dus met nr en ack, en herhaald tot de ack komt */

/* ---- scherm -> Niels ESP ---- */
#define OND_STUUR           "brouw/stuur"      // {"doel":65,"kp":20,"ki":0.05,"kd":10,"pomp":true,"zoemer":"uit"}   5x per seconde
                                               // doel = doeltemperatuur in °C (0 = verwarming uit); Niels ESP doet de PID
                                               // zoemer = "uit", "melding" of "alarm"; Niels ESP maakt het piepritme
#define OND_STATUS          "brouw/status"     // {"fase":"maischen","stap":2,"doel":63,"rest":754,"pauze":false}   elke seconde
#define OND_MELD            "brouw/meld"       // {"nr":7,"naam":"pauze"}  -> antwoord ack {"nr":7}
                                               // ook het antwoord op een recept: "recept_ok" of "recept_fout" + "reden"
#define OND_NET_START       "net/start"        // {}   recept vragen (antwoord komt via net/status); de broker komt uit de hotspot van Niels ESP
#define OND_NET_STOP        "net/stop"         // {}
#define OND_NET_VRAAG       "net/vraag"        // {}   stuur meteen net/toestand terug (scherm net opgestart of contact terug)
#define OND_NET_MODUS       "net/modus"        // {"modus":"configuratie"} of {"modus":"werking"}   alleen als de schakelaar verandert
                                               // configuratie = hotspot + wifi-formulier open (wifi en MQTT gaan uit)
                                               // Niels ESP voert het gewoon uit; antwoord via net/toestand

/* ---- Niels ESP -> scherm ---- */
#define OND_TEMP            "ketel/temp"       // {"temp":65.3,"ok":true,"vermogen":40}   elke seconde (vermogen in % van de PID)
#define OND_NET_STATUS      "net/status"       // {"status":"uit|bezig|verbonden|fout","reden":"..."}
#define OND_RECEPT          "recept"           // {"naam":..,"kooktijd":40,"stappen":[{"temp":52,"min":5}],"hops":[{"naam":..,"min":30}]}
#define OND_ACK             "ack"              // {"nr":7}   antwoord op brouw/meld
#define OND_NET_TOESTAND    "net/toestand"     // {"wifi":true,"mqtt":true,"online":true,"modus":"werking","ssid":"projectnetwork5G","server":"192.168.0.10","poort":1883}
                                               // alleen bij een verandering (wifi/mqtt/server/modus/instellingen) en als antwoord op net/vraag
                                               // wifi = verbonden met het wifi-netwerk, mqtt = verbonden met de broker
                                               // online = de server van de webapp is er (retained brouwer/server/status = "online")
                                               // modus = "configuratie" als de hotspot open staat, anders "werking"
                                               // server, poort = de broker uit het hotspot-formulier ("" en 0 als er nog geen is);
                                               // het scherm toont ze alleen op Instellingen

/* Hoort een kant langer dan dit niets, dan gaat hij uit van een fout:
 * Niels ESP zet verwarming, pomp en zoemer uit; het scherm toont een sensorfout. */
#define NIELS_STIL_MAX_MS 2000
