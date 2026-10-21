/*
 * pid.h - PID-regelaar voor de verwarming
 *
 * Gebruikt brouw.pid_kp / pid_ki / pid_kd (aan te passen in Instellingen).
 * Fout in °C, uitgang in % vermogen (0..100).
 */
#pragma once

int  pid_bereken(float temp, float doel, float dt_s);   // elke dt_s seconden aanroepen
void pid_reset();                                       // bij stoppen of nieuwe fase
