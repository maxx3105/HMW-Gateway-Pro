# HMW-Gateway-Pro-Dual — Konzept: zwei Busanschlüsse mit Ringtopologie

> **Status:** Analyse/Design. Noch **kein** Code geändert. Dieses Dokument sammelt
> die Erkenntnisse aus der Konzeptphase und skizziert das Software-Layer gegen den
> vorhandenen Single-Bus-Code (`hmw_gateway_pro/hmw_gateway_pro.ino`, Stand v1.2.2).
> Referenzgerät: Homematic IP Wired Access Point **HmIPW-DRAP**.

---

## 1. Ziel & Abgrenzung

Der HmIPW-DRAP besitzt zwei Busanschlüsse und kann als **Ring** (fehlertolerant)
oder als **zwei getrennte Stränge** betrieben werden. Ziel ist, dasselbe für unser
ESP32-LGW-Gateway zu ermöglichen — auf dem **HMW/HBWired-Protokoll** (BidCoS-Wired,
`hs485d`), nicht auf Homematic IP Wired.

**Gewählte Ausbaustufe:** Ring **mit vollem Forwarding im Bruchfall** — nach einem
Leitungsbruch bleiben auch Direktverknüpfungen (Taster↔Aktor) über die Bruchstelle
hinweg funktionsfähig, indem das Gateway zwischen den beiden entstehenden Stichen
als Store-and-Forward-Repeater arbeitet. (Feasibility siehe §5: bestätigt.)

**Wichtig — bleibt unverändert:** Das LGW-Protokoll zur CCU. Die `hs485d` sieht
weiterhin genau *einen* Bus mit N Geräten. Die Redundanz ist komplett gatewayintern:
kein XML, keine `hs485d`-Anpassung, keine Geräte-Firmware.

---

## 2. Referenz HmIPW-DRAP — was wir übernehmen, was nicht

| Aspekt | DRAP | Für uns |
|---|---|---|
| Physik | RS485, 4-adriges Kabel (24 V + GND + A + B) | **identisch übernehmen** |
| Topologie-Wahl | MASTER-Parameter am Wartungskanal (Ring / zwei Stränge) | Config-Feld im Portal |
| Bruch-Erkennung | „prüft, ob Daten an **beiden** Anschlüssen ankommen"; Auto-Umschalten | nachbauen (§6.4) |
| 24-V-Durchleitung | speist NICHT, **leitet 24 V durch** auf beide Ausgänge, misst Strom (650–3000 mA) | HW-seitig nachbauen (§3.4) |
| Fehler-Diagnose | Display/LED + Codes E10–E16, CCU-Servicemeldung | **nur teilweise** möglich (§8) |
| Protokoll/Krypto | Homematic IP Wired, AES-128/CCM, per-Device-Keys | **nicht** unser Weg — wir bleiben HMW |

DRAP-Platine (fotoverifiziert): **STM32F107RCT6** (MCU mit Ethernet-MAC) + **Davicom
DM9162** PHY + **2× VP1780** (je einer pro Port) + **74HC595** + Lastschalter pro Zweig
(24-V-Durchleitung) + Gleichtaktdrosseln + TVS auf A/B. Das ist im Kern unsere BOM —
und unser ESP32-ETH01 integriert MCU+MAC+PHY bereits.

---

## 3. Hardware

### 3.1 Stückliste (aus der DRAP-Platine abgeleitet)

| Block | DRAP | HMW-Gateway-Pro-Dual |
|---|---|---|
| MCU + Ethernet-MAC | STM32F107 | **ESP32** (im ESP32-ETH01 integriert) |
| Ethernet-PHY | DM9162 | LAN8720 (im ESP32-ETH01) |
| RS485-Transceiver | 2× VP1780 | **2× VP1780** (SN65HVD1780) — s. §3.3 |
| 24-V-Zweigschalter | 2× Lastschalter | 2× High-Side-Switch + Strommessung (§3.4) |
| Busschutz | CM-Drosseln + TVS | Gleichtaktdrossel + TVS-Diode je A/B, Serienwiderstände |

### 3.2 ESP32-ETH01-Pinbelegung

UART0 bleibt Debug. Bus A bleibt auf dem bewährten UART (heute `Serial2`,
RX5/TX17). Bus B kommt auf `Serial1` mit remappten Pins:

| Signal | Pin | Anmerkung |
|---|---|---|
| Bus A RX / TX | GPIO5 / GPIO17 | unverändert (ETH01-Header) |
| Bus A DE | frei wählbar (z. B. GPIO32) | oder −1 = Auto-Direction-Modul |
| Bus B RX | **GPIO14** | Eingang |
| Bus B TX | **GPIO4** | Ausgang |
| Bus B DE | **GPIO12** | ideal: MTDI-Strapping = Boot-LOW = Ruhe „Empfangen" |

