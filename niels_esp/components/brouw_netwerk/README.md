# Netwerkinstellingen voor de procescontroller

Deze ESP-IDF-component is geschreven voor ESP-IDF 5.4.3. Hij biedt een eigen WPA2-hotspot met een lokaal formulier. Na een geldige inzending bewaart hij de instellingen als één record in NVS, sluit hij het formulier en schakelt hij naar stationmodus. De procescontroller draait tijdens de overgang verder.

## 1. Toevoegen aan het project

1. Pak de ZIP uit. Kopieer de volledige map `brouw_netwerk` naar `components/` van de **procescontroller**. Het pad wordt `components/brouw_netwerk/include/brouw_netwerk.h`.
2. Voeg `brouw_netwerk` toe aan de bestaande `PRIV_REQUIRES` of `REQUIRES` in `main/CMakeLists.txt`. Voeg ook `mqtt` toe als `main` zelf de MQTT-client gebruikt.
3. Zet `#include "brouw_netwerk.h"` bovenaan `main.c`. Roep de component eenmaal op in `app_main()`, nadat je UART- en procestaken gestart zijn. Kies per ketel een eigen setupwachtwoord van 8–63 afdrukbare ASCII-tekens.

```c
const bool setup_afdwingen = false;
const esp_err_t netwerk_fout = brouw_netwerk_start("VervangDitSetupWachtwoord", setup_afdwingen);
if (netwerk_fout != ESP_OK) {
    ESP_LOGE("main", "Netwerkstart: %s", esp_err_to_name(netwerk_fout));
}
```

Voor `ESP_LOGE` heb je `#include "esp_log.h"` nodig. Laat `brouw_netwerk` de wifi-driver en de standaard wifi-interfaces beheren. Als `net/start` of een bestaande wifi-module zelf `esp_wifi_init()`, `esp_wifi_start()` of `esp_wifi_stop()` aanroept, verplaats die verantwoordelijkheid naar deze component.

## 2. Instellen vanaf de pc

