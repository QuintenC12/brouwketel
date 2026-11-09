#include "scherm_uart.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/uart.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define UART_POORT UART_NUM_1

static bool gestart = false;
static char ontvang_regel[SCHERM_UART_REGEL_MAX];
static size_t aantal_tekens = 0;
static bool regel_weggooien = false;

//Alleen deze onderwerpen komen volgens de afspraken VAN het scherm.
static bool onderwerp_ontvangen_bekend(const char *onderwerp)
{
    return strcmp(onderwerp, "brouw/stuur") == 0 ||
           strcmp(onderwerp, "brouw/status") == 0 ||
           strcmp(onderwerp, "brouw/meld") == 0 ||
           strcmp(onderwerp, "net/start") == 0 ||
           strcmp(onderwerp, "net/stop") == 0 ||
           strcmp(onderwerp, "recept/antwoord") == 0;
}

//Alleen deze onderwerpen sturen wij NAAR het scherm.
static bool onderwerp_versturen_bekend(const char *onderwerp)
{
    return strcmp(onderwerp, "ketel/temp") == 0 ||
           strcmp(onderwerp, "net/status") == 0 ||
           strcmp(onderwerp, "recept") == 0 ||
           strcmp(onderwerp, "ack") == 0;
}

static bool geldig_json_object(const char *tekst)
{
    // De lokale eindpointer voorkomt gebruik van de gedeelde cJSON-foutpointer.
    const char *einde = NULL;
    cJSON *object = cJSON_ParseWithOpts(tekst, &einde, true);
    bool geldig = cJSON_IsObject(object);
    cJSON_Delete(object);
    return geldig;
}

esp_err_t scherm_uart_init(void)
{
    if (gestart)
    {
        return ESP_ERR_INVALID_STATE;
    }

    uart_config_t instellingen = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(UART_POORT, 2048, 1024, 0, NULL, 0);
    if (err != ESP_OK)
    {
        return err;
    }

    err = uart_param_config(UART_POORT, &instellingen);
    if (err == ESP_OK)
    {
        err = uart_set_pin(UART_POORT,
                           CONFIG_SCHERM_UART_TX_GPIO,
                           CONFIG_SCHERM_UART_RX_GPIO,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }

    if (err != ESP_OK)
    {
        uart_driver_delete(UART_POORT);
        return err;
    }

    aantal_tekens = 0;
    regel_weggooien = false;
    gestart = true;
    return ESP_OK;
}

esp_err_t scherm_uart_verstuur(const char *onderwerp, const char *json)
{
    if (!gestart)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (onderwerp == NULL || json == NULL ||
        !onderwerp_versturen_bekend(onderwerp))
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Echte regeleinden zouden een bericht in meerdere regels splitsen.
    if (strchr(json, '\n') != NULL || strchr(json, '\r') != NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char regel[SCHERM_UART_REGEL_MAX + 1];
    int lengte = snprintf(regel, sizeof(regel), "%s %s\n", onderwerp, json);
    if (lengte < 0 || lengte > SCHERM_UART_REGEL_MAX)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!geldig_json_object(json))
    {
        return ESP_ERR_INVALID_ARG;
    }

    // De complete regel in EEN aanroep versturen, ook met meerdere zendende taken.
    int verstuurd = uart_write_bytes(UART_POORT, regel, lengte);
    return verstuurd == lengte ? ESP_OK : ESP_FAIL;
}

esp_err_t scherm_uart_ontvang(scherm_uart_bericht_t *bericht,
                            TickType_t wachttijd)
{
    if (!gestart)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (bericht == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Dit bewaakt de totale wachttijd, in plaats van opnieuw per ontvangen teken.
    TimeOut_t timeout;
    TickType_t resterend = wachttijd;
    vTaskSetTimeOutState(&timeout);

    while (1)
    {
        uint8_t teken;
        int ontvangen = uart_read_bytes(UART_POORT, &teken, 1, resterend);
        if (ontvangen < 0)
        {
            return ESP_FAIL;
        }
        if (ontvangen == 0)
        {
            return ESP_ERR_TIMEOUT;
        }

        if (teken == '\n')
        {
            ontvang_regel[aantal_tekens] = '\0';
            aantal_tekens = 0;

            if (!regel_weggooien)
            {
                // De eerste spatie scheidt het onderwerp van de JSON-tekst.
                char *spatie = strchr(ontvang_regel, ' ');
                if (spatie != NULL)
                {
                    *spatie = '\0';
                    const char *json = spatie + 1;

                    if (onderwerp_ontvangen_bekend(ontvang_regel) &&
                        geldig_json_object(json))
                    {
                        strcpy(bericht->onderwerp, ontvang_regel);
                        strcpy(bericht->json, json);
                        return ESP_OK;
                    }
                }
            }

            regel_weggooien = false;
        }
        else if (teken != '\r' && !regel_weggooien)
        {
            if (teken == '\0' || aantal_tekens >= sizeof(ontvang_regel) - 1)
            {
                // Ook de REST van deze regel weggooien, tot aan '\n'.
                regel_weggooien = true;
            }
            else
            {
                ontvang_regel[aantal_tekens++] = (char)teken;
            }
        }

        if (wachttijd != 0 &&
            xTaskCheckForTimeOut(&timeout, &resterend) == pdTRUE)
        {
            return ESP_ERR_TIMEOUT;
        }
    }
}
