/**
 * lv_conf.h voor LVGL 9.x - afgestemd op het EEZ Studio-brouwproject
 * Alles wat hier niet staat, krijgt de standaardwaarde van LVGL.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* Kleurdiepte: 16 bit */
#define LV_COLOR_DEPTH 16

/* Geheugen: het project maakt bij de start 14 schermen met ~430 objecten
 * aan, goed voor ~175 KB. */
#ifdef SIMULATOR
  /* Mac: gewone malloc */
  #define LV_USE_STDLIB_MALLOC  LV_STDLIB_CLIB
#else
  /* ESP32-S3: eigen geheugenblok van 512 KB in de PSRAM (8 MB), zodat het
   * snelle interne RAM vrij blijft voor wifi en de rest van het programma */
  #define LV_USE_STDLIB_MALLOC  LV_STDLIB_BUILTIN
  #define LV_MEM_SIZE           (512 * 1024U)
  #define LV_MEM_POOL_INCLUDE   <esp32-hal-psram.h>
  #define LV_MEM_POOL_ALLOC     ps_malloc
#endif
#define LV_USE_STDLIB_STRING  LV_STDLIB_CLIB

/* printf van de C-bibliotheek: nodig voor "%.1f" in lv_label_set_text_fmt */
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#ifdef SIMULATOR
  /* Simulator op de Mac: venster via SDL2 */
  #define LV_USE_SDL 1
  #define LV_SDL_INCLUDE_PATH <SDL2/SDL.h>
#endif
/* ESP32 (CrowPanel 7.0): scherm en touch worden in src/board_crowpanel7.cpp
 * zelf aangestuurd, daarvoor is hier geen LVGL-driver nodig. */

/* Logging naar de seriële monitor */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

/* QR-code op het scherm "Web app" */
#define LV_USE_QRCODE 1

/* Alle Montserrat-groottes die in je EEZ-ontwerp gebruikt worden. */
#define LV_FONT_MONTSERRAT_8  1
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_26 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_30 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_34 1
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_MONTSERRAT_38 1
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_MONTSERRAT_42 1
#define LV_FONT_MONTSERRAT_44 1
#define LV_FONT_MONTSERRAT_46 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#endif /* LV_CONF_H */