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
    String   webPass;                     // Web-UI-Login (leer = kein Login; Benutzer = admin)
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
    c.webPass     = p.getString("webpass", "");
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
    p.putString("webpass", c.webPass);
    p.putBool("valid", true);
    p.end();
}

inline void clear() { Preferences p; p.begin("hmwgw", false); p.clear(); p.end(); }

} // namespace cfg
