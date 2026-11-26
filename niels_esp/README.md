# ProcesRegelaar (Niels ESP) v0.5.0

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
| `main/net.c` | Vertaling naar de MQTT-afspraken van de webapp (zie onder) + net/status naar het scherm |
| `main/recept.c` | Recept van de webapp (`cmd/recept`) omzetten naar het `recept`-bericht van het scherm |
| `main/pid.c` | PID-regelaar (anti-windup, D-term op de temperatuur) |
| `main/niels_uart.h` | Kopie van `include/niels_uart.h` (zelfde bestand aan beide kanten) |
| `components/scherm_uart` | De kabel naar het scherm (van Niels; sinds v0.5.0 regels tot 2048 tekens en alle onderwerpen uit het regelboekje) |
| `components/max31865` | De PT100-sensor (van Niels, ongewijzigd) |
| `components/brouw_netwerk` | Wifi met hotspot + formulier (van Niels, ongewijzigd) |

## v0.4.0: vertaling naar de MQTT-afspraken van de webapp

Het UART-protocol met het scherm is **niet veranderd** (regelboekje). Alleen de MQTT-kant
volgt nu "MQTT-afspraken met elektronica" van het back-end-team. Alle topics beginnen met
`brouwer/<id>/`, met `<id>` = het wifi-MAC-adres in kleine letters zonder dubbelpunten.

| Van het scherm / de ketel | Naar MQTT (na `brouwer/<id>/`) |
|---|---|
| verbinden | client-id `brouwer-<id>`, keepalive 30 s, last will `status` = `offline` (QoS 1, retained) |
| na het verbinden | `info` (JSON, retained) en `status` = `online` (retained) |
| `ketel/temp` temp | `sensor/temperatuur` = `66.8` (elke seconde, alleen een geldige meting) |
| `ketel/temp` ok:false | `fout` = `{"code":"sensor","message":"..."}` (één keer bij het wegvallen) |
| vermogen van de PID | `actuator/verwarming/state` = `0`..`100` (retained, bij een verandering) |
| pomp / zoemer uit brouw/stuur | `actuator/pomp/state`, `actuator/zoemer/state` = `on`/`off` (retained) |
| `brouw/meld` sg | `sensor/sg` = `1.050` |
| `brouw/status` doel, rest, stap | `sensor/doeltemperatuur` (°C), `sensor/resterend` (s), `sensor/maischstap` (elke seconde) |
| `brouw/meld` recept_ok / recept_fout | `ack` op het recept, of `fout` met code `recept` |
| `brouw/status` fase | `fase` = `geen`/`maischen`/`spoelen`/`koken` (retained, bij een verandering; extra, nog niet in de afspraken) |
| `brouw/meld` (elke nieuwe) | `event` = `{"naam":"hop_alarm","tekst":"Hop toevoegen: Citra - 20 g","hop":"Citra - 20 g"}` (zonder `nr`; extra, nog niet in de afspraken) |
| `brouw/meld` start / start_fout | `ack` op `cmd/start` (als die wacht) |
| `brouw/verslag` | `verslag` = het brouwverslag zonder `nr` (QoS 1; zonder broker bewaard tot die er weer is; extra, nog niet in de afspraken) |

| Van MQTT | Naar het scherm |
|---|---|
| `cmd/recept` `{"message_id":..,"data":{recept}}` | `recept {"naam":..,"kooktijd":..,"stappen":[..],"hops":[..]}` |
| `cmd/start` `{"message_id":..}` | `brouw/start {}`; het scherm meldt `start` of `start_fout` en dat wordt de `ack` (na 8 s zonder antwoord: `ok:false`) |
| `brouwer/server/status` (retained) | `"online"` in `net/toestand` |
| `cmd/setpoint`, `cmd/actuator` | niets: `ack` met `ok:false`, het scherm regelt de ketel |