**Pin-Fallen:**
- **GPIO12 NICHT als RX** — Bus-Idle liegt HIGH, das Board bootet dann mit falscher
  Flash-Spannung. Als **DE-Ausgang** dagegen perfekt (Ruhezustand LOW = Empfangen).
- **GPIO15** hat internen Pullup → als DE ungeeignet (Treiber im Boot aktiv).
- **GPIO2** ist Bootstrap → meiden.
- **Jeder DE-Pin braucht einen externen Pulldown** — während Reset/Boot sind die GPIOs
  hochohmig; ohne Pulldown ist der Sendetreiber in dieser Zeit undefiniert und kann
  den Bus belegen.

Die ETH01-Pins sind knapp. Wer mehr Reserve will, nimmt ein **ESP32-DevKit + externes
LAN-Modul** — dann sind alle sechs Bus-GPIOs unkritisch.

### 3.3 Transceiver-Wahl: VP1780

RS485-Bus verträgt **32 Unit Loads (UL)**. Der Treiber bestimmt, wie viele Knoten
das sind:

| Treiber | UL | max. Knoten | Bemerkung |
|---|---|---|---|
| MAX485 / MAX3485 | 1 | 32 | zu wenig für große Installationen |
| MAX487 / LT1785 | 1/4 | 128 | eq3-Klassik (SW2 = LT1785) |
| **VP1780 (SN65HVD1780)** | **1/10** | **320** | **Empfehlung** |

Der **VP1780** ist genau der Typ auf der DRAP-Platine. Gründe, warum er hier ideal ist:
- **1/10 UL** → das Gateway kostet mit zwei Transceivern nur 0,2 UL.
- **±70 V Fault-Protection** → überlebt den Kurzschluss Datenleitung↔24 V im selben
  Kabel (DRAP-Fehlerbild E14). *Einschränkung:* voll bei DE=L / Gerät aus; beim Senden
  ist die sichere Grenze auf B +30 V — bei 24 V Busspannung in jedem Zustand sicher.
- **Failsafe-Receiver** (Open/Short/Idle) → nach einem Bruch hängt ein Port an offener
  Leitung; der VP1780 liefert dort definiert HIGH statt Rausch-Frames. Für den Ring
  keine Komfort-, sondern eine Notwendigkeitsfunktion.
- **SN75176-Pinout** → 1:1-Footprint-Ersatz für MAX485/MAX487/MAX3485 (nur `A/B`,
  `RO/DI`, `DE//RE`, `VCC/GND`). **Nicht** ersetzbar sind Auto-Direction-Module ohne
  DE-Pin — die brauchen wieder eine DE-Leitung.
- 3,3–5 V, 115 kbps (Faktor 6 Reserve auf 19200 Bd), slew-rate-begrenzt (EMV-freundlich).

Bei gemischten Bussen zählt das schwächste Glied: ein einzelner MAX485 frisst so viel
Budget wie zehn VP1780.

### 3.4 24-V-Durchleitung + Absicherung

Der DRAP speist den Bus **nicht** — externes 24-V-Netzteil —, aber er **leitet die
24 V auf beide Ausgänge durch** und misst den Strom (Parameter 650–3000 mA; max. 3 A
je Zweig). Erst diese Durchleitung macht den Ring **versorgungsseitig** redundant:
sitzt die Einspeisung im Ring, wird jeder Punkt von zwei Seiten erreicht, ein einzelner
Bruch trennt nichts von der Spannung.

Für uns:
- 24 V aufs Gateway, von dort auf **beide** Klemmensätze.
- **Absicherung pro Zweig** (Sicherung/PTC passiv, oder High-Side-Switch mit
  Strombegrenzung aktiv). Der aktive Weg erlaubt zweigweises Abwerfen bei Kurzschluss
  (DRAP-Verhalten E16: Fehler wird **gelatcht** und muss quittiert werden — kein
  Auto-Retry).
- Strommessung braucht Shunt + ADC; für die reine Dual-Bus-Funktion optional.
- **Kurzschluss rettet der Ring nicht** (geschlossener Kreis sieht ihn beidseitig) —
  nur zweigweises Abwerfen hilft.

---

## 4. Topologie & Betriebsarten

Config-Feld `busMode`:

