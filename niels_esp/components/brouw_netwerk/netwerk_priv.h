#ifndef BROUW_NETWERK_PRIV_H
#define BROUW_NETWERK_PRIV_H

#include "brouw_netwerk.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    uint16_t versie;
    brouw_netwerk_mqtt_t mqtt;
    char ssid[33];
    char wifi_wachtwoord[64];
} brouw_instellingen_t;

bool brouw_wpa_wachtwoord_geldig(const char *wachtwoord);
esp_err_t brouw_instellingen_lees(brouw_instellingen_t *instellingen);
esp_err_t brouw_instellingen_bewaar(brouw_instellingen_t *instellingen);
const char *brouw_instellingen_fout(const brouw_instellingen_t *instellingen);
esp_err_t brouw_portaal_start(TaskHandle_t netwerktaak, httpd_handle_t *server);

#endif
