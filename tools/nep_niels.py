"""
nep_niels.py - speel Niels ESP na vanaf je computer

Het scherm praat via UART0 met Niels ESP, en dat is dezelfde lijn als de
USB-kabel. Dit script doet zich voor als Niels ESP:
  - elke seconde ketel/temp (anders denkt het scherm dat het contact weg is),
    met een eenvoudig vermogen: 100 % onder het doel uit brouw/stuur, anders 0 %
    (de echte Niels ESP doet hier de PID)
  - meldingen (brouw/meld) krijgen vanzelf een ack
  - verandert de zoemer (uit / melding / alarm), dan zie je dat hier
  - wat het scherm stuurt, zie je hier (zonder de hartslag brouw/stuur)

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

toestand = {"temp": 20.0, "ok": True, "praat": True, "stuur_tonen": False, "doel": 0, "zoemer": "uit"}
slot = threading.Lock()


def stuur(poort, regel):
    with slot:
        poort.write((regel + "\n").encode())


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
        else:
            stuur(poort, invoer)
            continue
        print("  (ok)")
    poort.close()


if __name__ == "__main__":
    main()
