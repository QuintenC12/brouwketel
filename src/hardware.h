/*
 * hardware.h - koppeling met de echte wereld
 *
 * Op de ESP32 staan deze functies in uart.cpp (alles gaat via Niels ESP) en
 * instellingen.cpp, in de simulator in sim/sim_hardware.cpp. De rest van de
 * code weet niet welke van de twee draait.
 */
#pragma once

void  hw_init();
void  hw_tick();                                 // elke loop: berichten met Niels ESP (UART) lezen en sturen

float hw_lees_temperatuur();                     // in °C; <= -100 betekent sensorfout
int   hw_regel_verwarming(float temp, float doel); // geeft het doel door aan Niels ESP (die doet de PID), geeft het gemelde vermogen in %
void  hw_pomp(bool aan);
void  hw_verwarming_uit();
/* Zoemer: het scherm zegt alleen wat voor geluid (gaat mee in brouw/stuur).
 * Hoe het klinkt, beslist Niels ESP; het blijft gaan tot ZOEMER_UIT. */
enum Zoemer { ZOEMER_UIT, ZOEMER_MELDING, ZOEMER_ALARM };
void  hw_zoemer(Zoemer soort);                   // bijv. hop toevoegen = melding, sensorfout = alarm
bool  hw_sensor_ok();                            // true als de temperatuursensor weer reageert

void  net_verbind_server();                      // verbinding maken, wachten op maischschema
void  net_verbreek();
void  net_meld(const char *gebeurtenis);         // bijv. "start", "pauze", "stop", "hop", ...
void  net_meld_sg(int stap, float sg);           // gemeten SG na maischstap (0 = stap 1) naar de server

void  instellingen_opslaan();                    // IP, poort en PID-waarden in flash (Preferences)
void  instellingen_laden();
