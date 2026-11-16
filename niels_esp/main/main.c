/*
 * ProcesRegelaar (Niels ESP) - ESP-IDF 5.4, ESP32-S3
 *
 * Doet het werk aan de ketel en praat met twee kanten:
 *   - het scherm (CrowPanel) via UART1, regels "onderwerp {json}"
 *     (component scherm_uart, afspraken in niels_uart.h en het regelboekje)
 *   - de MQTT-server via wifi (wifi: component brouw_netwerk, MQTT: net.c)
 *
 * Aan de ketel: PT100 via MAX31865 (temperatuur), SSR (verwarming),
 * pomp en zoemer.
 *
 * Wie doet wat:
 *   Het scherm stuurt 5x per seconde
 *     brouw/stuur {"doel":65,"kp":20,"ki":0.05,"kd":10,"pomp":true,"zoemer":"uit"}
 *   Deze ESP doet de PID (pid.c): elke seconde rekent hij met de gemeten
 *   temperatuur een vermogen uit (0..100 %). Dat vermogen wordt een venster
 *   van 2 s waarin de SSR zoveel % aan staat. Het vermogen gaat terug naar
 *   het scherm in ketel/temp.
 *
 * Taken:
 *   temp_task     elke seconde meten, PID uitrekenen, ketel/temp sturen
 *   receive_task  berichten van het scherm ontvangen en verwerken
 *   ketel_task    SSR, pomp en zoemer aansturen + veiligheid
 *   net (net.c)   MQTT starten en stoppen
 *
 * Veiligheid: komt er NIELS_STIL_MAX_MS (2 s) geen brouw/stuur binnen,
 * dan gaan verwarming, pomp en zoemer uit, gaat het doel op 0 en wordt de
 * PID gereset.
 *
 * Pinnen en setupwachtwoord: idf.py menuconfig -> "Niels ESP" en
 * "Niels ESP: ketel en wifi-setup".
 */
#include <stdio.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "brouw_netwerk.h"
#include "max31865.h"
#include "net.h"
#include "niels_uart.h"
#include "pid.h"
#include "scherm_uart.h"

#define SPI_HOST     SPI2_HOST
#define PIN_NUM_MISO 13
#define PIN_NUM_MOSI 1
#define PIN_NUM_CLK  12
#define PIN_NUM_CS   10

#define TEMP_MS        1000     // zo vaak meten, PID rekenen en ketel/temp sturen
#define SSR_VENSTER_MS 2000     // vermogen 40 % = 800 ms aan, 1200 ms uit

// Piepritme van de zoemer (regelboekje, brouw/stuur)
#define MELDING_PERIODE_MS 3000 // melding: om de 3 s ...
#define MELDING_PIEP_MS    150  // ... drie korte piepjes van 150 ms aan, 150 ms uit
#define MELDING_PIEPJES    3
#define ALARM_PIEP_MS      200  // alarm: 200 ms aan, 200 ms uit

TaskHandle_t temp_handle;
max31865_t sensor;

typedef enum { ZOEMER_UIT, ZOEMER_MELDING, ZOEMER_ALARM } zoemer_t;

// Wat het scherm wil (receive_task schrijft, temp_task en ketel_task lezen)
static volatile float      doel = 0;                     // °C, 0 = verwarming uit
static volatile float      kp = 20, ki = 0.05f, kd = 10; // tot het scherm de echte waarden stuurt
static volatile bool       pomp_aan = false;
static volatile zoemer_t   zoemer = ZOEMER_UIT;
static volatile uint32_t   zoemer_sinds = 0;             // begin van het piepritme
static volatile TickType_t laatste_stuur = 0;
static volatile bool       ooit_stuur = false;

// Uitkomst van de PID (temp_task schrijft, ketel_task leest)
static volatile int vermogen = 0;                        // 0..100 %

// Laatst behandelde meldnummers, om herhalingen te herkennen
static int last_message[8] = {0};
static int next_index = 0;

static uint32_t nu_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* Geen brouw/stuur in de laatste 2 s (of nog nooit)? */
static bool scherm_stil(void)
{
    return !ooit_stuur ||
           xTaskGetTickCount() - laatste_stuur > pdMS_TO_TICKS(NIELS_STIL_MAX_MS);
}

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
        .filter_50hz = true,    // in Europa is het stroomnet 50 Hz
    };

    esp_err_t err = max31865_init(&sensor, &config);
    if (err != ESP_OK)
    {
        printf("Failed to initialize MAX31865: %s\n", esp_err_to_name(err));
    }
    return err;
}

/* ---------------- temperatuur en PID ---------------- */

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
        bool ok = err == ESP_OK && isfinite(temperature_c);

        // PID: zonder geldige meting, zonder doel of zonder scherm: verwarming uit
        float d = doel;
        if (ok && d > 0 && !scherm_stil())
        {
            vermogen = pid_bereken(temperature_c, d, TEMP_MS / 1000.0f, kp, ki, kd);
        }
        else
        {
            pid_reset();
            vermogen = 0;
        }

        char json[80];
        if (ok)
        {
            snprintf(json, sizeof(json), "{\"temp\":%.1f,\"ok\":true,\"vermogen\":%d}",
                     temperature_c, vermogen);
        }
        else
        {
            snprintf(json, sizeof(json), "{\"temp\":null,\"ok\":false,\"vermogen\":0}");
            printf("Uitlezen mislukt: %s\n", esp_err_to_name(err));
        }

        err = scherm_uart_verstuur(OND_TEMP, json);
        if (err != ESP_OK)
        {
            printf("Temperatuur versturen mislukt: %s\n", esp_err_to_name(err));
        }
        net_publiceer(OND_TEMP, json);

        vTaskDelay(pdMS_TO_TICKS(TEMP_MS));
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

