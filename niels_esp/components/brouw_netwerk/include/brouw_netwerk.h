#ifndef BROUW_NETWERK_H
#define BROUW_NETWERK_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BROUW_NETWERK_UIT,
    BROUW_NETWERK_SETUP,
    BROUW_NETWERK_WACHT_OP_IP,
    BROUW_NETWERK_IP_GEREED,
    BROUW_NETWERK_FOUT,
} brouw_netwerk_status_t;

typedef struct {
    char host[254];
    char ketel_id[33];
    uint16_t poort;
} brouw_netwerk_mqtt_t;

/* Roep eenmaal op vanuit app_main. De component beheert beide wifi-modi.
 * setup_wachtwoord beveiligt de hotspot en bevat 8–63 afdrukbare ASCII-tekens.
 * setup_afdwingen opent het formulier ook wanneer er al instellingen zijn.
 * De verbinding en het formulier draaien in aparte taken. */
esp_err_t brouw_netwerk_start(const char *setup_wachtwoord, bool setup_afdwingen);

/* IP_GEREED betekent dat de station-interface een IPv4-adres heeft. */
brouw_netwerk_status_t brouw_netwerk_status(void);

/* Leest de opgeslagen brokergegevens en ketel-ID uit NVS. */
esp_err_t brouw_netwerk_lees_mqtt(brouw_netwerk_mqtt_t *mqtt);

#ifdef __cplusplus
}
#endif

#endif
