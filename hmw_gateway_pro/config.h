// config.h -- persistente Konfiguration im NVS (Preferences).
// Loest die hartkodierten Werte des Bare-Gateways ab: alles ueber das
// Config-Portal editierbar und im Flash gespeichert (ueberlebt Reflash).
#pragma once
#include <Arduino.h>
#include <Preferences.h>

// --- Werks-Defaults Netz-Interface + RS485-Pins (im Sketch VOR dem Include uebersteuerbar).
//     Mit Arduino-Board "WT32-ETH01" (= ESP32-ETH01) automatisch passend: Ethernet an,
//     RS485 auf den Header-Pins RXD=GPIO5 / TXD=GPIO17 -- GPIO16 gehoert dort dem
//     PHY-Oszillator! Beim generischen "ESP32 Dev Module"-Build fuer ein ETH01-Board
//     einfach im Portal Ethernet anhaken + RX-Pin auf 5 stellen (NVS, kein Reflash).
#ifndef DEF_USE_ETH
  #ifdef ARDUINO_WT32_ETH01
    #define DEF_USE_ETH  true
  #else
    #define DEF_USE_ETH  false
  #endif
#endif
#ifndef DEF_RS485_RX
  #ifdef ARDUINO_WT32_ETH01
    #define DEF_RS485_RX 5
  #else
    #define DEF_RS485_RX 16
  #endif
#endif
#ifndef DEF_RS485_TX
  #define DEF_RS485_TX   17
#endif
#ifndef DEF_RS485_DE
  #define DEF_RS485_DE   -1              // -1 = Auto-Direction-Modul (kein DE-Pin)
#endif
#ifndef DEF_RS485_DE_INV
  #define DEF_RS485_DE_INV false         // true = DE ueber Inverter (aktiv-LOW)
#endif

