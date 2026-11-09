/*
 * ProcesRegelaar (Niels ESP) - ESP-IDF, ESP32-S3
 *
 * Doet het werk aan de ketel en praat met twee kanten:
 *   - het scherm (CrowPanel) via UART1, regels "onderwerp {json}"
 *     (component scherm_uart, afspraken in niels_uart.h)
 *   - de MQTT-server via wifi (net.c), zodat de webapp alles ziet
 *
 * Aan de ketel: PT100 via MAX31865 (temperatuur), SSR (verwarming),
 * pomp en zoemer.
 *
 * Wie doet wat:
 *   Het scherm rekent zelf de PID uit en stuurt 5x per seconde
 *   brouw/stuur {"vermogen":40,"pomp":true,"zoemer":false}.
 *   Deze ESP zet "vermogen" (0..100 %) om in een venster van 2 s waarin
 *   de SSR zoveel % aan staat, en zet pomp en zoemer aan of uit.
 *
 * Taken:
 *   temp_task     elke seconde temperatuur meten en sturen (ketel/temp)
 *   receive_task  berichten van het scherm ontvangen en verwerken
 *   ketel_task    SSR, pomp en zoemer aansturen + veiligheid
 *   net (net.c)   wifi/MQTT bijhouden
 *
 * Veiligheid: komt er NIELS_STIL_MAX_MS (2 s) geen brouw/stuur binnen,
 * dan gaan verwarming, pomp en zoemer uit.
 *
 * Pinnen en wifi: idf.py menuconfig -> "Niels ESP" en
 * "Niels ESP: wifi en ketel".
 */
#include <stdio.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "max31865.h"
#include "net.h"
#include "niels_uart.h"
#include "scherm_uart.h"

#define SPI_HOST     SPI2_HOST
#define PIN_NUM_MISO 13
#define PIN_NUM_MOSI 1
#define PIN_NUM_CLK  12
#define PIN_NUM_CS   10

#define SSR_VENSTER_MS 2000     // vermogen 40 % = 800 ms aan, 1200 ms uit

TaskHandle_t temp_handle;
max31865_t sensor;

// Wat het scherm wil (receive_task schrijft, ketel_task leest)
static volatile int        vermogen = 0;      // 0..100 %
static volatile bool       pomp_aan = false;
static volatile bool       zoemer_aan = false;
static volatile TickType_t laatste_stuur = 0;
static volatile bool       ooit_stuur = false;

// Laatst behandelde meldnummers, om herhalingen te herkennen
static int last_message[8] = {0};
static int next_index = 0;

static bool message_is_new(int nr)
{
    for(int i = 0; i < 8; i++)
    {
        if(last_message[i] == nr)
        {
            return false;
        }
    }
    last_message[next_index] = nr;
    next_index = (next_index + 1) % 8;
    return true;
}

esp_err_t spi_init(void)
{
    max31865_config_t config = {
        .spi_host = SPI_HOST,
        .pin_sck = PIN_NUM_CLK,
        .pin_mosi = PIN_NUM_MOSI,
        .pin_miso = PIN_NUM_MISO,
        .pin_cs = PIN_NUM_CS,
        .rtd_nominal_ohms = 100.0f,
        .reference_resistor_ohms = 430.0f,
        .temperature_offset_c = 0.0f,
        .filter_50hz = true,    // in Europa is het net 50 Hz
    };

    esp_err_t err = max31865_init(&sensor, &config);
    if (err != ESP_OK)
    {
        printf("Failed to initialize MAX31865: %s\n", esp_err_to_name(err));
    }
    return err;
}

/* ---------------- temperatuur ---------------- */

