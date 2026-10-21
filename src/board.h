/*
 * board.h - scherm en touch van het gebruikte bord
 *
 * board_init() zet het scherm, de achtergrondverlichting en de touch op
 * en koppelt ze aan LVGL. Aanroepen na lv_init() en vóór ui_init().
 *
 * Implementatie voor de Elecrow CrowPanel 7.0" staat in board_crowpanel7.cpp.
 */
#pragma once
#include <stdbool.h>

bool board_init();