Werking:
- MQTT verbindt **vanzelf** zodra er wifi is, met de broker uit het wifi-formulier. Zo ziet de
  webapp de ketel altijd online. `server`/`poort` uit net/start gebruikt hij alleen als het
  formulier nog geen broker heeft.
- **Recept:** stuur het recept eerst vanuit de webapp en klik daarna op het scherm op
  **Automatisch**. Niels ESP bewaart het laatste recept en stuurt het na net/start naar het
  scherm. Het antwoord van het scherm (`recept_ok` of `recept_fout`) gaat als `ack` naar de
  back-end. Komt het recept binnen terwijl het scherm al wacht, dan gaat het meteen door. Wacht
  het scherm nog niet, dan antwoordt Niels ESP meteen met `ok:true` (geladen).
- **Omzetten van het recept:**
  - `name` wordt `naam`, ingekort tot 47 bytes
  - `mash_schedule.steps` wordt `stappen`, gesorteerd op `step_nr`, met temperatuur en minuten
    afgerond
  - `hops` wordt `hops`, als `"Citra - 20 g"` met `boil_time_min`, langste eerst
  - `kooktijd` is de langste hoptijd, met een minimum van 60 min
  - meer dan 4 stappen of meer dan 10 hopgiften en extra's samen: `ack` met `ok:false`
- net/status naar het scherm: `verbonden` zolang MQTT verbonden is. Na net/start zonder
  verbinding is het `bezig`, en na 25 s `fout` met de reden "wifi", "wifi niet ingesteld" of
  "server onbereikbaar".

## v0.5.0: de kabel volgens het regelboekje

1. **Lange regels:** `scherm_uart` nam maar regels tot 512 tekens aan. Een recept met mout en
   een brouwverslag zijn langer, dus die vielen weg. Nu 2048 tekens (`NIELS_MAX_REGEL`, aan
   beide kanten). Versturen gebeurt in stukken met een slot, zonder kopie van 2 kB op de stack.
2. **Onderwerpen:** `brouw/verslag`, `net/vraag` en `net/modus` van het scherm, en
   `net/toestand` en `brouw/start` naar het scherm kwamen niet door de filter van `scherm_uart`.
3. **net/toestand** heeft nu alle velden uit het regelboekje (`online`, `ssid`, `server`,
   `poort`) en komt alleen bij een verandering en als antwoord op `net/vraag`.
4. **brouw/verslag** krijgt een `ack` en gaat één keer naar MQTT `verslag`.
5. **cmd/start** van de webapp gaat als `brouw/start` naar het scherm.

## v0.3.0: wat er veranderd is tegenover de code van Niels

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
8. *(aangepast in v0.4.0)* **net/start** deed eerst 1 s niets en meldde dan altijd "verbonden". Nu:
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
10. *(vervangen in v0.4.0)* **Meldingen**: een nieuwe brouw/meld gaat naar `brouwketel/brouw/meld`, met QoS 1. Is er
    geen verbinding, dan bewaart hij er tot 10 en stuurt hij die later.
11. *(vervangen in v0.4.0)* **Recept**: na het verbinden publiceert hij op `brouwketel/recept/vraag`. Wat de server op
    `brouwketel/recept` stuurt, gaat als `recept {...}` naar het scherm. Dat mag hooguit 512
    tekens zijn.
12. *(vervangen in v0.4.0)* **ketel/temp** gaat ook naar MQTT (`brouwketel/ketel/temp`).
13. Het setupwachtwoord van de hotspot staat in menuconfig en niet meer in `main.c`. De
    standaardwaarde is nog "admin123".
14. Grotere app-partitie (`sdkconfig.defaults`): het programma is ongeveer 1 MB.

Pinnen: SPI 13/1/12/10 (MISO/MOSI/CLK/CS), UART naar het scherm TX 17 / RX 18,
SSR 5, pomp 6, zoemer 7 (aanpasbaar in menuconfig).