1. Flash de procescontroller en open de seriële monitor. Bij een eerste start staat daar de naam `Brouwketel-Setup-XXXXXX`. De laatste zes tekens komen uit het MAC-adres.
2. Verbind je pc met dat netwerk en geef het **setupwachtwoord uit `app_main()`** in. Windows kan melden dat dit netwerk geen internet heeft. Blijf ermee verbonden.
3. Open [http://192.168.4.1/](http://192.168.4.1/) in de browser en vul de gegevens in.

| Veld | Invoer |
| --- | --- |
| SSID | Naam van het doelnetwerk, maximaal 32 bytes. Gebruik op een klassieke ESP32 een 2,4 GHz-netwerk. |
| Wifiwachtwoord | Wachtwoord van het doelnetwerk. De component ondersteunt een WPA2-Personal-wachtwoord van 8–63 afdrukbare ASCII-tekens. Een leeg veld dient voor een open netwerk. |
| MQTT-broker | DNS-hostname of IPv4-adres, bijvoorbeeld `192.168.0.25`. |
| MQTT-poort | `1`–`65535`. Het formulier start op `1883`, de standaardpoort voor MQTT over TCP. |
| Ketel-ID | 1–32 letters, cijfers, koppeltekens of underscores, bijvoorbeeld `ketel-01`. |

4. Klik op **Opslaan en verbinden**. Het setupnetwerk verdwijnt. De seriële monitor toont `Wifi IPv4-adres` als de ESP32 een adres krijgt van de router. Zet je pc daarna weer op het gewone netwerk.

Bij een volgende opstart leest de component het NVS-record. Krijgt de ESP32 bij de eerste poging binnen ongeveer 25 seconden geen IP-adres, dan verschijnt het setupnetwerk opnieuw. Na een geslaagde verbinding probeert de component bij uitval telkens opnieuw te verbinden in stationmodus. Wil je bijvoorbeeld een werkend wifi-netwerk behouden en de broker wijzigen, laat je applicatie bij het opstarten `setup_afdwingen = true` doorgeven op basis van een knop of servicestand. Vul het formulier dan opnieuw in. Doe de configuratie vóór een brouwsessie.

## 3. Aansluiten op UART en MQTT

De stationverbinding start zodra de ESP32 een configuratie heeft. Gebruik de bestaande UART-opdracht `net/start` als aanvraag om **MQTT** te starten. De host en poort komen uit het webformulier. De payload die het scherm vandaag bij `net/start` meestuurt kan voor de overgang blijven bestaan. Spreek met de schermbouwer af dat `net/stop` in deze opzet de MQTT-client stopt en de wifi-verbinding beschikbaar blijft.

Laat de UART-ontvangsttaak `net/start` en `net/stop` doorgeven aan je MQTT- of netwerktaak, zodat `ketel/temp` en `brouw/stuur` op tijd verwerkt blijven. Die taak kan `brouw_netwerk_status()` opvragen. Wacht met het aanmaken en starten van de MQTT-client tot de status `BROUW_NETWERK_IP_GEREED` is. Bij `BROUW_NETWERK_SETUP` moet de gebruiker eerst configureren. Houd voor de volledige verbinding rekening met de bestaande timeout van 30 seconden op het scherm.

Dit fragment maakt een ESP-MQTT-client voor een broker over TCP aan. Roep het vanuit de taak die de levenscyclus van je MQTT-client beheert. Registreer op de geretourneerde client jullie MQTT-eventhandler en roep daarna `esp_mqtt_client_start()` aan.

```c
#include "brouw_netwerk.h"
#include "mqtt_client.h"

static esp_mqtt_client_handle_t maak_mqtt_client(void)
{
    if (brouw_netwerk_status() != BROUW_NETWERK_IP_GEREED) {
        return NULL;
    }

    brouw_netwerk_mqtt_t instellingen;
    if (brouw_netwerk_lees_mqtt(&instellingen) != ESP_OK) {
        return NULL;
    }

    const esp_mqtt_client_config_t config = {
        .broker.address.hostname = instellingen.host,
        .broker.address.port = instellingen.poort,
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
    };
    return esp_mqtt_client_init(&config);
}
```

Gebruik `instellingen.ketel_id` waar jullie backend een ketel identificeert. Als jullie afspreken dat de ketel-ID ook de MQTT-client-ID is, voeg dan `.credentials.client_id = instellingen.ketel_id` toe en geef elke ketel een unieke ID die de broker accepteert.

Stuur bij `net/start` eerst `net/status {"status":"bezig"}`. Stuur `net/status {"status":"verbonden"}` zodra je `MQTT_EVENT_CONNECTED` ontvangt. Als wifi geen IP-adres krijgt, gebruik je bijvoorbeeld `net/status {"status":"fout","reden":"wifi"}`. Geef statuswijzigingen meteen door en stuur de actuele status ook elke 5 seconden zoals afgesproken voor UART.

## 4. Extra velden toevoegen

- Voeg de invoer toe aan `formulier.html`.
- Voeg het veld toe aan `brouw_instellingen_t` in `netwerk_priv.h` en koppel het in `formulier_lezen()` in `portaal.c`. Pas `FORMULIER_MAX` aan als er langere invoer bijkomt.
- Valideer de waarde in `brouw_instellingen_fout()` in `instellingen.c`. Verhoog `INSTELLINGEN_VERSIE` wanneer het opgeslagen record van vorm verandert. Voor behoud van oude instellingen voeg je een migratie toe. De huidige code opent bij een onbekende versie het setupformulier.

Het formulier wordt via `EMBED_TXTFILES` meegecompileerd. Het wifiwachtwoord staat in NVS. Of NVS versleuteld is, hangt af van de instellingen van je ESP-IDF-project.

## Controle op je bord

- Start zonder instellingen. Open het formulier en sla geldige gegevens op. Kijk of het setupnetwerk verdwijnt en er een IPv4-adres verschijnt.
- Herstart de ESP32. De gegevens horen behouden te blijven en de ketel hoort direct als station te verbinden.
- Probeer een verkeerd wifiwachtwoord. De hotspot hoort terug te komen. Corrigeer de gegevens via het formulier.
- Start MQTT via `net/start`. Het scherm hoort pas online te tonen zodra de MQTT-broker de verbinding heeft geaccepteerd.