/* Piepritme: zo hoor je het verschil tussen een melding en een alarm.
 *   melding: om de 3 s drie korte piepjes   (bijv. hop toevoegen, koken klaar)
 *   alarm:   snel aan-uit                    (bijv. sensorfout)
 * Het ritme begint opnieuw (met een piep) zodra de stand verandert. */
static bool zoemer_piept(void)
{
    uint32_t t = nu_ms() - zoemer_sinds;
    switch (zoemer)
    {
    case ZOEMER_MELDING:
        t %= MELDING_PERIODE_MS;
        return t < MELDING_PIEPJES * 2 * MELDING_PIEP_MS &&
               t % (2 * MELDING_PIEP_MS) < MELDING_PIEP_MS;
    case ZOEMER_ALARM:
        return t % (2 * ALARM_PIEP_MS) < ALARM_PIEP_MS;
    default:
        return false;
    }
}

void ketel_task(void *parameters)
{
    while (1)
    {
        if (scherm_stil())
        {
            // Veiligheid: geen brouw/stuur meer van het scherm = alles uit.
            // temp_task zet het vermogen op 0 en reset de PID.
            doel = 0;
            vermogen = 0;
            pomp_aan = false;
            zoemer = ZOEMER_UIT;
            alles_uit();
        }
        else
        {
            bool ssr = (nu_ms() % SSR_VENSTER_MS) < (uint32_t)vermogen * SSR_VENSTER_MS / 100;
            gpio_set_level(CONFIG_NIELS_PIN_SSR, ssr);
            gpio_set_level(CONFIG_NIELS_PIN_POMP, pomp_aan);
            gpio_set_level(CONFIG_NIELS_PIN_ZOEMER, zoemer_piept());
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ---------------- berichten van het scherm ---------------- */

static float kommagetal(const cJSON *o, const char *naam, float standaard)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, naam);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : standaard;
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
        // {"doel":65,"kp":20,"ki":0.05,"kd":10,"pomp":true,"zoemer":"uit"}
        // Ontbreekt doel, dan gaat de verwarming uit (veilige kant).
        // Ontbreekt kp, ki of kd, dan blijft de vorige waarde.
        doel = kommagetal(data, "doel", 0);
        kp = kommagetal(data, "kp", kp);
        ki = kommagetal(data, "ki", ki);
        kd = kommagetal(data, "kd", kd);
        pomp_aan = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "pomp"));

        // "uit", "melding" of "alarm"; een onbekende waarde = "uit"
        const char *z = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data, "zoemer"));
        zoemer_t nieuw = z == NULL                  ? ZOEMER_UIT
                       : strcmp(z, "melding") == 0  ? ZOEMER_MELDING
                       : strcmp(z, "alarm") == 0    ? ZOEMER_ALARM
                       :                              ZOEMER_UIT;
        if (nieuw != zoemer)
        {
            zoemer_sinds = nu_ms();             // ritme begint opnieuw met een piep
        }
        zoemer = nieuw;

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

            // Elk nummer maar één keer doorgeven aan MQTT.
            // Ook recept_ok en recept_fout komen zo bij de server.
            if (message_is_new(nr))
            {
                printf("Nieuwe melding: %s\n", bericht->json);
                net_publiceer(OND_MELD, bericht->json);
            }
        }
    }
    else if (strcmp(bericht->onderwerp, OND_STATUS) == 0)
    {
        // Alleen voor de webapp: ongewijzigd doorsturen naar MQTT.
        net_publiceer(OND_STATUS, bericht->json);
    }
    else if (strcmp(bericht->onderwerp, OND_NET_START) == 0)
    {
        // {"server":"192.168.0.25","poort":1883}. Geen ack: het antwoord
        // komt via net/status. Zonder server gebruikt net.c de broker uit
        // het wifi-formulier.
        cJSON* server = cJSON_GetObjectItemCaseSensitive(data, "server");
        cJSON* poort = cJSON_GetObjectItemCaseSensitive(data, "poort");
        net_start(cJSON_IsString(server) ? server->valuestring : NULL,
                  cJSON_IsNumber(poort) ? (int)poort->valuedouble : 0);
    }
    else if (strcmp(bericht->onderwerp, OND_NET_STOP) == 0)
    {
        // Geen ack. MQTT stopt, de wifi blijft verbonden.
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
        if(scherm_stil() && !timeout_reported)
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

    // Wifi: eigen hotspot met formulier bij de eerste start, daarna stationmodus.
    const esp_err_t fout = brouw_netwerk_start(CONFIG_NIELS_SETUP_WACHTWOORD, false);
    if (fout != ESP_OK)
    {
        ESP_LOGE("main", "Netwerkstart: %s", esp_err_to_name(fout));
    }
    net_init();                       // voor de taken: die publiceren meteen

    xTaskCreate(ketel_task, "Ketel task", 1024*3, NULL, 6, NULL);
    xTaskCreate(temp_task, "Temperature task", 1024*8, NULL, 5, &temp_handle);
    xTaskCreate(receive_task, "Receive task", 1024*8, NULL, 5, NULL);
}
