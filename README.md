# Brouwketel

Brouwcontroller voor de Elecrow CrowPanel 7.0" (ESP32-S3), gemaakt met EEZ Studio en LVGL 9.

## Inhoud
- `Brouwketel.eez-project`: het schermontwerp. Open dit in EEZ Studio. Met **Build** schrijft EEZ de code naar `src/ui`.
- `src/`: de firmware (PlatformIO, Arduino).
- `platformio.ini`: de build-omgevingen.
- `niels_esp/`: firmware voor Niels ESP (ESP32-S3 met sensor, SSR, pomp, zoemer en MQTT). Apart ESP-IDF-project: `idf.py set-target esp32s3`, `idf.py menuconfig` (wifi en pinnen onder "Niels ESP"), `idf.py build flash monitor`.
- `include/niels_uart.h`: de afspraken voor de kabel tussen het scherm en Niels ESP (staat ook in `niels_esp/main/`).

## Bouwen (Windows en Mac)
Open deze map in VS Code met de PlatformIO-extensie en kies een omgeving:
- `crowpanel7`: de echte firmware.
- `crowpanel7_demo`: echt scherm, maar met een nagebootste ketel en server (de tijd loopt 30x sneller).
- `simulator`: alleen op een Mac met Homebrew SDL2 (`brew install sdl2`).

Op Windows heb je misschien de CH340 USB-driver nodig om het CrowPanel als COM-poort te zien.

## Samenwerken
1. Haal eerst de laatste versie op (**Pull**) voordat je begint.
2. Maak je wijziging, en commit met een korte beschrijving.
3. Stuur ze door (**Push**).

Werk liefst niet tegelijk aan `Brouwketel.eez-project`: dat is één groot bestand, en twee wijzigingen tegelijk zijn moeilijk samen te voegen.
