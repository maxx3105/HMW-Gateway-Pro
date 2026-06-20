// config.h -- persistente Konfiguration im NVS (Preferences).
// Loest die hartkodierten Werte des Bare-Gateways ab: alles ueber das
// Config-Portal editierbar und im Flash gespeichert (ueberlebt Reflash).
#pragma once
#include <Arduino.h>
#include <Preferences.h>

struct GwConfig {
    String   ssid;
    String   pass;
    String   serial      = "HMW-LGW01";   // Hostname + LGW-Seriennummer
    String   passphrase;                  // LAN-Sicherheitsschluessel (AES)
    bool     useStaticIp = false;
    uint32_t ip = 0, gw = 0, sn = 0;      // als 32-bit (IPAddress(uint32_t))
    uint16_t port        = 1000;          // LGW-Port
    bool     useAes      = true;          // AES-Verschluesselung aktiv? (false = Klartext)
    // --- Verbindungs-Ueberwachung (zur Laufzeit ueber /config tunebar) ---
    uint16_t rxTimeoutS  = 60;            // Inaktivitaets-Timeout der CCU-Verbindung (s)
    bool     useKeepAlive= false;         // TCP-SO_KEEPALIVE -> toten Peer aktiv erkennen
    uint16_t kaIdleS     = 30;            // Keepalive: Idle vor erster Probe (s)
    uint16_t kaIntvlS    = 10;            // Keepalive: Intervall zwischen Probes (s)
    uint8_t  kaCount     = 3;             // Keepalive: tote Probes bis "Verbindung tot"
    uint16_t ackWaitMs   = 400;           // max. Wartezeit auf die erste Bus-Antwort (Unicast)
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
    c.rxTimeoutS  = p.getUShort("rxto", 60);
    c.useKeepAlive= p.getBool("ka", false);
    c.kaIdleS     = p.getUShort("kaidle", 30);
    c.kaIntvlS    = p.getUShort("kaintvl", 10);
    c.kaCount     = p.getUChar("kacnt", 3);
    c.ackWaitMs   = p.getUShort("ackwait", 400);
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
    p.putUShort("rxto", c.rxTimeoutS);
    p.putBool("ka", c.useKeepAlive);
    p.putUShort("kaidle", c.kaIdleS);
    p.putUShort("kaintvl", c.kaIntvlS);
    p.putUChar("kacnt", c.kaCount);
    p.putUShort("ackwait", c.ackWaitMs);
    p.putBool("valid", true);
    p.end();
}

inline void clear() { Preferences p; p.begin("hmwgw", false); p.clear(); p.end(); }

} // namespace cfg
