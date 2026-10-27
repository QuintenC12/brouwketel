#include <Arduino.h>
#include <lvgl.h>
#include "ui/ui.h"      // gegenereerd door EEZ Studio
#include "board.h"      // scherm + touch van de CrowPanel 7.0 (board_crowpanel7.cpp)
#include "brouwer.h"
#include "hardware.h"

static uint32_t my_tick() { return millis(); }

#ifdef DEMO
// Omgeving crowpanel7_demo: nep-ketel en nep-server (sim/sim_hardware.cpp),
// de tijd loopt SNELHEID keer sneller zodat je een hele brouwdag snel doorloopt.
void sim_netwerk();
#define SNELHEID 30
#else
#define SNELHEID 1
#endif

void setup() {
  Serial.begin(115200);  //seriele monitor starten
  delay(200);
  Serial.println("Start brouwcontroller");
  hw_init();  //pinnen klaarzetten
  lv_init();  //LVGL starten
  lv_tick_set_cb(my_tick);  //LVGL zeggen hoe laat het is
  if (!board_init()) {      //scherm (800x480 RGB) + touch (GT911) koppelen aan LVGL
    Serial.println("Scherm kon niet starten - zie meldingen hierboven");
  }
  ui_init();        // bouwt de schermen uit EEZ Studio op
  brouwer_init();   // startwaarden + scherm in de juiste toestand
}

void loop() {
  lv_timer_handler();   // LVGL tekenen en touch verwerken
  ui_tick();  //doet in dit project niks, maar EEZ verwacht dit
  hw_tick();  //I2C: Niels ESP zijn opdrachten geven en temperatuur/status ophalen
#ifdef DEMO
  sim_netwerk();        // nep-server: verbinden + demorecept sturen
#endif

  // Elke seconde: meten, verwarming regelen, brouwproces laten lopen
  static uint32_t vorige_seconde = millis(); //millis = aantal miliseconden sinds de start
  if (millis() - vorige_seconde >= 1000) {  //is er een seconde voorbij?
    vorige_seconde += 1000;
    for (int i = 0; i < SNELHEID; i++) {  //in de demo SNELHEID brouwseconden per echte seconde
      float temp = hw_lees_temperatuur(); //temperatuur wordt uit de HW gelezen
      int vermogen = hw_regel_verwarming(temp, brouwer_doel_temp());  //kijkt hoe warm het zou moeten zijn en regelt de verwarming
      brouwer_seconde(temp, vermogen);  //kijkt wat de volgende stap gaat zijn, als er iets 'belangrijks' gebeurt
    }
  }
  delay(5);
}