- **SINGLE** — ein Bus, exakt heutiges Verhalten. Bus B ungenutzt. (Rückwärtskompatibel.)
- **RING** — Bus A und Bus B sind die beiden Enden **einer** Schleife.
- **SPLIT** — zwei unabhängige Stränge, permanentes Forwarding (der teurere Modus).

### 4.1 Ring geschlossen (Normalfall)

Elektrisch **ein** Segment. Nur **Bus A sendet**, Bus B ist reiner Mithörer/Wächter.
Dass alles, was A sendet, auf B zurückkommt, **ist** der Ring-Nachweis. Direktverknüp-
fungen laufen nativ (ein Segment, alle hören alles) — **kein Forwarding nötig**.
Geräte-Events kommen zweimal rein (je Port-Ende) → **Dedup** (§6.7).

> Der Normalfall verwendet damit fast unverändert den heutigen, am echten Bus
> verifizierten Single-Bus-Pfad. Die neue Komplexität engagiert sich **erst beim Bruch**.

### 4.2 Ring gebrochen

Die Schleife zerfällt in zwei galvanisch getrennte Stiche:
- Stub A: Gateway-PortA → Dev1 … Dev_k
- Stub B: Gateway-PortB → DevN … Dev_{k+1}

Sie hören sich nur noch **durch das Gateway**. Ein Taster auf Stub A, der einen Aktor
auf Stub B schalten will, wird vom Gateway weitergereicht — inkl. des zurücklaufenden
Bus-ACKs (§6.5/§6.6). Versorgungsseitig behält jeder Stub die Speisung von seinem
Gateway-Ende (dank Durchleitung §3.4).

### 4.3 Split (zwei getrennte Stränge)

Zwei Stränge, die sich **nie** hören. Cross-Strang-Direktverknüpfungen funktionieren
nur mit **permanentem** Forwarding (dasselbe Store-and-Forward wie im Bruchfall, aber
immer aktiv). Deshalb ist Split für HMW der **teurere** Modus, nicht der billigere.
Portal-Beschriftung ehrlich: „zwei getrennte Linien (keine Direktverknüpfungen über
die Linien hinweg)", falls Forwarding hier zunächst weggelassen wird.

---

## 5. Timing-Nachweis (Forwarding im Bruchfall) — bestätigt

Maßgeblich ist die **ACKWAITTIME** aus der echten HBWired-Lib (über alle Geräte-Builds
identisch: Arduino-IDE, Loetmeister-Master, PlatformIO nano328 + 328P):

```
#define DIFS_CONSTANT 210   // Bus muss 210 + rand(0..100) ms frei sein vor dem Senden
#define DIFS_RANDOM   100
#define ACKWAITTIME   200   // ein Gerät wartet max. 200 ms auf sein ACK
#define RETRYIDLETIME 150
```
(`HBWired.cpp:18-25`, Retry-Schleife bis 3× je vollem 200-ms-Fenster in `:127-154`)

Store-and-Forward-Rundlauf bei 19200 Bd/8E1 (= 11 Bit/Byte = 0,573 ms/Byte):

| Schritt | Zeit |
|---|---|
| Kommando A→B weiterleiten (Carrier-Sense ~5 ms + ~9 ms TX) | ~14 ms |
| Aktor verarbeitet + ACK auf B (~3 ms + ~7 ms) | ~10 ms |
| ACK B→A zurückleiten (~5 ms + ~7 ms) | ~12 ms |
| **Summe** | **~40 ms** (real 50–70 ms) |

**~50–70 ms gegen 200 ms → Faktor 3–4 Reserve im ersten Versuch, 3 Versuche vorhanden.**
Verpasst ein Rundlauf das erste Fenster, wiederholt der Taster; der Aktor dedupliziert
über die txSeqNum und quittiert erneut — der Befehl geht nicht verloren, kostet nur
einen Anlauf.

**Das Nadelöhr ist NICHT die ESP32-CPU** (240 MHz; Frame = µs Rechenzeit), sondern die
19200-Baud-Drahtzeit — und die passt fünffach ins ACK-Fenster. Die **128-Byte-Hardware-
FIFO** jedes UARTs fängt zudem ein ~17-Byte-Frame auf, das ankommt, während der andere
Port gerade sendet: nichts geht verloren, es wird nur verspätet gelesen.

> **Kritische Regel:** Das Gateway leitet mit **kurzem** Carrier-Sense weiter
> (`busIdleMs` ~5 ms), **niemals** mit der Geräte-DIFS (210–310 ms) — sonst sprengt
> allein das Sense-Fenster die 200 ms. Das Gateway ist privilegierter Repeater wie die
> `hs485d`, kein gleichberechtigter Peer.

---

## 6. Software-Architektur (gegen den vorhandenen Code)

