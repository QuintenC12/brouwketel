/*
 * pid.h - PID-regelaar voor de verwarming
 *
 * Kp, Ki en Kd komen van het scherm (Instellingen), mee in brouw/stuur.
 * Fout in °C, uitgang in % vermogen (0..100).
 */
#pragma once

int  pid_bereken(float temp, float doel, float dt_s, float kp, float ki, float kd);   // elke dt_s seconden aanroepen
void pid_reset(void);                                                                 // verwarming uit of sensorfout
