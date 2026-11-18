#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "netwerk_priv.h"

#define FORMULIER_MAX 1280
#define SETUP_HOST "192.168.4.1"
#define SETUP_HOSTNAAM "Brouwketel-setup.local"
#define FORMULIER_TYPE "application/x-www-form-urlencoded"

extern const char formulier_html_start[] asm("_binary_formulier_html_start");

static const char *TAG = "brouw_portaal";
//De HTTP-server handelt deze verzoeken af in één taak.
static bool s_bewaard;

static int hex_cijfer(unsigned char teken)
{
    if (teken >= '0' && teken <= '9') {
        return teken - '0';
    }
    if (teken >= 'A' && teken <= 'F') {
        return teken - 'A' + 10;
    }
    if (teken >= 'a' && teken <= 'f') {
        return teken - 'a' + 10;
    }
    return -1;
}

static bool decodeer_veld(char *uit, size_t capaciteit, const char *in)
{
    size_t lengte = 0;
    while (*in != '\0') {
        unsigned char teken = (unsigned char)*in++;
        if (teken == '+') {
            teken = ' ';
        } else if (teken == '%') {
            if (in[0] == '\0' || in[1] == '\0') {
                return false;
            }
            const int hoog = hex_cijfer(in[0]);
            const int laag = hex_cijfer(in[1]);
            if (hoog < 0 || laag < 0) {
                return false;
            }
            teken = (unsigned char)((hoog << 4) | laag);
            in += 2;
        }
        if (teken < ' ' || teken == 0x7f || lengte + 1 >= capaciteit) {
            return false;
        }
        uit[lengte++] = (char)teken;
    }
    uit[lengte] = '\0';
    return true;
}

static const char *formulier_lezen(char *inhoud, brouw_instellingen_t *instellingen)
{
    char poort[6];
    struct {
        const char *naam;
        char *waarde;
        size_t capaciteit;
        bool ontvangen;
    } velden[] = {
        {"ssid", instellingen->ssid, sizeof(instellingen->ssid), false},
        {"wifi_wachtwoord", instellingen->wifi_wachtwoord, sizeof(instellingen->wifi_wachtwoord), false},
        {"mqtt_host", instellingen->mqtt.host, sizeof(instellingen->mqtt.host), false},
        {"mqtt_poort", poort, sizeof(poort), false},
        {"ketel_id", instellingen->mqtt.ketel_id, sizeof(instellingen->mqtt.ketel_id), false},
    };
    const size_t aantal_velden = sizeof(velden) / sizeof(velden[0]);
    char *volgend = NULL;

    for (char *paar = inhoud; paar != NULL; paar = volgend) {
        volgend = strchr(paar, '&');
        if (volgend != NULL) {
            *volgend++ = '\0';
        }
        char *waarde = strchr(paar, '=');
        if (waarde == NULL) {
            return "De gegevens konden niet gelezen worden. Open het formulier opnieuw.";
        }
        *waarde++ = '\0';

        size_t i = 0;
        while (i < aantal_velden && strcmp(paar, velden[i].naam) != 0) {
            ++i;
        }
        if (i == aantal_velden || velden[i].ontvangen) {
            return "Het formulier bevat een onbekend of dubbel veld.";
        }
        if (!decodeer_veld(velden[i].waarde, velden[i].capaciteit, waarde)) {
            return "Een veld is te lang of bevat een ongeldig teken. Ga terug en controleer de invoer.";
        }
        velden[i].ontvangen = true;
    }
    for (size_t i = 0; i < aantal_velden; ++i) {
        if (!velden[i].ontvangen) {
            return "Het formulier is onvolledig. Open het formulier opnieuw.";
        }
    }

    if (poort[0] == '\0' || strspn(poort, "0123456789") != strlen(poort)) {
        return "Vul een MQTT-poort van 1 tot 65535 in.";
    }
    const unsigned long poort_nummer = strtoul(poort, NULL, 10);
    if (poort_nummer == 0 || poort_nummer > UINT16_MAX) {
        return "Vul een MQTT-poort van 1 tot 65535 in.";
    }
    instellingen->mqtt.poort = (uint16_t)poort_nummer;
    return brouw_instellingen_fout(instellingen);
}

