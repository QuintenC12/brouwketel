/*
 * pid.c - PID-regelaar voor de verwarming
 *
 *   vermogen = Kp * fout + Ki * som(fout * dt) - Kd * d(temp)/dt
 *
 * - De D-term rekent op de gemeten temperatuur (geen sprong bij een nieuwe doeltemperatuur).
 * - Anti-windup: de I-term telt niet op zolang de uitgang op 0 of 100 % staat.
 *
 * Stond vroeger op het scherm; nu regelt Niels ESP zelf, vlak bij de sensor.
 * Kp, Ki en Kd komen van het scherm (Instellingen), mee in brouw/stuur.
 */
#include <stdbool.h>
#include "pid.h"

static float integraal = 0;      // Ki * som(fout * dt), al in %
static float vorige_temp = 0;
static float vorige_doel = 0;
static bool  eerste = true;

void pid_reset(void) {
    integraal = 0;
    eerste = true;
}

static float begrens(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

int pid_bereken(float temp, float doel, float dt_s, float kp, float ki, float kd) {
    if (doel <= 0) { pid_reset(); return 0; }
    if (eerste || doel != vorige_doel) { vorige_temp = temp; vorige_doel = doel; eerste = false; }

    float fout = doel - temp;
    float afgeleide = dt_s > 0 ? (temp - vorige_temp) / dt_s : 0;
    vorige_temp = temp;

    // Alleen integreren als de uitgang niet al vol open (of dicht) staat,
    // anders loopt de I-term op tijdens het opwarmen en schiet je voorbij.
    float pd = kp * fout - kd * afgeleide;
    float nieuw = begrens(integraal + ki * fout * dt_s, 0, 100);
    if (pd + nieuw <= 100 && pd + nieuw >= 0) integraal = nieuw;

    float uit = pd + integraal;
    return (int)(begrens(uit, 0, 100) + 0.5f);
}
