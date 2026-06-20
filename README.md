# HMW-Gateway-Pro

Konfigurierbare **ESP32-Firmware als Ersatz für ein HomeMatic Wired LAN Gateway (HMW-LGW)**.
Sie verbindet eine HomeMatic-Zentrale (OpenCCU / RaspberryMatic / Homegear) über WLAN/TCP
mit einem klassischen HomeMatic-Wired-RS485-Bus — als wartbares „Gerät" mit Config-Portal,
OTA, Status-Webseite und Watchdog.

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
- **Verbindungs-Überwachung zur Laufzeit tunebar** — Inaktivitäts-Timeout, optionales
  TCP-Keepalive, Unicast-Antwort-Fenster (alle über `/config`, kein Reflash nötig).

## Hardware

- **ESP32** (WLAN + HW-AES).
- **3,3 V RS485-Transceiver** — Auto-Direction-Modul (z. B. XY-K485) oder MAX3485 / SN65HVD72.
  ⚠️ ESP32 ist **nicht 5 V-tolerant**.
- Verdrahtung: `GPIO16` = RX (← Modul RXD/RO), `GPIO17` = TX (→ Modul TXD/DI).
  Bei Auto-Direction-Modul `RS485_DE = -1`.
- Bus: **19200 8N1**, 2-Draht RS485, 120 Ω-Terminierung an den Enden, gemeinsame Masse.

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
2. Captive-Portal öffnet sich → SSID, WLAN-Passwort, Serial/Hostname,
   **AES-Sicherheitsschlüssel** (= LGW-Schlüssel der Zentrale), Port eintragen.
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