### 6.1 Der zentrale Grundsatz

Der ganze Umbau lässt sich auf **eine** Frage im Sendepfad reduzieren:

> **Dürfen gerade beide Transceiver treiben?**

- **Nein** (Ring geschlossen) → nur Bus A sendet, Bus B hört. = heutiger Pfad.
- **Ja** (Ring gebrochen ODER Split) → das Kommando geht auf den Port, an dem das Ziel
  hängt (bzw. beide bei Unbekannt/Broadcast), Antworten werden von beiden eingesammelt.

Damit bleibt `handleLan()` strukturell fast unverändert; die Fallunterscheidung sitzt
in einer neuen `BusPort`-Auswahl statt im nackten `Serial2`.

### 6.2 `BusPort`-Abstraktion

Heute ist `Serial2` an ~15 Stellen hart verdrahtet. Kapselung in eine Struktur, davon
zwei Instanzen:

```cpp
// SKIZZE -- nicht kompilierfertig, illustriert die Kapselung.
struct BusPort {
    HardwareSerial& uart;         // Serial2 (A) bzw. Serial1 (B)
    int8_t  rx, tx, de;
    bool    deInv;
    uint32_t lastRxMs = 0;        // heute global busLastRxMs -> pro Port

    void begin(uint32_t baud) {
        if (de >= 0) { pinMode(de, OUTPUT); setTx(false); }
        uart.begin(baud, SERIAL_8E1, rx, tx);
    }
    inline void setTx(bool on) { if (de >= 0) digitalWrite(de, (on != deInv) ? HIGH : LOW); }
    void send(const uint8_t* d, size_t n) {           // = heutiges busSend
        setTx(true); uart.write(d, n); uart.flush();
        if (de >= 0) delayMicroseconds(700);
        setTx(false);
    }
    size_t read(uint8_t* b, size_t max, uint32_t win); // = heutiges busRead (uart statt Serial2)
    size_t readResponse(uint8_t* b, size_t max, uint32_t first, uint32_t gap);
    void   waitIdle(uint16_t idleMs, uint16_t maxMs);  // = heutiges busWaitIdle
    void   ack(uint32_t dev, uint8_t ctrl);            // = heutiges busAck, auf DIESEM Port
    bool   probe(uint32_t prefix, uint8_t validBits);  // = heutiges busProbe
    // txAck(), query() analog fuer den /flash-Pfad
};

BusPort busA{ Serial2, /*rx*/5,  /*tx*/17, /*de*/CFG.rs485De,  CFG.rs485DeInv };
BusPort busB{ Serial1, /*rx*/14, /*tx*/4,  /*de*/12,           CFG.rs485De2Inv };
```

Regel: **Bus A = heutige Pins/Verhalten** (minimaler Diff, verifizierter Pfad bleibt).
Bus B kommt neu dazu. In `busMode == SINGLE` wird `busB` nie initialisiert → das heutige
Gateway ist ein Sonderfall des neuen.

### 6.3 Betroffene Funktionen (Mapping)

| heute (`Serial2`-gebunden) | wird | Anmerkung |
|---|---|---|
| `busTx`, `busSend` | `BusPort::setTx/send` | 1:1 |
| `busRead`, `busReadResponse` | `BusPort::read/readResponse` | `uart` statt `Serial2` |
| `busWaitIdle` (global `busLastRxMs`) | `BusPort::waitIdle` (`port.lastRxMs`) | Idle pro Port |
| `busAck` | `BusPort::ack` | **auf dem Port, wo das Event kam** |
| `busProbe`, `busDiscover` | pro Port | Treffer auf A **oder** B |
| `busTxAck`, `busQuery`, `busFlashRun`, `busDiscoverRun` | pro Port | Flash läuft auf dem Ziel-Port |
| `pollBusEvents` | liest **beide** Ports | + Dedup (§6.7) |
| `handleLan` CMD_SEND | Port-Auswahl (§6.1) | Kernänderung |
| `runGateway` (`Serial2.begin`) | `busA.begin` + ggf. `busB.begin` | modusabhängig |
| `devAddr[32]`, `g_devInfo[32]`, `found[32]` | **[128]** | bis zu 128 Geräte (§6.8) |

### 6.4 Bruch-Erkennung mit Hysterese

Im geschlossenen Ring kommt jedes Bus-A-TX auf Bus B zurück. Wächter:

```
Nach jedem A-Sendevorgang: kam es innerhalb ringEchoTimeoutMs auf B an?
  ja  -> echoOK++, echoMiss=0
  nein-> echoMiss++, echoOK=0
echoMiss >= ringBreakThreshold (N)  -> Zustand BRUCH, Forwarding an
echoOK  >= ringHealThreshold  (M)   -> Zustand GESCHLOSSEN, Forwarding aus
```

