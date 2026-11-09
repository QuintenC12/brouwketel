# ProcesRegelaar (Niels ESP) v0.2.0

Code van Niels (v0.1.0), aangepast zodat hij past bij de laatste versie van het scherm
(`src/hardware.cpp`) en de afspraken in `main/niels_uart.h`.

Bouwen (ESP-IDF 5.4):

    idf.py set-target esp32s3
    idf.py menuconfig    # "Niels ESP" (UART-pinnen) en "Niels ESP: wifi en ketel"
    idf.py build flash monitor

## Wat is er veranderd tegenover v0.1.0

1. `spi_init()` gebeurt nog maar één keer, in `temp_task`. Eerst werd die ook in `app_main`
   aangeroepen. De tweede keer weigerde ESP-IDF dat, en dan stopte de temperatuurtaak.
2. Lukt de sensor niet, dan blijft `temp_task` `{"temp":null,"ok":false}` sturen en probeert
   hij het elke seconde opnieuw. Het scherm toont dan "sensorfout" en niet "geen contact".
3. `ketel/temp` is nu `{"temp":65.3,"ok":true}`, zoals in het regelboekje. Het veld
   `vermogen` is weg, want het scherm rekent de PID uit.
4. `brouw/stuur` leest nu `vermogen` (0..100 %), `pomp` en `zoemer`. `ketel_task` zet het
   vermogen om in een venster van 2 s voor de SSR, en stuurt de pomp en de zoemer.
5. Veiligheid: komt er 2 s geen `brouw/stuur` binnen, dan zet `ketel_task` de verwarming,
   de pomp en de zoemer uit.
6. `recept/antwoord` wordt nu aangenomen (in `scherm_uart.c`) en naar MQTT doorgestuurd,
   net als `brouw/status` en nieuwe `brouw/meld`-meldingen.
7. `net/start` en `net/stop` krijgen een ack `{"voor":"net/start"}`, zoals in
   `niels_uart.h`. Daarna verbindt `net.c` met wifi en de MQTT-server en stuurt het
   `net/status` terug (uit, bezig, verbonden of fout).
8. Recept: is er een verbinding, dan publiceert Niels ESP op `brouwketel/recept/vraag`. Het
   recept dat de server op `brouwketel/recept` stuurt, gaat als `recept {...}` naar het
   scherm. Het mag hooguit 512 tekens lang zijn.
9. De MAX31865 filtert nu op 50 Hz, de frequentie van het stroomnet in Europa.
10. Grotere app-partitie (`sdkconfig.defaults`), omdat wifi + MQTT niet in 1 MB passen.

Pinnen: SPI 13/1/12/10 (MISO/MOSI/CLK/CS), UART naar het scherm TX 17 / RX 18,
SSR 5, pomp 6, zoemer 7 (aanpasbaar in menuconfig).