struct GwConfig {
    String   ssid;
    String   pass;
    String   serial      = "HMW-LGW01";   // Hostname + LGW-Seriennummer
    String   passphrase;                  // LAN-Sicherheitsschluessel (AES)
    bool     useStaticIp = false;
    uint32_t ip = 0, gw = 0, sn = 0;      // als 32-bit (IPAddress(uint32_t))
    uint16_t port        = 1000;          // LGW-Port
    bool     useAes      = true;          // AES-Verschluesselung aktiv? (false = Klartext)
    bool     useEth      = DEF_USE_ETH;   // Ethernet (LAN8720, ESP32-ETH01) statt WLAN
    // --- RS485-Pins (DevKit: RX16/TX17 -- ESP32-ETH01: RX5/TX17) ---
    int8_t   rs485Rx     = DEF_RS485_RX;
    int8_t   rs485Tx     = DEF_RS485_TX;
    int8_t   rs485De     = DEF_RS485_DE;
    bool     rs485DeInv  = DEF_RS485_DE_INV;
    // --- Verbindungs-Ueberwachung (zur Laufzeit ueber /config tunebar) ---
    uint16_t rxTimeoutS  = 60;            // Inaktivitaets-Timeout der CCU-Verbindung (s)
    bool     useKeepAlive= false;         // TCP-SO_KEEPALIVE -> toten Peer aktiv erkennen
    uint16_t kaIdleS     = 30;            // Keepalive: Idle vor erster Probe (s)
    uint16_t kaIntvlS    = 10;            // Keepalive: Intervall zwischen Probes (s)
    uint8_t  kaCount     = 3;             // Keepalive: tote Probes bis "Verbindung tot"
    uint16_t ackWaitMs   = 400;           // max. Wartezeit auf die erste Bus-Antwort (Unicast)
    // --- Bus-Zugriff / Kollisionsvermeidung (CSMA/CA, nur Master-Sendepfad) ---
    bool     useCarrierSense = false;     // vor dem Senden auf freien Bus warten (Carrier-Sense)
    uint16_t busIdleMs   = 5;             // Bus muss so lange (ms) still sein, bevor gesendet wird
    bool     useRetransmit = false;       // Kommando bei fehlender Antwort wiederholen (Master-Retransmit)
    uint8_t  sendRetries = 3;             // max. Sendeversuche (1 = kein Retransmit), wie hs485d MAX_SEND_RETRY
    String   webPass;                     // Web-UI-Login (leer = kein Login; Benutzer = admin)
    // --- Selbst-Geraet: Gateway meldet sich zusaetzlich als HMW-Busgeraet an, damit es in der
    //     CCU als konfigurierbares Geraet erscheint. Alle Werte ueber /config aenderbar, damit
    //     das Ausprobieren von Typ/XML-Kombinationen KEINEN Reflash braucht. ---
    bool     selfEnable  = true;          // Selbst-Geraet aktiv (aus = exakt bisheriges Verhalten)
    uint32_t selfAddr    = 1129999999UL;  // eigene Busadresse (nur busweit eindeutig)
    uint8_t  selfType    = 0xAB;          // Geraetetyp-Byte (Antwort auf 'h'); muss zur XML passen (171)
    uint8_t  selfHw      = 0x00;          // HW-Version (Antwort auf 'h')
    uint16_t selfFw      = 0x0102;        // FW-Version (Antwort auf 'v')
    String   selfSerial  = "LGW0000001";  // Seriennummer (Antwort auf 'n'), GENAU 10 Zeichen
    // --- Zweiter Busanschluss (Ring/Split, siehe DUAL-BUS-KONZEPT.md) ---
    // Bus-Betriebsart: 0=SINGLE (ein Bus, Bus B ungenutzt), 1=RING, 2=SPLIT.
    uint8_t  busMode     = 0;
    // Pins Bus B. Default fuer Auto-Direction-Module (XY-K485 o.ae.): kein DE noetig.
    // Am ESP32-ETH01 sind IO14/IO4 frei und unkritisch; IO12 waere als RX unbrauchbar
    // (MTDI-Strapping, Bus-Idle liegt HIGH), als DE dagegen ideal.
    int8_t   rs485Rx2    = 14;
    int8_t   rs485Tx2    = 4;
    int8_t   rs485De2    = -1;            // -1 = Auto-Direction-Modul (kein DE-Pin)
    bool     rs485De2Inv = false;
    // Ring-Ueberwachung: im geschlossenen Ring muss jedes auf A gesendete Frame auf B
    // ankommen. Bleibt das N-mal aus -> Bruch; kommt es M-mal wieder -> Ring geheilt.
    // Getrennte Schwellen = Hysterese gegen Flattern auf einer marginalen Leitung.
    uint16_t ringEchoMs  = 30;            // Zeitfenster fuer das Echo auf dem anderen Port
    uint8_t  ringBreakN  = 3;             // fehlende Echos bis "Ring gebrochen"
    uint8_t  ringHealM   = 5;             // erfolgreiche Echos bis "Ring wieder zu"
    bool     valid       = false;         // schon konfiguriert?
};