Hysterese (N/M getrennt, z. B. N=3, M=5) verhindert Flattern auf einer marginalen
Leitung. Im Forwarding-Modus kann A nicht mehr passiv „sich selbst auf B" hören
(beide Stubs isoliert) → der Heilungs-Test läuft dann über ein **periodisches
Probe-Frame** von A, dessen Rückkehr auf B den geschlossenen Ring signalisiert.

### 6.5 Passive Stub-Zuordnung + selektives Forwarding

Nach einem Bruch muss das Gateway wissen, welches Gerät auf welchem Stub hängt.
**Primär passiv** (keine Bus-Störung): jedes Frame, das ein Gerät sendet, kommt
post-Bruch auf **genau einem** Port rein → `senderPort[addr] = Port` mitschreiben.
Nach kurzer Zeit sind alle aktiven Geräte gemappt.

```
Frame F auf Port X gehört:
  F.target == CENTRAL              -> wie heute: an CCU ('e') + selbst ACKen auf X
  F.target ist Gerät, senderPort[F.target] == X   -> lokal, echter Aktor ACKt selbst; nichts tun
  F.target ist Gerät, == anderer Port Y           -> forwardAcross(X -> Y, F)
  F.target unbekannt / Broadcast                  -> auf BEIDE Ports spiegeln
```

**Fallback deterministisch:** bei Unklarheit die vorhandene `busDiscover` **pro Port**
laufen lassen (Bruch ist selten, kurze Störung tolerierbar).

`forwardAcross` relayed das **rohe** Frame byte-genau (Target/Control/Sender/Daten/CRC
unverändert — Repeater, kein Re-Origination; txSeqNum bleibt, damit das Ziel über
Retransmits korrekt dedupliziert):

```
forwardAcross(src, dst, rawFrame):
    dst.waitIdle(busIdleMs, ...)          // KURZ, nicht DIFS!
    while (dst.uart.available()) dst.uart.read();   // eigenes Echo/Alt-Bytes weg
    dst.send(rawFrame)                    // Frame 1:1 weiter
    ack = dst.readResponse(ackWaitMs, gap)// echtes ACK vom Ziel-Aktor abwarten
    if (ack) {
        src.waitIdle(busIdleMs, ...)
        src.send(ack)                     // ACK zurueck an den Absender-Stub
    }
```

### 6.6 ACK-Ownership über die Bruchstelle

Der subtile Protokollpunkt: Das Gateway darf ein Cross-Stub-Frame **nicht** selbst
quittieren — sonst meldet es dem Taster „zugestellt", bevor es stimmt. Es forwardet
transparent und lässt das **echte** ACK des Ziel-Aktors zurücklaufen. Nur Frames an
**CENTRAL** quittiert das Gateway weiterhin selbst (wie heute). Deshalb die Fallunter-
scheidung in §6.5 nach `F.target`.

### 6.7 Dedup im geschlossenen Ring

Jedes Geräte-Event kommt zweimal (einmal je Port-Ende der Schleife). `pollBusEvents`
liest künftig beide Ports; ein kurzer Ringpuffer über zuletzt gesehene
`(sender, seqNum, dataLen, crc)` unterdrückt das Duplikat (analog der heutigen
`lastQueryAddr`-De-Dup, nur portübergreifend).

### 6.8 Discovery über zwei Ports

`busDiscover` je Port laufen lassen, Ergebnisse mergen, Port pro Fund vermerken
(füttert `senderPort[]` schon deterministisch vor). Grenzen anheben: bis zu 128 Geräte
gesamt → `found[128]`, `devAddr[128]`, `g_devInfo[128]` (128×~18 B ≈ 2,3 KB, unkritisch
auf ESP32).

---

## 7. Config-Erweiterungen (`config.h` / `GwConfig`)

```cpp
uint8_t busMode      = 0;      // 0=SINGLE, 1=RING, 2=SPLIT
// zweiter Port:
int8_t  rs485Rx2     = 14;
int8_t  rs485Tx2     = 4;
int8_t  rs485De2     = 12;
bool    rs485De2Inv  = false;
// Bruch-Erkennung:
uint16_t ringEchoTimeoutMs = 30;
uint8_t  ringBreakThreshold = 3;   // N
uint8_t  ringHealThreshold  = 5;   // M
```
+ Portal-Felder (Modus-Dropdown, Pins Bus B, Schwellen) analog zu den vorhandenen
RS485-Feldern; NVS-Load/Save wie gehabt.

