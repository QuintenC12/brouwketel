# ProcesRegelaar (Niels ESP) v0.3.0

Code van Niels (ProcesRegalaar-main), afgewerkt volgens het regelboekje
(`regelboekje-uart.md`, `main/niels_uart.h`) en het scherm op de branch
`protocol-vereenvoudigd`.

Bouwen (ESP-IDF 5.4.3):

    idf.py set-target esp32s3
    idf.py menuconfig    # "Niels ESP" (UART-pinnen) en "Niels ESP: ketel en wifi-setup"
    idf.py build flash monitor

Bij de eerste build downloadt ESP-IDF de component `espressif/mdns` (voor het formulier).

## Bestanden

| Bestand | Wat |
|---|---|
| `main/main.c` | Taken: `temp_task` (meten + PID + ketel/temp), `receive_task` (berichten van het scherm), `ketel_task` (SSR, pomp, zoemer, failsafe) |
| `main/net.c` | MQTT: verbinden na net/start, net/status, recept doorgeven, meldingen doorsturen |
| `main/pid.c` | PID-regelaar (anti-windup, D-term op de temperatuur) |
| `main/niels_uart.h` | Kopie van `include/niels_uart.h` (zelfde bestand aan beide kanten) |
| `components/scherm_uart` | De kabel naar het scherm (van Niels, ongewijzigd) |
| `components/max31865` | De PT100-sensor (van Niels, ongewijzigd) |
| `components/brouw_netwerk` | Wifi met hotspot + formulier (van Niels, ongewijzigd) |

## Wat er veranderd is tegenover de code van Niels

1. **brouw/stuur** leest nu `doel`, `kp`, `ki`, `kd`, `pomp` en `zoemer`. Ontbreekt kp, ki of kd,
   dan blijft de vorige waarde. Ontbreekt `doel`, dan staat de verwarming uit.
2. **PID**: `temp_task` rekent elke seconde het vermogen uit met `pid.c`. Met doel 0, een
   sensorfout of zonder scherm is het vermogen 0 en wordt de PID gereset.
3. **ketel/temp** stuurt het echte vermogen mee: `{"temp":65.3,"ok":true,"vermogen":40}`.
   Bij een sensorfout is dat `{"temp":null,"ok":false,"vermogen":0}`.
4. **SSR, pomp en zoemer** (nieuw, `ketel_task`): de SSR werkt met een venster van 2 s. De
   zoemer maakt zelf het ritme. Bij "melding" zijn dat om de 3 s drie piepjes van 150 ms, bij
   "alarm" 200 ms aan en 200 ms uit.
5. **Failsafe**: komt er 2 s geen brouw/stuur binnen, dan gaat alles uit, staat het doel op 0
   en wordt de PID gereset. De lijst met geziene meldnummers wordt gewist.
6. **Sensor**: `spi_init()` gebeurt alleen in `temp_task`. Lukt het niet, dan stopt de taak niet
   meer. Hij blijft `ok:false` sturen en probeert het elke seconde opnieuw. Het 50 Hz-filter
   staat aan.
7. **brouw/status** antwoordde eerst met een nep-`net/status verbonden`. Nu gaat het
   ongewijzigd naar MQTT (`brouwketel/brouw/status`).
8. **net/start** deed eerst 1 s niets en meldde dan altijd "verbonden". Nu:
   - het antwoord is `bezig`
   - daarna wacht hij op wifi via `brouw_netwerk`, hooguit 12 s
   - dan start hij MQTT, hooguit 12 s, en meldt hij `verbonden`
   - lukt dat niet, dan meldt hij `fout` met een reden: "wifi", "wifi niet ingesteld" of
     "server onbereikbaar"
   - de server en de poort komen uit net/start. Ontbreken die, dan neemt hij de broker uit het
     wifi-formulier
   - de ketel-ID uit het formulier is de MQTT-client-ID
   - het verbinden gebeurt in een eigen taak, zodat brouw/stuur nooit blijft liggen
9. **net/stop** stopt MQTT. De wifi blijft verbonden.
10. **Meldingen**: een nieuwe brouw/meld gaat naar `brouwketel/brouw/meld`, met QoS 1. Is er
    geen verbinding, dan bewaart hij er tot 10 en stuurt hij die later.
11. **Recept**: na het verbinden publiceert hij op `brouwketel/recept/vraag`. Wat de server op
    `brouwketel/recept` stuurt, gaat als `recept {...}` naar het scherm. Dat mag hooguit 512
    tekens zijn.
12. **ketel/temp** gaat ook naar MQTT (`brouwketel/ketel/temp`).
13. Het setupwachtwoord van de hotspot staat in menuconfig en niet meer in `main.c`. De
    standaardwaarde is nog "admin123".
14. Grotere app-partitie (`sdkconfig.defaults`): het programma is ongeveer 1 MB.

Pinnen: SPI 13/1/12/10 (MISO/MOSI/CLK/CS), UART naar het scherm TX 17 / RX 18,
SSR 5, pomp 6, zoemer 7 (aanpasbaar in menuconfig).
