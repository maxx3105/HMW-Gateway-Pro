# HMW-Gateway-Pro

Konfigurierbare **ESP32-Firmware als Ersatz für ein HomeMatic Wired LAN Gateway (HMW-LGW)**.
Sie verbindet eine HomeMatic-Zentrale (OpenCCU / RaspberryMatic / Homegear) über **WLAN
oder Ethernet** (ESP32-ETH01/LAN8720) mit einem klassischen HomeMatic-Wired-RS485-Bus —
als wartbares „Gerät" mit Config-Portal, OTA, Status-Webseite und Watchdog.

> Weiterentwicklung eines verifizierten Bare-ESP32-Gateways. Die Protokoll-Logik
> (Bus-Codec + LAN/AES-Schicht) wurde per Reverse-Engineering ermittelt und gegen
> **HBWired**, **Homegear** und **FHEM** validiert.

## Status

- ✅ **LAN-Seite:** CCU-Verbindung + AES-Handshake (Port 1000) live verifiziert.
- ✅ **Bus-Seite:** Device-Discovery (Binärsuche über den Adressraum) + Interrogation
  (Typ / Firmware / Serial) end-to-end am echten RS485-Bus.
- ✅ Gerät erscheint im CCU-Posteingang.
- 🚧 In Arbeit: saubere Typ-Erkennung (statt „generic"), Steuern/Events, Reconnect-Politur.

## Features

- **NVS-Konfiguration + Captive-Portal** — keine Secrets im Code; SSID/Passwort/Serial/
  AES-Schlüssel/feste IP/Port werden über ein Webformular gesetzt und im Flash gespeichert.
- **OTA** — ArduinoOTA *und* Web-OTA (`/update`), Flashen ohne USB.
- **Status-Webseite** (`/`) — RSSI, IP, CCU-Status, gefundene Geräte, letztes Bus-Event,
  Uptime, freier Heap; Auto-Refresh.
- **Watchdog** (`esp_task_wdt`).
- **AES abschaltbar** (für FHEM-Klartext-Betrieb).
- **WLAN oder Ethernet** — per Haken in der Konfiguration; Ethernet (LAN8720, z. B.
  ESP32-ETH01) für den Dauerbetrieb ohne Funk.
- **RS485-Pins zur Laufzeit konfigurierbar** (RX/TX/DE über `/config`, kein Reflash).
- **Verbindungs-Überwachung zur Laufzeit tunebar** — Inaktivitäts-Timeout, optionales
  TCP-Keepalive, Unicast-Antwort-Fenster (alle über `/config`, kein Reflash nötig).

## Hardware

- **3,3 V RS485-Transceiver** — Auto-Direction-Modul (z. B. XY-K485) oder MAX3485 / SN65HVD72.
  ⚠️ ESP32 ist **nicht 5 V-tolerant**.
- Bus: **19200 8E1** (even parity — macht die Firmware automatisch), 2-Draht RS485,
  120 Ω-Terminierung an den Enden, gemeinsame Masse.

### Variante A: ESP32 DevKit (WLAN)

- Verdrahtung: `GPIO16` = RX (← Modul RXD/RO), `GPIO17` = TX (→ Modul TXD/DI).
  Bei Auto-Direction-Modul DE-Pin `-1`.

### Variante B: ESP32-ETH01 / WT32-ETH01 (Ethernet)

- Onboard **LAN8720-PHY** — in der Konfiguration **„Ethernet statt WLAN"** anhaken,
  fertig (PHY-Parameter sind im Options-Block der Firmware hinterlegt).
- Verdrahtung RS485: Header-Pin **`RXD` = GPIO5** = RX (← Modul RXD/RO),
  Header-Pin **`TXD` = GPIO17** = TX (→ Modul TXD/DI), 3V3 + GND vom Board.
  DE-Pin (falls MAX3485 o. ä.): freier GPIO, z. B. `GPIO4` — per `/config` setzen.
- ⚠️ **Tabu-Pins** (gehören dem Ethernet-RMII): GPIO0 (50-MHz-Takt), 16 (Oszillator-Enable),
  18, 19, 21, 22, 23, 25, 26, 27. GPIO35/36/39 sind input-only, GPIO12 Strapping meiden.
- **Flashen:** kein USB onboard, kein Auto-Reset, kein BOOT-Taster — einmalig per
  USB-TTL-Adapter, danach bequem per Web-OTA. Verdrahtung: Adapter-TX → **`RX0`**,
  Adapter-RX → **`TX0`** (gekreuzt!), GND → GND, Adapter-**5V** → 5V-Pin
  (der 3V3-Ausgang kleiner Adapter schafft den Ethernet-PHY nicht).
  ⚠️ **Nicht** an die Pins `RXD`/`TXD` — das sind GPIO5/17 (UART2, RS485!);
  geflasht wird nur über `TX0`/`RX0` (GPIO1/3).
  Ablauf: ① `IO0` an GND brücken → ② Strom an (oder `EN` kurz an GND) →
  ③ Upload starten (läuft `Connecting…` durch, darf die Brücke weg) →
  ④ nach dem Flashen Brücke entfernen + Reset (Dauer-GND an IO0 stört den RMII-Takt).
  Meldet esptool „No serial data received": Board war nicht im Bootloader
  (Reihenfolge!), TX/RX nicht gekreuzt oder falsches Pin-Paar; notfalls
  Upload-Speed auf 115200 senken.
- **Kein BOOT-Taster:** das Config-Portal (WLAN-AP) öffnet automatisch beim ersten Boot
  ohne Config oder wenn das Netzwerk 20 s nicht zustande kommt; laufende Geräte werden
  über die Status-Seite `/config` umkonfiguriert.
- Beim Build mit dem Arduino-Board **„WT32-ETH01"** sind Ethernet + RS485-Pins (RX=5)
  bereits die Werks-Defaults; der generische „ESP32 Dev Module"-Build läuft ebenso
  (Haken + RX-Pin einmalig im Portal setzen) — **eine Binary für beide Boards**.

## Aufbau

| Datei | Inhalt |
|---|---|
| `hmw_gateway_pro/hmw_gateway_pro.ino` | Hauptfirmware: Config-Portal, Gateway-Loop, OTA, Status-Web, Watchdog |
| `hmw_gateway_pro/config.h` | Persistente Konfiguration im NVS (`Preferences`) |
| `hmw_gateway_pro/hmw_protocol.h` | Bus-Codec (CRC16 `0x1002`, FC-Escaping, Frame-Bau/-Parsing) — header-only |
| `hmw_gateway_pro/hmw_lgw.h` | LAN-Schicht (LGW-Frames, Bridge-Übersetzung, AES-128-CFB via mbedTLS) — header-only |

## Build

- Arduino IDE + ESP32-Core.
- Libraries: **ESP Async WebServer** (ESP32Async) + **Async TCP**.
- Ordner `hmw_gateway_pro/` als Sketch öffnen → kompilieren → per USB flashen.
  Danach Updates bequem per Web-OTA: in der IDE *Sketch → Kompilierte Binärdatei exportieren*,
  dann die `…ino.bin` auf der Geräteseite `/update` hochladen.

## Erst-Einrichtung

1. Erster Boot ohne Config → Access-Point **`HMW-GW-<chipid>`**, Passwort **`hmwgwconfig`**.
2. Captive-Portal öffnet sich → SSID + WLAN-Passwort **oder** Haken „Ethernet statt WLAN",
   Serial/Hostname, **AES-Sicherheitsschlüssel** (= LGW-Schlüssel der Zentrale), Port
   und ggf. RS485-Pins eintragen.
3. Speichern → Neustart → das Gateway verbindet sich; in der Zentrale als
   HMW-LGW-Interface anlegen (IP + Sicherheitsschlüssel).

## Reverse-Engineering-Basis

Protokoll ermittelt aus RS485-Bus-Mitschnitten und einem LAN-MITM, validiert gegen
**HBWired** (Thorsten Pferdekämper), **Homegear** (`HMW-LGW`) und **FHEM**
(`00_HM485_LAN.pm`). AES liegt **nur auf dem LAN-Hop** (CCU↔LGW); der RS485-Bus ist Klartext.

## Lizenz / Haftung

Privates Hobbyprojekt, **ohne jede Gewähr**. Lizenz noch festzulegen.
„HomeMatic" ist eine Marke der eQ-3 AG — dies ist kein offizielles Produkt und steht in
keiner Verbindung zu eQ-3.
