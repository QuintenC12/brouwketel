#pragma once

/*
 * net.h - wifi en MQTT voor Niels ESP
 *
 * Het scherm vraagt met net/start om met de MQTT-server te verbinden en met
 * net/stop om te stoppen. De toestand gaat terug naar het scherm met
 * net/status {"status":"uit|bezig|verbonden|fout","reden":"..."}.
 *
 * Wifi-naam en wachtwoord: idf.py menuconfig -> "Niels ESP: wifi en ketel".
 */

// Eenmaal aanroepen na scherm_uart_init(): start wifi en de net-taak.
void net_init(void);

// Verbinden met de MQTT-server (bv. "192.168.0.25", 1883).
void net_start(const char *server, int poort);

// Verbinding verbreken.
void net_stop(void);

// Bericht doorsturen naar MQTT op "brouwketel/" + onderwerp.
// Een brouw/meld zonder verbinding wordt bewaard en later verstuurd,
// de rest (temperatuur, status) komt toch vanzelf opnieuw.
void net_publiceer(const char *onderwerp, const char *json);
