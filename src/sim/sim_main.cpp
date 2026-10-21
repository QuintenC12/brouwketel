/*
 * sim_main.cpp - draait je EEZ-UI in een venster op de Mac (omgeving "simulator")
 *
 * Wordt NIET mee gecompileerd voor de ESP32 (zie build_src_filter in platformio.ini).
 * Alle code is dezelfde als op de ESP32; alleen hardware.cpp is vervangen
 * door sim_hardware.cpp, en de tijd loopt SNELHEID keer sneller.
 */
#include <unistd.h>
#include <lvgl.h>
#include "ui/ui.h"
#include "brouwer.h"
#include "hardware.h"

#define SNELHEID 30   // 1 echte seconde = 30 brouwseconden (30 min rust = 1 min)

void sim_netwerk();   // sim_hardware.cpp

int main() {
    lv_init();

    lv_display_t *disp = lv_sdl_window_create(800, 480);
    lv_sdl_window_set_title(disp, "Brouwcontroller - simulator");
    lv_sdl_mouse_create();   // muisklik = aanraking

    hw_init();
    ui_init();
    brouwer_init();

    uint32_t vorige_seconde = lv_tick_get();
    while (true) {
        uint32_t wacht = lv_timer_handler();
        ui_tick();
        sim_netwerk();

        // Precies zoals main.cpp op de ESP32, maar SNELHEID keer per seconde
        if (lv_tick_get() - vorige_seconde >= 1000) {
            vorige_seconde += 1000;
            for (int i = 0; i < SNELHEID; i++) {
                float temp = hw_lees_temperatuur();
                int vermogen = hw_regel_verwarming(temp, brouwer_doel_temp());
                brouwer_seconde(temp, vermogen);
            }
        }
        usleep((wacht < 5 ? wacht : 5) * 1000);
    }
}
