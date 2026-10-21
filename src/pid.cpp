/*
 * pid.cpp - PID-regelaar voor de verwarming
 *
 *   vermogen = Kp * fout + Ki * som(fout * dt) + Kd * d(fout)/dt
 *
 * - De D-term rekent op de gemeten temperatuur (geen sprong bij een nieuwe doeltemperatuur).
 * - Anti-windup: de I-term telt niet op zolang de uitgang op 0 of 100 % staat.
 */
#include "pid.h"
#include "brouwer.h"

static float integraal = 0;      // Ki * som(fout * dt), al in %
static float vorige_temp = 0;
static float vorige_doel = 0;
static bool  eerste = true;

void pid_reset() {
    integraal = 0;
    eerste = true;
}

static float begrens(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

int pid_bereken(float temp, float doel, float dt_s) {
    if (doel <= 0) { pid_reset(); return 0; }
    if (eerste || doel != vorige_doel) { vorige_temp = temp; vorige_doel = doel; eerste = false; }

    float fout = doel - temp;
    float afgeleide = dt_s > 0 ? (temp - vorige_temp) / dt_s : 0;
    vorige_temp = temp;

    // Alleen integreren als de uitgang niet al vol open (of dicht) staat,
    // anders loopt de I-term op tijdens het opwarmen en schiet je voorbij.
    float pd = brouw.pid_kp * fout - brouw.pid_kd * afgeleide;
    float nieuw = begrens(integraal + brouw.pid_ki * fout * dt_s, 0, 100);
    if (pd + nieuw <= 100 && pd + nieuw >= 0) integraal = nieuw;

    float uit = pd + integraal;
    return (int)(begrens(uit, 0, 100) + 0.5f);
}