> **Vor dem ersten echten Dual-Betrieb:** `GH_OWNER`/`GH_REPO` im Sketch
> (`hmw_gateway_pro.ino`) zeigen noch auf **maxx3105/HMW-Gateway-Pro** — sonst zieht
> sich ein Dual-Gateway per Auto-Update die **Single-Bus-Firmware** und überschreibt
> sich selbst. Muss auf das Dual-Repo umgestellt werden.

**Schutzbedingung analog DRAP-E15** („konfigurierte ≠ tatsächliche Verkabelung"): Vor
dem ersten simultanen TX auf beiden Ports (Split-Modus) prüfen, ob ein auf Bus A
gesendetes Frame auf Bus B ankommt. Wenn **ja**, sind die Ports in Wahrheit verbunden
(Ring) → simultanes Treiben würde denselben Draht kollidieren lassen → im Split-Modus
**verweigern** und Fehler melden, nicht senden.

---

## 8. Was NICHT geht / offene Punkte

- **Bruch-Meldung an die CCU:** Der DRAP meldet den Bruch als Servicemeldung — er ist
  ein angelerntes Gerät mit Wartungskanal. Behandelt man das Gateway nur als reines
  **Interface**, ist keine Servicemeldung möglich (nur Web-UI-Status, Log, Push/MQTT).
  **Aufgehoben durch §11:** meldet sich das Gateway zusätzlich als *eigenes* Busgerät
  an, kann es den Ringbruch sehr wohl als Servicemeldung an die CCU schicken — der
  Wartungskanal `ERROR_RING_BROKEN` in `ccu/hs485types/hmw_lgw_dual.xml` tut genau das.
  Mindestanforderung bleibt: die Web-Statusseite zeigt den Ringzustand.
- **Strommessung/Übertemperatur (E10/E11):** braucht Zusatz-HW (Shunt+ADC / NTC).
  Für die reine Dual-Bus-Funktion optional; ohne sie keine E11/E14/E16-Analogie.
- **Kurzschluss:** Ring rettet ihn nicht; nur aktives zweigweises Abwerfen (§3.4).
- **Blind-Fenster beim Forwarding:** während eines Rundlaufs (~15–30 ms) ist der jeweils
  andere Port kurz unbedient; die UART-FIFO fängt ein Frame auf, darüber hinaus greift
  die Geräte-Retransmission. Gleiches Fehlermodell wie heute (Gateway kann beim Senden
  ein Frame verpassen), nur häufiger — kein neuer Korrektheitsverlust.

---

## 9. Umbau-Reihenfolge (Vorschlag)

1. `GH_OWNER`/`GH_REPO` auf das Dual-Repo umstellen (sonst Selbst-Überschreibung).
2. `BusPort`-Abstraktion einführen, **Bus A auf Serial2 wie heute** — Verhalten
   bit-identisch, reines Refactoring, am echten Bus gegenprüfen (Regressionstest).
3. `busMode`-Config + Portal-Felder, Default **SINGLE** (= heutiges Verhalten).
4. Bus B initialisieren; **RING geschlossen** (nur A sendet, B hört + Dedup) — ohne
   Forwarding. Erst hier den Ring-Nachweis (Echo A→B) messen.
5. Bruch-Erkennung + `forwardAcross` (Store-and-Forward) — der eigentliche Kern.
6. Passive Stub-Map + selektives Forwarding.
7. Discovery über zwei Ports, Limits auf 128.
8. Optional: SPLIT-Modus (+ E15-Schutzbedingung), Strommessung, Push-Meldung.
9. Optional/parallel: **Selbst-Gerät** (§11) — erst der billige Akzeptanztest
   (§11.5), dann `handleSelfFrame` + `selfEeprom`↔`CFG`, dann die XML ausrollen. Bringt
   die Config in die CCU **und** die Ringbruch-Servicemeldung. Unabhängig von 2–8
   entwickelbar.

Jede Stufe ist für sich am echten Bus verifizierbar; Stufe 2–4 halten das Risiko klein,
weil der verifizierte Single-Bus-Pfad bis zuletzt intakt bleibt.

---

## 10. DRAP-Fehlercodes als Referenz (aus der Anleitung)

| Code | Bedeutung | Für uns relevant |
|---|---|---|
| E10 | Übertemperatur | nur mit NTC |
| E11 | Unterspannung (Bus zu niedrig) | nur mit Spannungsmessung |
| E14 | Kurzschluss Datenleitung ↔ 24 V | VP1780-Fault-Protection deckt das HW-seitig |
| **E15** | **konfigurierte ≠ tatsächliche Verkabelung** | **Schutzbedingung §7** |
| E16 | Kurzschluss Versorgung (gelatcht, quittierpflichtig) | nur mit Zweigschalter |

---

## 11. Gateway als konfigurierbares CCU-Gerät (Selbst-Adresse)

**Idee:** Das Gateway meldet sich zusätzlich zu seiner LGW-Interface-Rolle als *eigenes*
Busgerät auf einer eigenen Adresse an. Damit erscheint es in der OpenCCU wie ein Gerät
mit Kanälen und MASTER-Paramset — die Dual-Bus-Konfiguration (`BUS_MODE`: Ring/Split)
wird dort editierbar, **exakt wie das „Buskonfiguration"-Dropdown des HmIPW-DRAP**.

XML-Entwurf: **[`ccu/hs485types/hmw_lgw_dual.xml`](ccu/hs485types/hmw_lgw_dual.xml)**
(am realen `hbw_sys_pm.xml` modelliert, Kanalstruktur am **echten DRAP** abgeglichen).
Exponiert:
- **Geräte-MASTER:** `BUS_MODE` (Option SINGLE/RING/SPLIT), `RING_BREAK/HEAL_THRESHOLD`,
  `RING_ECHO_TIMEOUT`, `RETRANSMIT`, `OWN_ADDRESS` (Busadresse aus der CCU setzbar,
  wie bei jedem HBWired-Gerät in den letzten 4 EEPROM-Bytes, Offset 1120000000).
- **Wartungskanal 0:** `ERROR_RING_BROKEN` als sticky Servicemeldung → **der Ringbruch
  taucht als Servicemeldung in der CCU auf** (hebt die §8-Einschränkung auf); dazu
  `RING_STATE` (Klartext-Option CLOSED/BROKEN_A/BROKEN_B/SPLIT).
- **Kanäle 1 + 2 = BUS 1 / BUS 2** (ein `<channel index="1" count="2">` erzeugt beide),
  je **vier Werte wie am echten DRAP** (Status-und-Bedienung-Screenshot):
  `OUTPUT_VOLTAGE` (V), `VOLTAGE_STATUS` (Normal/Over/Under), `CURRENT` (mA, zeigt
  „&lt; 100.0 mA" unter Schwelle wie der DRAP), `CURRENT_STATUS` (Normal/Overload).
  **Voraussetzung Mess-HW:** Spannung ist billig (Teiler + ADC je Zweig), Strom braucht
  den Shunt — derselbe Block wie der High-Side-Zweigschalter für E16 (§3.4). Ohne
  Messung bleiben diese vier Werte Platzhalter; `BUS_MODE`/Ring-Status funktionieren
  unabhängig davon.

### 11.1 Warum das sauber ins Gateway passt

Ein an die eigene Bus-Adresse gerichtetes Frame kommt über LAN als `CMD_SEND` herein.
Statt es auf den RS485-Bus zu legen, **kurzschließt** das Gateway es intern und
antwortet direkt über LAN. → **null Buslast** für die Gateway-Konfiguration, und kein
Henne-Ei-Problem (die CCU erreicht das Gateway per LAN unabhängig vom Buszustand).

### 11.2 Bus-Kommandos, die das Selbst-Gerät beantworten muss

Die CCU liest/schreibt Geräte-Config über EEPROM-Kommandos (verifiziert in
`HBWired.cpp:430-591`). Das Selbst-Gerät emuliert genau diese für seine Adresse:

| Kmd | Byte | Bedeutung | Selbst-Behandlung |
|---|---|---|---|
| `h` | 0x68 | Typ + HW-Version | `[0x70, 0x01]` (aus `<supported_types>`) |
| `v` | 0x76 | Firmware-Version | `[0x01,0x00]` (≥ XML-`cond_op GE`) |
| `n` | 0x6E | Seriennummer | 10 Byte `CFG.serial` |
| `R` | 0x52 | Read EEPROM `[adrHi,adrLo,len]` | `len` Byte aus `selfEeprom[]` |
| `W` | 0x57 | Write EEPROM `[adrHi,adrLo,_,_,daten…]` | in `selfEeprom[]` schreiben, ACK |
| `C` | 0x43 | Config anwenden | `selfEeprom → CFG`, `cfg::save`, Ports neu, ACK |
| `s`/`x`/`S` | — | Level set/get | für Status-Kanal: `BUS_STATE_EVENT` liefern |

`i`/Discovery: das Gateway hängt seine **eigene** Adresse zusätzlich in die `found[]`-
Liste (in `busDiscover`/dem `CMD_DISCOVERY`-Zweig).

### 11.3 Skizze: Selbst-Adresse in `handleLan()`

Im vorhandenen `CMD_SEND`-Zweig ([hmw_gateway_pro.ino:583](hmw_gateway_pro/hmw_gateway_pro.ino)),
**vor** `busSend`, eine Abzweigung einziehen:

```cpp
// SKIZZE -- nicht kompilierfertig.
if (pl[0] == lgw::CMD_SEND) {
    uint32_t tgt = ((uint32_t)pl[2]<<24)|((uint32_t)pl[3]<<16)|((uint32_t)pl[4]<<8)|pl[5];

    // NEU: an das Gateway selbst adressiert? -> intern, NICHT auf den Bus.
    if (CFG.busMode != SINGLE && g_ownBusAddr && tgt == g_ownBusAddr) {
        uint8_t bus[300]; size_t bl = lgw::embeddedToBus(pl + 2, plen - 2, bus);
        hmw::Frame f;
        if (hmw::parseFrame(bus, bl, &f) && f.dataLen) {
            uint8_t rp[260]; uint8_t rpl = 0;
            bool ok = handleSelfFrame(f, rp, &rpl);     // R/W/C/h/v/n intern beantworten
            if (ok) {                                    // Antwort als 'r' zurueck an die CCU
                uint8_t lan[320];
                sendLan(cli, cr, lan, lgw::lanEncode(idx, rp, rpl, lan));
            }
        }
        return;                                          // Bus bleibt unberuehrt
    }
    ... // ab hier unveraendert: busWaitIdle(); busSend(bus, bl); ...
}
```

`handleSelfFrame()` schaut auf `f.data[0]` (das Kmd-Byte) und bedient die Tabelle aus
§11.2. Die emulierte EEPROM ist ein `uint8_t selfEeprom[1024]`, beim Boot aus `CFG`
befüllt; `C` dekodiert sie zurück nach `CFG` und persistiert:

```cpp
// SKIZZE
uint8_t selfEeprom[1024];
void encodeSelfEeprom() {                 // CFG -> selfEeprom (Boot / nach Web-Edit)
    selfEeprom[0x0001] = CFG.busMode;
    selfEeprom[0x0002] = CFG.ringBreakThreshold;
    selfEeprom[0x0003] = CFG.ringHealThreshold;
    selfEeprom[0x0004] = (uint8_t)CFG.ringEchoTimeoutMs;
    selfEeprom[0x0005] = CFG.useRetransmit ? 1 : 0;
    // 0x03FC..0x03FF = OWN_ADDRESS (big-endian), 0x0008.. = CENTRAL_ADDRESS
}
void applySelfConfig() {                  // selfEeprom -> CFG (nach Kmd 'C')
    CFG.busMode           = selfEeprom[0x0001];
    CFG.ringBreakThreshold= selfEeprom[0x0002];
    // ... OWN_ADDRESS lesen -> g_ownBusAddr
    cfg::save(CFG);
    // Ports/Modus neu anwenden (ggf. busB.begin / Modus-Wechsel)
}
```

### 11.4 Zwei Editoren, ein Speicher

Config gibt es dann an zwei Stellen: **Web-Portal** und **CCU-Paramset**, beide auf
dieselbe `CFG`/NVS. Regeln:
- CCU schreibt (`W`+`C`) → `applySelfConfig` persistiert + wendet an.
- Web schreibt → `cfg::save` **und** `encodeSelfEeprom()` nachziehen, sonst zeigt die
  CCU beim nächsten Read alte Werte (sie cached das Paramset). Der DRAP hat für genau
  diese Resync den „Restore Config"-Knopf.
- Konfliktregel festlegen (Vorschlag: letzter Schreiber gewinnt; CCU-Read triggert
  `CONFIG_PENDING`, bis übernommen).

### 11.5 Die eine offene Frage (empirisch klären)

Ob `hs485d`/OpenCCU akzeptiert, dass **dasselbe LGW-Interface zusätzlich ein Busgerät**
auf einer Adresse ist, ist nicht verifiziert. Der DRAP macht genau das — aber auf der
HmIP-Seite. Für HMW sehr wahrscheinlich (adressbasierter Geräte-Layer), aber zu prüfen.

**Billiger Test (ohne eine Zeile neuen Config-Code):** dem Gateway eine Bus-Adresse
geben, es bei Discovery mit einem *vorhandenen* Typ (z. B. `h`→ ein bekanntes Byte)
antworten lassen und schauen, ob OpenCCU es im Posteingang zeigt und interrogieren kann.
Erscheint es → der Rest ist XML-Fleißarbeit.
