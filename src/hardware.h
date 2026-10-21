/*
 * hardware.h - koppeling met de echte wereld
 *
 * Op de ESP32 staan deze functies in hardware.cpp, in de simulator in
 * sim/sim_hardware.cpp. De rest van de code weet niet welke van de twee draait.
 */
#pragma once

void  hw_init();

float hw_lees_temperatuur();                     // in °C; <= -100 betekent sensorfout
int   hw_regel_verwarming(float temp, float doel); // stuurt de verwarming, geeft vermogen in %
void  hw_pomp(bool aan);
void  hw_verwarming_uit();
void  hw_zoemer(bool aan);
bool  hw_sensor_ok();                            // true als de temperatuursensor weer reageert

void  net_verbind_server();                      // verbinding maken, wachten op maischschema
void  net_verbreek();
void  net_meld(const char *gebeurtenis);         // bijv. "start", "pauze", "stop", "hop", ...
void  net_meld_sg(int stap, float sg);           // gemeten SG na maischstap (0 = stap 1) naar de server

void  instellingen_opslaan();                    // IP, poort en PID-waarden in flash (Preferences)
void  instellingen_laden();