static esp_err_t antwoord(httpd_req_t *verzoek, const char *status, const char *tekst)
{
    httpd_resp_set_status(verzoek, status);
    httpd_resp_set_type(verzoek, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(verzoek, "Cache-Control", "no-store");
    return httpd_resp_sendstr(verzoek, tekst);
}

static esp_err_t weiger_en_sluit(httpd_req_t *verzoek, const char *status, const char *tekst)
{
    antwoord(verzoek, status, tekst);
    return ESP_FAIL;
}

static bool host_komt_overeen(const char *waarde, const char *naam)
{
    const size_t lengte = strlen(naam);

    return strncasecmp(waarde, naam, lengte) == 0 &&
           (waarde[lengte] == '\0' || strcmp(waarde + lengte, ":80") == 0);
}

static bool lokaal_verzoek(httpd_req_t *verzoek)
{
    char host[64];
    if (httpd_req_get_hdr_value_str(verzoek, "Host", host, sizeof(host)) != ESP_OK) {
        return false;
    }

    const char *verwachte_host;
    if (host_komt_overeen(host, SETUP_HOST)) {
        verwachte_host = SETUP_HOST;
    } else if (host_komt_overeen(host, SETUP_HOSTNAAM)) {
        verwachte_host = SETUP_HOSTNAAM;
    } else {
        return false;
    }

    const char schema[] = "http://";
    char origin[sizeof(host) + sizeof(schema) - 1];
    const esp_err_t fout =
        httpd_req_get_hdr_value_str(verzoek, "Origin", origin, sizeof(origin));

    if (fout == ESP_ERR_NOT_FOUND) {
        return true;
    }

    return fout == ESP_OK &&
           strncasecmp(origin, schema, sizeof(schema) - 1) == 0 &&
           host_komt_overeen(origin + sizeof(schema) - 1, verwachte_host);
}

static esp_err_t formulier_get(httpd_req_t *verzoek)
{
    if (!lokaal_verzoek(verzoek)) {
        return antwoord(verzoek, "403 Forbidden", "Open http://" SETUP_HOST "/");
    }
    httpd_resp_set_type(verzoek, "text/html; charset=utf-8");
    httpd_resp_set_hdr(verzoek, "Cache-Control", "no-store");
    return httpd_resp_sendstr(verzoek, formulier_html_start);
}

static esp_err_t formulier_post(httpd_req_t *verzoek)
{
    if (!lokaal_verzoek(verzoek)) {
        return weiger_en_sluit(verzoek, "403 Forbidden", "Open http://" SETUP_HOST "/");
    }
    if (s_bewaard) {
        return weiger_en_sluit(verzoek, "409 Conflict", "Opgeslagen. De ESP32 schakelt naar wifi.");
    }
    char content_type[80];
    if (httpd_req_get_hdr_value_str(verzoek, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        strcmp(content_type, FORMULIER_TYPE) != 0) {
        return weiger_en_sluit(verzoek, "415 Unsupported Media Type", "Verstuur via het instellingenformulier.");
    }
    if (verzoek->content_len == 0 || verzoek->content_len > FORMULIER_MAX) {
        return weiger_en_sluit(verzoek, "400 Bad Request", "Het formulier is leeg of te groot.");
    }

    char inhoud[FORMULIER_MAX + 1];
    size_t ontvangen = 0;
    while (ontvangen < verzoek->content_len) {
        const int gelezen = httpd_req_recv(verzoek, inhoud + ontvangen, verzoek->content_len - ontvangen);
        if (gelezen == HTTPD_SOCK_ERR_TIMEOUT) {
            return weiger_en_sluit(verzoek, "408 Request Timeout", "Het ontvangen van de instellingen duurde te lang.");
        }
        if (gelezen <= 0) {
            return ESP_FAIL;
        }
        ontvangen += (size_t)gelezen;
    }
    if (memchr(inhoud, '\0', ontvangen) != NULL) {
        return antwoord(verzoek, "400 Bad Request", "Het formulier bevat een ongeldig teken.");
    }
    inhoud[ontvangen] = '\0';

    brouw_instellingen_t instellingen = {0};
    const char *invoerfout = formulier_lezen(inhoud, &instellingen);
    if (invoerfout != NULL) {
        return antwoord(verzoek, "400 Bad Request", invoerfout);
    }

    const esp_err_t fout = brouw_instellingen_bewaar(&instellingen);
    if (fout != ESP_OK) {
        ESP_LOGE(TAG, "Opslaan in NVS: %s", esp_err_to_name(fout));
        return antwoord(verzoek, "500 Internal Server Error", "Opslaan mislukt. De ESP32 blijft in setupmodus.");
    }

    s_bewaard = true;
    const esp_err_t verzonden = antwoord(verzoek, "200 OK",
        "Instellingen opgeslagen. Het setupnetwerk sluit en de ESP32 probeert de gekozen wifi. "
        "Kijk in de seriële monitor voor het IP-adres. Verschijnt het setupnetwerk opnieuw, "
        "open dan het formulier om de wifi-instellingen te corrigeren.");
    xTaskNotifyGive((TaskHandle_t)verzoek->user_ctx);
    return verzonden;
}

esp_err_t brouw_portaal_start(TaskHandle_t netwerktaak, httpd_handle_t *server)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 3;

    s_bewaard = false;
    esp_err_t fout = httpd_start(server, &config);
    if (fout != ESP_OK) {
        return fout;
    }
    const httpd_uri_t get = { .uri = "/", .method = HTTP_GET, .handler = formulier_get };
    const httpd_uri_t post = {
        .uri = "/opslaan", .method = HTTP_POST, .handler = formulier_post, .user_ctx = netwerktaak,
    };
    fout = httpd_register_uri_handler(*server, &get);
    if (fout == ESP_OK) {
        fout = httpd_register_uri_handler(*server, &post);
    }
    if (fout != ESP_OK) {
        httpd_stop(*server);
    }
    return fout;
}
