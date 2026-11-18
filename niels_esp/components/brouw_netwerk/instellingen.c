#include <string.h>

#include "netwerk_priv.h"
#include "nvs.h"

#define INSTELLINGEN_VERSIE 1
#define NVS_NAMESPACE "brouw_netwerk"
#define NVS_SLEUTEL "instellingen"

static bool ascii_letter_of_cijfer(char teken)
{
    return (teken >= 'a' && teken <= 'z') ||
           (teken >= 'A' && teken <= 'Z') ||
           (teken >= '0' && teken <= '9');
}

bool brouw_wpa_wachtwoord_geldig(const char *wachtwoord)
{
    const size_t lengte = strnlen(wachtwoord, 64);
    if (lengte < 8 || lengte > 63) {
        return false;
    }
    for (size_t i = 0; i < lengte; ++i) {
        if (wachtwoord[i] < ' ' || wachtwoord[i] > '~') {
            return false;
        }
    }
    return true;
}

static bool brokerhost_geldig(const char *host, size_t capaciteit)
{
    const size_t lengte = strnlen(host, capaciteit);
    if (lengte == 0 || lengte == capaciteit) {
        return false;
    }

    size_t label_lengte = 0;
    for (size_t i = 0; i < lengte; ++i) {
        const char teken = host[i];
        if (teken == '.') {
            if (label_lengte == 0 || host[i - 1] == '-') {
                return false;
            }
            label_lengte = 0;
            continue;
        }
        if (!ascii_letter_of_cijfer(teken) && (teken != '-' || label_lengte == 0)) {
            return false;
        }
        if (++label_lengte > 63) {
            return false;
        }
    }
    return label_lengte > 0 && host[lengte - 1] != '-';
}

const char *brouw_instellingen_fout(const brouw_instellingen_t *instellingen)
{
    const size_t ssid_lengte = strnlen(instellingen->ssid, sizeof(instellingen->ssid));
    if (ssid_lengte == 0 || ssid_lengte == sizeof(instellingen->ssid)) {
        return "De wifi-naam moet 1 tot 32 bytes bevatten.";
    }
    if (instellingen->wifi_wachtwoord[0] != '\0' &&
        !brouw_wpa_wachtwoord_geldig(instellingen->wifi_wachtwoord)) {
        return "Vul een wifiwachtwoord van 4 tot 63 afdrukbare ASCII-tekens in. "
               "Laat het veld leeg voor een open netwerk.";
    }
    if (!brokerhost_geldig(instellingen->mqtt.host, sizeof(instellingen->mqtt.host))) {
        return "Vul bij MQTT-broker een hostname of IPv4-adres in, zonder schema of poort.";
    }
    if (instellingen->mqtt.poort == 0) {
        return "Vul een MQTT-poort van 1 tot 65535 in.";
    }

    const char *ketel_id = instellingen->mqtt.ketel_id;
    const size_t id_lengte = strnlen(ketel_id, sizeof(instellingen->mqtt.ketel_id));
    if (id_lengte == 0 || id_lengte == sizeof(instellingen->mqtt.ketel_id)) {
        return "De ketel-ID moet 1 tot 32 tekens bevatten.";
    }
    for (size_t i = 0; i < id_lengte; ++i) {
        if (!ascii_letter_of_cijfer(ketel_id[i]) && ketel_id[i] != '-' && ketel_id[i] != '_') {
            return "Gebruik letters, cijfers, een koppelteken of een underscore voor de ketel-ID.";
        }
    }
    return NULL;
}

esp_err_t brouw_instellingen_lees(brouw_instellingen_t *instellingen)
{
    nvs_handle_t nvs;
    esp_err_t fout = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (fout != ESP_OK) {
        return fout;
    }

    size_t lengte = sizeof(*instellingen);
    fout = nvs_get_blob(nvs, NVS_SLEUTEL, instellingen, &lengte);
    nvs_close(nvs);
    if (fout == ESP_ERR_NVS_INVALID_LENGTH || (fout == ESP_OK && lengte != sizeof(*instellingen))) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (fout != ESP_OK) {
        return fout;
    }
    if (instellingen->versie != INSTELLINGEN_VERSIE) {
        return ESP_ERR_INVALID_VERSION;
    }
    return brouw_instellingen_fout(instellingen) == NULL ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t brouw_instellingen_bewaar(brouw_instellingen_t *instellingen)
{
    nvs_handle_t nvs;
    esp_err_t fout = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (fout != ESP_OK) {
        return fout;
    }

    instellingen->versie = INSTELLINGEN_VERSIE;
    fout = nvs_set_blob(nvs, NVS_SLEUTEL, instellingen, sizeof(*instellingen));
    if (fout == ESP_OK) {
        fout = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return fout;
}
