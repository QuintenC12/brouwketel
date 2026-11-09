#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

// Maximaal 512 bytes per regel, inclusief de afsluitende '\n'.
#define SCHERM_UART_REGEL_MAX 512

typedef struct
{
    char onderwerp[24];
    char json[SCHERM_UART_REGEL_MAX];
} scherm_uart_bericht_t;

// UART1: 115200 baud, 8N1, geen flow control, RX-buffer van 2048 bytes.
// TX en RX zijn instelbaar via menuconfig -> Niels ESP (standaard 17 en 18).
// Roep dit eenmaal aan voordat je de taken start.
esp_err_t scherm_uart_init(void);

// Verstuur: onderwerp + spatie + JSON-object + '\n'.
// Geef json als tekst mee, zonder echte regeleinden en zonder '\n' achteraan.
// ESP_OK betekent: aangenomen door de UART-driver, geen ontvangstbevestiging.
esp_err_t scherm_uart_verstuur(const char *onderwerp, const char *json);

// Ontvang een volledig bericht van het scherm. Gebruik deze functie in EEN taak.
// wachttijd is in FreeRTOS-ticks: bijvoorbeeld pdMS_TO_TICKS(50).
// ESP_OK: geldig bericht; ESP_ERR_TIMEOUT: geen volledig geldig bericht op tijd.
// Een onvolledige regel blijft bewaard voor de volgende aanroep.
// Debugregels, onbekende onderwerpen, te lange regels en ongeldige JSON vervallen.
// De inhoud staat na succes in bericht->onderwerp en bericht->json.
esp_err_t scherm_uart_ontvang(scherm_uart_bericht_t *bericht,
                            TickType_t wachttijd);
