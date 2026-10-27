/*
 * niels_i2c.h - afspraken voor I2C tussen het scherm en Niels ESP
 *
 * Dit bestand is voor BEIDE kanten: kopieer het ongewijzigd naar het project
 * van Niels ESP, zodat beide kanten dezelfde nummers en indeling gebruiken.
 *
 * Rolverdeling
 *   Scherm (CrowPanel) = master. Het begint elk gesprek.
 *   Niels ESP          = slave op adres NIELS_ADRES. Hij heeft de sensor,
 *                        SSR, pomp, zoemer en de verbinding met de server.
 *
 * Bedrading: SDA-SDA, SCL-SCL, GND-GND op de I2C-connector van het CrowPanel
 * (GPIO19/GPIO20, dezelfde bus als de touch). 3,3 V aan beide kanten.
 *
 * Elk bericht (in beide richtingen) ziet er zo uit:
 *     [soort] [lengte] [lengte bytes inhoud] [crc8]
 * crc8 wordt berekend over soort + lengte + inhoud (zie niels_crc8).
 * Getallen van meer dan één byte: little-endian (laagste byte eerst),
 * dat is wat de ESP32 standaard doet bij memcpy van een struct.
 *
 * Scherm -> Niels ESP (Wire write)
 *   NB_STUUR      elke 200 ms. Ook het "levensteken": krijgt Niels ESP
 *                 2 s geen geldig NB_STUUR, dan zet hij SSR, pomp en
 *                 zoemer zelf uit.
 *   NB_VERBIND    verbinding maken met de server en het recept ophalen
 *   NB_VERBREEK   verbinding met de server sluiten
 *   NB_MELD       gebeurtenis voor de webapp, bijv. "start", "pauze", "stop"
 *   NB_MELD_SG    gemeten SG na een maischstap
 *   NB_KIES_DEEL  de volgende read geeft dit stuk van het recept
 *
 * Niels ESP -> scherm (Wire requestFrom)
 *   Normaal geeft elke read een NB_STATUS terug.
 *   Na NB_KIES_DEEL geeft de eerstvolgende read het gekozen receptdeel
 *   (NB_RECEPT_KOP, NB_RECEPT_STAPPEN of NB_RECEPT_HOP), daarna weer
 *   NB_STATUS. Niels ESP moet zijn antwoord dus al klaar hebben staan
 *   voordat het scherm leest (het scherm wacht 2 ms tussen write en read).
 *
 * Verbinden, zoals het scherm het doet:
 *   1. NB_VERBIND sturen
 *   2. status opvragen tot net = NET_RECEPT_KLAAR (of NET_FOUT)
 *   3. NB_KIES_DEEL 0 + read -> kop, NB_KIES_DEEL 1 + read -> stappen,
 *      NB_KIES_DEEL 2+i + read -> hop i
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define NIELS_ADRES     0x30    // vrij: de touch zit op 0x5D/0x14, de andere chip op 0x18
#define NIELS_MAX_BERICHT 64    // ruim onder de 128 bytes van de ESP32 Wire-buffer

/* ---- Soorten berichten ---- */
enum NielsSoort : uint8_t {
    // scherm -> Niels ESP
    NB_STUUR        = 0x01,
    NB_VERBIND      = 0x02,
    NB_VERBREEK     = 0x03,
    NB_MELD         = 0x04,
    NB_MELD_SG      = 0x05,
    NB_KIES_DEEL    = 0x06,
    // Niels ESP -> scherm
    NB_STATUS       = 0x81,
    NB_RECEPT_KOP   = 0x82,
    NB_RECEPT_STAPPEN = 0x83,
    NB_RECEPT_HOP   = 0x84,
};

/* Verbinding met de server, zoals Niels ESP ze meldt in NielsStatus.net */
enum NielsNet : uint8_t {
    NET_UIT          = 0,   // niet verbonden, niets aan het doen
    NET_BEZIG        = 1,   // aan het verbinden (wifi/server)
    NET_VERBONDEN    = 2,   // server bereikt, recept wordt opgehaald
    NET_RECEPT_KLAAR = 3,   // recept staat klaar om op te halen
    NET_FOUT         = 4,   // verbinden of recept ophalen mislukt
};

/* ---- Inhoud van de berichten ---- */
#pragma pack(push, 1)

struct NielsStuur {             // NB_STUUR
    uint8_t vermogen;           // 0..100 % voor de SSR (Niels ESP: venster van 2 s)
    uint8_t pomp;               // 0/1
    uint8_t zoemer;             // 0/1
};

struct NielsVerbind {           // NB_VERBIND
    char     server_ip[16];
    uint16_t server_poort;
};

struct NielsMeld {              // NB_MELD
    char tekst[24];             // afgesloten met 0
};

struct NielsMeldSG {            // NB_MELD_SG
    uint8_t  stap;              // 0 = stap 1
    uint16_t sg_x1000;          // 1.052 -> 1052
};

struct NielsKiesDeel {          // NB_KIES_DEEL
    uint8_t deel;               // 0 = kop, 1 = stappen, 2.. = hop (deel - 2)
};

struct NielsStatus {            // NB_STATUS
    int16_t temp_x10;           // 65.3 °C -> 653
    uint8_t sensor_ok;          // 0 = sensorfout
    uint8_t net;                // NielsNet
};

struct NielsReceptKop {         // NB_RECEPT_KOP
    char     naam[40];          // "Demo Tripel - 20 L"
    uint8_t  aantal_stappen;    // max 4
    uint8_t  aantal_hops;       // max 5
    uint16_t kooktijd_min;
};

struct NielsReceptStappen {     // NB_RECEPT_STAPPEN
    struct { uint8_t temp; uint16_t minuten; } stap[4];
};

struct NielsReceptHop {         // NB_RECEPT_HOP
    char     naam[32];          // "Cascade - 30 g"
    uint16_t minuten;           // toevoegen als er nog zoveel minuten kooktijd over zijn
};

#pragma pack(pop)

/* CRC-8 (polynoom 0x07): vangt verminkte berichten op */
static inline uint8_t niels_crc8(const uint8_t *data, size_t n) {
    uint8_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}

/* Bericht inpakken in buf (minstens n + 3 bytes); geeft de totale lengte */
static inline size_t niels_pak_in(uint8_t *buf, uint8_t soort, const void *inhoud, uint8_t n) {
    buf[0] = soort;
    buf[1] = n;
    const uint8_t *p = (const uint8_t *)inhoud;
    for (uint8_t i = 0; i < n; i++) buf[2 + i] = p[i];
    buf[2 + n] = niels_crc8(buf, 2 + n);
    return 3 + n;
}

/* Bericht controleren; true als soort, lengte en crc kloppen.
 * De inhoud staat daarna vanaf buf + 2. */
static inline bool niels_pak_uit(const uint8_t *buf, size_t len, uint8_t soort, uint8_t n) {
    return len >= (size_t)n + 3 && buf[0] == soort && buf[1] == n
        && buf[2 + n] == niels_crc8(buf, 2 + n);
}
