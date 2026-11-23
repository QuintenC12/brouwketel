"""
nep_niels.py - speel Niels ESP na vanaf je computer

Het scherm praat via UART0 met Niels ESP, en dat is dezelfde lijn als de
USB-kabel. Dit script doet zich voor als Niels ESP:
  - elke seconde ketel/temp (anders denkt het scherm dat het contact weg is),
    met een eenvoudig vermogen: 100 % onder het doel uit brouw/stuur, anders 0 %
    (de echte Niels ESP doet hier de PID)
  - meldingen (brouw/meld) krijgen vanzelf een ack
  - verandert de zoemer (uit / melding / alarm), dan zie je dat hier
  - net/toestand (wifi, MQTT, modus, wifinaam projectnetwork5G en de broker
    192.168.0.10:1883) alleen bij een verandering en als het scherm net/vraag
    stuurt, net zoals Niels ESP; de schakelaar Modus op het scherm (net/modus) zet de
    nep-hotspot open of dicht
  - wat het scherm stuurt, zie je hier (zonder de hartslag brouw/stuur,
    brouw/status)

Hang de echte Niels ESP los (TX en RX) voor je dit start.

Starten (PlatformIO heeft pyserial al):
    ~/.platformio/penv/bin/python tools/nep_niels.py /dev/cu.usbserial-110
(op Windows: python tools/nep_niels.py COM5)

Typen:
    temp 65.3               temperatuur die het script elke seconde stuurt
    sensor uit / sensor aan "ok": false / true (sensorfout testen)
    stil / praat            ketel/temp stoppen / weer sturen (contact kwijt)
    stuur aan / stuur uit   brouw/stuur en brouw/status ook tonen
    recept                  voorbeeldrecept sturen (eerst Automatisch drukken)
    ok / fout               net/status verbonden / fout
    wifi aan / wifi uit     wifi in net/toestand
    mqtt aan / mqtt uit     MQTT in net/toestand
    server aan / server uit de server van de webapp ("online" in net/toestand)
    opslaan                 wifi-formulier op de nep-hotspot opslaan: hotspot dicht,
                            wifi verbonden (de schakelaar Modus moet terug naar Werking)
    stop                    afsluiten
    alles anders            gaat letterlijk naar het scherm, bijv.:
                            net/status {"status":"verbonden"}
"""
import json
import sys
import threading
import time

import serial

VOORBEELD_RECEPT = {
    "naam": "Test Blond - 20 L",
    "kooktijd": 60,
    "stappen": [{"temp": 52, "min": 10}, {"temp": 65, "min": 45}, {"temp": 78, "min": 5}],
    "hops": [{"naam": "Magnum - 20 g", "min": 60}, {"naam": "Saaz - 30 g", "min": 10}],
}

toestand = {"temp": 20.0, "ok": True, "praat": True, "stuur_tonen": False, "doel": 0, "zoemer": "uit",
            "wifi": True, "mqtt": True, "server": True, "modus": "werking"}
slot = threading.Lock()


def stuur(poort, regel):
    with slot:
        poort.write((regel + "\n").encode())


def stuur_toestand(poort):
    hotspot = toestand["modus"] == "configuratie"
    stuur(poort, "net/toestand " + json.dumps({"wifi": toestand["wifi"] and not hotspot,
                                               "mqtt": toestand["mqtt"] and not hotspot,
                                               "online": toestand["mqtt"] and toestand["server"] and not hotspot,
                                               "modus": toestand["modus"],
                                               "ssid": "projectnetwork5G",
                                               "server": "192.168.0.10", "poort": 1883}))


def temperatuur_lus(poort):
    while True:
        if toestand["praat"]:
            t = toestand["temp"] if toestand["ok"] else None
            vermogen = 100 if toestand["ok"] and toestand["temp"] < toestand["doel"] else 0
            stuur(poort, "ketel/temp " + json.dumps({"temp": t, "ok": toestand["ok"], "vermogen": vermogen}))
        time.sleep(1)


def lees_lus(poort):
    while True:
        regel = poort.readline().decode(errors="replace").rstrip()
        if not regel:
            continue
        onderwerp, _, rest = regel.partition(" ")
        if onderwerp == "brouw/stuur":
            try:
                stuur_json = json.loads(rest)
                toestand["doel"] = stuur_json.get("doel", 0)
                if stuur_json.get("zoemer", "uit") != toestand["zoemer"]:
                    toestand["zoemer"] = stuur_json.get("zoemer", "uit")
                    print("  zoemer: " + toestand["zoemer"])
            except ValueError:
                pass
        if onderwerp == "net/vraag":
            print("  scherm vraagt net/toestand")
            stuur_toestand(poort)
            continue
        if onderwerp == "net/modus":
            try:
                modus = json.loads(rest).get("modus", "werking")
            except ValueError:
                modus = toestand["modus"]
            # net/modus komt alleen als de schakelaar verandert: gewoon uitvoeren
            # (al in die modus, bijv. na "opslaan": niets doen)
            if modus != toestand["modus"]:
                toestand["modus"] = modus
                print("  modus: " + modus + (" (nep-hotspot open)" if modus == "configuratie" else ""))
                stuur_toestand(poort)
            continue
        if onderwerp in ("brouw/stuur", "brouw/status") and not toestand["stuur_tonen"]:
            continue
        print("  scherm> " + regel)
        if onderwerp == "brouw/meld":
            try:
                stuur(poort, "ack " + json.dumps({"nr": json.loads(rest)["nr"]}))
            except (ValueError, KeyError):
                pass


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(1)
    poort = serial.Serial(sys.argv[1], 115200, timeout=0.5)
    print("Verbonden met", sys.argv[1], "- het scherm start opnieuw op. Typ 'stop' om te stoppen.")
    threading.Thread(target=temperatuur_lus, args=(poort,), daemon=True).start()
    threading.Thread(target=lees_lus, args=(poort,), daemon=True).start()

    while True:
        try:
            invoer = input().strip()
        except (EOFError, KeyboardInterrupt):
            break
        woorden = invoer.split()
        if not woorden:
            continue
        if invoer == "stop":
            break
        elif woorden[0] == "temp" and len(woorden) == 2:
            toestand["temp"] = float(woorden[1].replace(",", "."))
        elif invoer in ("sensor uit", "sensor aan"):
            toestand["ok"] = invoer == "sensor aan"
        elif invoer in ("stil", "praat"):
            toestand["praat"] = invoer == "praat"
        elif invoer in ("stuur aan", "stuur uit"):
            toestand["stuur_tonen"] = invoer == "stuur aan"
        elif invoer == "recept":
            stuur(poort, "recept " + json.dumps(VOORBEELD_RECEPT))
        elif invoer in ("ok", "fout"):
            status = {"status": "verbonden"} if invoer == "ok" else {"status": "fout", "reden": "test"}
            stuur(poort, "net/status " + json.dumps(status))
        elif invoer == "opslaan":
            toestand["modus"] = "werking"
            toestand["wifi"] = True
            print("  formulier opgeslagen: nep-hotspot dicht, wifi verbonden")
            stuur_toestand(poort)
        elif invoer in ("wifi aan", "wifi uit", "mqtt aan", "mqtt uit", "server aan", "server uit"):
            toestand[woorden[0]] = woorden[1] == "aan"
            stuur_toestand(poort)
        else:
            stuur(poort, invoer)
            continue
        print("  (ok)")
    poort.close()


if __name__ == "__main__":
    main()