void temp_task(void *parameters)
{
    // spi_init() gebeurt alleen hier: een tweede keer weigert ESP-IDF,
    // omdat de SPI-bus dan al gestart is.
    bool sensor_klaar = spi_init() == ESP_OK;

    while (1)
    {
        if (!sensor_klaar)
        {
            // Niet stoppen: het scherm moet "sensorfout" kunnen tonen,
            // dus we blijven ok:false sturen en proberen opnieuw.
            sensor_klaar = spi_init() == ESP_OK;
        }

        float temperature_c = 0.0f;
        esp_err_t err = sensor_klaar ? max31865_read_temperature(&sensor, &temperature_c)
                                     : ESP_ERR_INVALID_STATE;

        char json[64];
        if (err == ESP_OK && isfinite(temperature_c))
        {
            snprintf(json, sizeof(json), "{\"temp\":%.1f,\"ok\":true}", temperature_c);
        }
        else
        {
            snprintf(json, sizeof(json), "{\"temp\":null,\"ok\":false}");
            printf("Uitlezen mislukt: %s\n", esp_err_to_name(err));
        }

        err = scherm_uart_verstuur(OND_TEMP, json);
        if (err != ESP_OK)
        {
            printf("Temperatuur versturen mislukt: %s\n", esp_err_to_name(err));
        }
        net_publiceer(OND_TEMP, json);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---------------- ketel: SSR, pomp, zoemer ---------------- */

static void alles_uit(void)
{
    gpio_set_level(CONFIG_NIELS_PIN_SSR, 0);
    gpio_set_level(CONFIG_NIELS_PIN_POMP, 0);
    gpio_set_level(CONFIG_NIELS_PIN_ZOEMER, 0);
}

static void uitgangen_init(void)
{
    gpio_config_t uit = {
        .pin_bit_mask = (1ULL << CONFIG_NIELS_PIN_SSR) | (1ULL << CONFIG_NIELS_PIN_POMP) |
                        (1ULL << CONFIG_NIELS_PIN_ZOEMER),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&uit);
    alles_uit();
}

void ketel_task(void *parameters)
{
    while (1)
    {
        bool stil = !ooit_stuur ||
                    xTaskGetTickCount() - laatste_stuur > pdMS_TO_TICKS(NIELS_STIL_MAX_MS);
        if (stil)
        {
            // Veiligheid: geen brouw/stuur meer van het scherm = alles uit
            alles_uit();
        }
        else
        {
            uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
            bool ssr = (ms % SSR_VENSTER_MS) < (uint32_t)vermogen * SSR_VENSTER_MS / 100;
            gpio_set_level(CONFIG_NIELS_PIN_SSR, ssr);
            gpio_set_level(CONFIG_NIELS_PIN_POMP, pomp_aan);
            gpio_set_level(CONFIG_NIELS_PIN_ZOEMER, zoemer_aan);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ---------------- berichten van het scherm ---------------- */

static void stuur_ack_voor(const char *voor)
{
    char ack[48];
    snprintf(ack, sizeof(ack), "{\"voor\":\"%s\"}", voor);
    esp_err_t err = scherm_uart_verstuur(OND_ACK, ack);
    if (err != ESP_OK)
    {
        printf("Ack versturen mislukt: %s\n", esp_err_to_name(err));
    }
}

static void bericht_verwerken(const scherm_uart_bericht_t *bericht)
{
    const char* einde = NULL;
    cJSON *data = cJSON_ParseWithOpts(bericht->json, &einde, true);
    if (data == NULL)
    {
        return;
    }

    if (strcmp(bericht->onderwerp, OND_STUUR) == 0)
    {
        // {"vermogen":40,"pomp":true,"zoemer":false}
        // Ontbreekt een veld, dan staat dat deel uit (veilige kant).
        cJSON* v = cJSON_GetObjectItemCaseSensitive(data, "vermogen");
        int nieuw = cJSON_IsNumber(v) ? (int)v->valuedouble : 0;
        if (nieuw < 0) nieuw = 0;
        if (nieuw > 100) nieuw = 100;
        vermogen = nieuw;
        pomp_aan = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "pomp"));
        zoemer_aan = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "zoemer"));
        laatste_stuur = xTaskGetTickCount();
        ooit_stuur = true;
    }
    else if (strcmp(bericht->onderwerp, OND_MELD) == 0)
    {
        cJSON* nummer = cJSON_GetObjectItemCaseSensitive(data, "nr");
        if (cJSON_IsNumber(nummer) && nummer->valuedouble >= 1 &&
            nummer->valuedouble <= INT_MAX &&
            nummer->valuedouble == (int)nummer->valuedouble)
        {
            int nr = (int)nummer->valuedouble;
            char ack[32];
            snprintf(ack, sizeof(ack), "{\"nr\":%d}", nr);

            //ALTIJD bevestigen, ook wanneer dezelfde melding opnieuw komt.
            esp_err_t err = scherm_uart_verstuur(OND_ACK, ack);
            if (err != ESP_OK)
            {
                printf("Ack versturen mislukt: %s\n", esp_err_to_name(err));
            }

            if (message_is_new(nr))
            {
                printf("Nieuwe melding: %s\n", bericht->json);
                net_publiceer(OND_MELD, bericht->json);
            }
        }
    }
    else if (strcmp(bericht->onderwerp, OND_STATUS) == 0 ||
             strcmp(bericht->onderwerp, OND_RECEPT_ANTWOORD) == 0)
    {
        // Alleen doorgeven aan de webapp
        net_publiceer(bericht->onderwerp, bericht->json);
    }
    else if (strcmp(bericht->onderwerp, OND_NET_START) == 0)
    {
        // {"server":"192.168.0.25","poort":1883}
        cJSON* server = cJSON_GetObjectItemCaseSensitive(data, "server");
        cJSON* poort = cJSON_GetObjectItemCaseSensitive(data, "poort");
        stuur_ack_voor(OND_NET_START);
        if (cJSON_IsString(server))
        {
            net_start(server->valuestring, cJSON_IsNumber(poort) ? (int)poort->valuedouble : 1883);
        }
    }
    else if (strcmp(bericht->onderwerp, OND_NET_STOP) == 0)
    {
        stuur_ack_voor(OND_NET_STOP);
        net_stop();
    }

    cJSON_Delete(data);
}

void receive_task(void *parameters)
{
    bool timeout_reported = false;

    while (1)
    {
        scherm_uart_bericht_t bericht;
        esp_err_t err = scherm_uart_ontvang(&bericht, pdMS_TO_TICKS(50));

        if(err == ESP_OK)
        {
            bericht_verwerken(&bericht);
            if(strcmp(bericht.onderwerp, OND_STUUR) == 0)
            {
                timeout_reported = false;
            }
        }
        else if (err != ESP_ERR_TIMEOUT)
        {
            printf("UART-ontvangst fout: %s\n", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        //Tijd tot timeout (de uitgangen zelf gaan uit in ketel_task):
        bool stil = !ooit_stuur ||
                    xTaskGetTickCount() - laatste_stuur >= pdMS_TO_TICKS(NIELS_STIL_MAX_MS);
        if(stil && !timeout_reported)
        {
            printf("Geen brouw/stuur ontvangen gedurende 2 seconden: alles uit.\n");
            // Het scherm is misschien herstart en telt dan weer vanaf 1.
            memset(last_message, 0, sizeof(last_message));
            next_index = 0;
            timeout_reported = true;
        }
    }
}

void app_main(void)
{
    uitgangen_init();                 // eerst alles uit
    scherm_uart_init();
    net_init();

    xTaskCreate(ketel_task, "Ketel task", 1024*3, NULL, 6, NULL);
    xTaskCreate(temp_task, "Temperature task", 1024*8, NULL, 5, &temp_handle);
    xTaskCreate(receive_task, "Receive task", 1024*8, NULL, 5, NULL);
}