namespace cfg {

inline void load(GwConfig& c) {
    Preferences p; p.begin("hmwgw", /*readOnly=*/true);
    c.valid       = p.getBool("valid", false);
    c.ssid        = p.getString("ssid", "");
    c.pass        = p.getString("pass", "");
    c.serial      = p.getString("serial", "HMW-LGW01");
    c.passphrase  = p.getString("pass2", "");
    c.useStaticIp = p.getBool("staticip", false);
    c.ip          = p.getUInt("ip", 0);
    c.gw          = p.getUInt("gw", 0);
    c.sn          = p.getUInt("sn", 0);
    c.port        = p.getUShort("port", 1000);
    c.useAes      = p.getBool("aes", true);
    c.useEth      = p.getBool("eth", DEF_USE_ETH);
    c.rs485Rx     = p.getChar("busrx", DEF_RS485_RX);
    c.rs485Tx     = p.getChar("bustx", DEF_RS485_TX);
    c.rs485De     = p.getChar("busde", DEF_RS485_DE);
    c.rs485DeInv  = p.getBool("businv", DEF_RS485_DE_INV);
    c.rxTimeoutS  = p.getUShort("rxto", 60);
    c.useKeepAlive= p.getBool("ka", false);
    c.kaIdleS     = p.getUShort("kaidle", 30);
    c.kaIntvlS    = p.getUShort("kaintvl", 10);
    c.kaCount     = p.getUChar("kacnt", 3);
    c.ackWaitMs   = p.getUShort("ackwait", 400);
    c.useCarrierSense = p.getBool("cs", false);
    c.busIdleMs   = p.getUShort("busidle", 5);
    c.useRetransmit = p.getBool("rtx", false);
    c.sendRetries = p.getUChar("rtxn", 3);
    c.webPass     = p.getString("webpass", "");
    c.selfEnable  = p.getBool("selfen", true);
    c.selfAddr    = p.getUInt("selfaddr", 1129999999UL);
    c.selfType    = p.getUChar("selftype", 0xAB);
    c.selfHw      = p.getUChar("selfhw", 0x00);
    c.selfFw      = p.getUShort("selffw", 0x0102);
    c.selfSerial  = p.getString("selfser", "LGW0000001");
    c.busMode     = p.getUChar("busmode", 0);
    c.rs485Rx2    = p.getChar("busrx2", 14);
    c.rs485Tx2    = p.getChar("bustx2", 4);
    c.rs485De2    = p.getChar("busde2", -1);
    c.rs485De2Inv = p.getBool("businv2", false);
    c.ringEchoMs  = p.getUShort("ringecho", 30);
    c.ringBreakN  = p.getUChar("ringbrk", 3);
    c.ringHealM   = p.getUChar("ringheal", 5);
    p.end();
}

inline void save(const GwConfig& c) {
    Preferences p; p.begin("hmwgw", false);
    p.putString("ssid", c.ssid);
    p.putString("pass", c.pass);
    p.putString("serial", c.serial);
    p.putString("pass2", c.passphrase);
    p.putBool("staticip", c.useStaticIp);
    p.putUInt("ip", c.ip);
    p.putUInt("gw", c.gw);
    p.putUInt("sn", c.sn);
    p.putUShort("port", c.port);
    p.putBool("aes", c.useAes);
    p.putBool("eth", c.useEth);
    p.putChar("busrx", c.rs485Rx);
    p.putChar("bustx", c.rs485Tx);
    p.putChar("busde", c.rs485De);
    p.putBool("businv", c.rs485DeInv);
    p.putUShort("rxto", c.rxTimeoutS);
    p.putBool("ka", c.useKeepAlive);
    p.putUShort("kaidle", c.kaIdleS);
    p.putUShort("kaintvl", c.kaIntvlS);
    p.putUChar("kacnt", c.kaCount);
    p.putUShort("ackwait", c.ackWaitMs);
    p.putBool("cs", c.useCarrierSense);
    p.putUShort("busidle", c.busIdleMs);
    p.putBool("rtx", c.useRetransmit);
    p.putUChar("rtxn", c.sendRetries);
    p.putString("webpass", c.webPass);
    p.putBool("selfen", c.selfEnable);
    p.putUInt("selfaddr", c.selfAddr);
    p.putUChar("selftype", c.selfType);
    p.putUChar("selfhw", c.selfHw);
    p.putUShort("selffw", c.selfFw);
    p.putString("selfser", c.selfSerial);
    p.putUChar("busmode", c.busMode);
    p.putChar("busrx2", c.rs485Rx2);
    p.putChar("bustx2", c.rs485Tx2);
    p.putChar("busde2", c.rs485De2);
    p.putBool("businv2", c.rs485De2Inv);
    p.putUShort("ringecho", c.ringEchoMs);
    p.putUChar("ringbrk", c.ringBreakN);
    p.putUChar("ringheal", c.ringHealM);
    p.putBool("valid", true);
    p.end();
}

inline void clear() { Preferences p; p.begin("hmwgw", false); p.clear(); p.end(); }

} // namespace cfg
