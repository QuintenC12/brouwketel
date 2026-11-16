#pragma once

/*
 * net.h - MQTT voor Niels ESP
 *
 * Wifi zelf regelt de component brouw_netwerk (hotspot + formulier, daarna
 * stationmodus). Dit deel start alleen de MQTT-client, op vraag van het scherm:
 *   net/start {"server":"192.168.0.25","poort":1883}  -> verbinden
 *   net/stop {}                                        -> MQTT stoppen (wifi blijft)
 * De toestand gaat terug met net/status {"status":"uit|bezig|verbonden|fout","reden":".."}.
 */

// Eenmaal aanroepen na scherm_uart_init() en brouw_netwerk_start().
void net_init(void);

// Vraag om met de MQTT-server te verbinden. server mag NULL of leeg zijn:
// dan gebruikt Niels ESP de broker uit het wifi-formulier.
void net_start(const char *server, int poort);

// MQTT stoppen. De wifi-verbinding blijft bestaan.
void net_stop(void);

// Bericht doorsturen naar MQTT op "brouwketel/" + onderwerp.
// Een brouw/meld zonder verbinding wordt bewaard en later verstuurd;
// temperatuur en status komen toch vanzelf opnieuw.
void net_publiceer(const char *onderwerp, const char *json);
