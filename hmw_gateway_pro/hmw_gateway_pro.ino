/*
 * hmw_gateway_pro.ino  --  konfigurierbare HMW-LGW-Firmware (ESP32).
 *
 * Protokoll-Logik aus hmw_protocol.h / hmw_lgw.h (verifiziert).
 * Features: NVS-Config, Captive-Portal, Gateway aus CFG, OTA, AES abschaltbar,
 *           Status-Webseite, Watchdog, WLAN ODER Ethernet (ESP32-ETH01/LAN8720).
 * Libraries: "ESP Async WebServer" (ESP32Async) + "Async TCP".
 *
 * ESP32-ETH01 (WT32-ETH01): Ethernet im Portal anhaken; RS485 an die Header-Pins
 * RXD=GPIO5 / TXD=GPIO17 (GPIO16 schaltet dort den 50-MHz-PHY-Oszillator, GPIO0
 * traegt den RMII-Takt -- beide tabu). Kein BOOT-Taster: Portal oeffnet bei
 * leerer Config oder wenn das Netz 20 s nicht kommt (WLAN-AP HMW-GW-xxxx).
 */
#include <WiFi.h>
#include <ETH.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <esp_task_wdt.h>
#include <lwip/sockets.h>
#include "config.h"
#include "hmw_protocol.h"
#include "hmw_lgw.h"
#include "log_tee.h"

// --- Firmware-Version: erscheint auf der Status-Seite, im Footer JEDER Web-Seite und im
//     Boot-Log. FW_VERSION = menschliche Version, __DATE__/__TIME__ = eindeutiger Build-
//     Stempel -> geflashte Staende lassen sich nie verwechseln. Bei Aenderungen erhoehen.
#define FW_VERSION "1.1.2"

// --- Auto-Update via GitHub Releases (oeffentliches Repo -> kein Token noetig).
//     Das Gateway prueft das neueste Release und zieht die passende .bin per HTTPS.
//     .bin-Name folgt dem CI-Muster hmw-gateway-pro-<version>.bin.
#define GH_OWNER "maxx3105"
#define GH_REPO  "HMW-Gateway-Pro"

// Web-Ring-Log: 1 = aktiv (jedes Serial.print* wird zusaetzlich in einen 8-KB-RAM-Ring
// gespiegelt, abrufbar unter /log -- Anlernmitschnitt ohne USB), 0 = aus (nur UART0).
// Diagnose 2026-07-05 hat bestaetigt: das Logging beeinflusst das RS485-Timing NICHT
// (die Discovery-Aussetzer waren ein Bus-Hardware-Problem) -> standardmaessig an.
#define WEB_LOG 1
// Muss NACH allen System-/Lib-Includes stehen; Serial2 (RS485) ist ein eigenes Token
// und bleibt vom #define unberuehrt. Siehe log_tee.h.
LogTee Logger;
#if WEB_LOG
  #define Serial Logger
#endif

#ifndef ESP_ARDUINO_VERSION_VAL
  #define ESP_ARDUINO_VERSION_VAL(a,b,c) 0
#endif

const int      PIN_CONFIG = 0;
const char*    AP_PASS    = "hmwgwconfig";
// RS485-Pins liegen jetzt in der NVS-Config (Portal bzw. /config), Defaults in config.h:
// DevKit RX16/TX17 -- ESP32-ETH01 RX5/TX17, DE -1 = Auto-Direction-Modul.
const uint32_t BUS_BAUD   = 19200;
const uint32_t PROBE_WINDOW_MS = 12;
const uint32_t WDT_TIMEOUT_S   = 30;

// --- Ethernet-PHY (nur bei CFG.useEth aktiv). Werte = ESP32-ETH01 / WT32-ETH01
//     (LAN8720 an RMII). Andere LAN8720-Boards hier anpassen, z.B. Olimex
//     ESP32-POE: ADDR 0, POWER 12, CLK ETH_CLOCK_GPIO17_OUT.
const int ETH01_PHY_ADDR  = 1;
const int ETH01_PIN_POWER = 16;        // Enable des externen 50-MHz-Oszillators
const int ETH01_PIN_MDC   = 23;
const int ETH01_PIN_MDIO  = 18;
#define   ETH01_PHY_TYPE    ETH_PHY_LAN8720
#define   ETH01_CLK_MODE    ETH_CLOCK_GPIO0_IN

// --- Bus-Diagnose: LAN-Kommandos + jedes Bus-TX/RX als Hexdump im Log. Zur LAUFZEIT
//     ueber die /log-Seite umschaltbar (kein Reflash noetig), Standard AUS fuer den
//     Dauerbetrieb -> bei Bedarf (z.B. Anlern-Mitschnitt) per Web einschalten.
volatile bool g_debugBus = false;
static void dbgHex(const char* tag, const uint8_t* d, size_t n) {
    if (!g_debugBus) return;
    Serial.printf("# %s [%u]", tag, (unsigned)n);
    for (size_t i = 0; i < n; i++) Serial.printf(" %02X", d[i]);
    Serial.println();
}

GwConfig        CFG;
uint8_t         aesKey[16];
AsyncWebServer  webServer(80);
DNSServer       dns;
WiFiServer      lgwServer(1000);
bool            portalMode    = false;
bool            rebootPending = false;
uint32_t        rebootAt      = 0;

// --- Status (von der Status-Seite gelesen) ---
uint32_t        bootMs   = 0;
volatile bool   ccuConn  = false;
volatile uint32_t lastCcuRxMs = 0;       // millis() des letzten CCU-Bytes (fuer Status)
volatile uint32_t lanFrames = 0;
uint32_t        devAddr[32];
volatile int    devCount = 0;
char            lastEvent[48] = "-";
uint32_t        lastQueryAddr = 0;       // Ziel der letzten Unicast-Abfrage (De-Dup fuer spurious 'e')
uint32_t        lastQueryMs   = 0;

// --- Auto-Update-Status (Web-UI liest; loop schreibt). char[] statt String, weil
//     cross-task gelesen -> kein Realloc (konsistent mit lastEvent). ---
char            g_updLatest[16] = "";    // zuletzt ermittelte neueste Version (ohne 'v')
char            g_updStatus[72] = "noch nicht geprueft";
volatile bool   g_updAvail   = false;    // neuere Version verfuegbar?
volatile bool   g_doCheckUpd = false;    // Web -> loop: Release pruefen
volatile bool   g_doInstall  = false;    // Web -> loop: Update installieren
static void setUpdStatus(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(g_updStatus, sizeof(g_updStatus), fmt, ap); va_end(ap);
}

// ============================== Helfer ====================================== //
static String ipStr(uint32_t v) { return IPAddress(v).toString(); }
static uint32_t parseIp(const String& s) { IPAddress a; a.fromString(s); return (uint32_t)a; }
static String esc(const String& s) {
    String o; for (char c : s) { if (c=='"') o+="&quot;"; else if (c=='<') o+="&lt;";
        else if (c=='>') o+="&gt;"; else if (c=='&') o+="&amp;"; else o+=c; } return o;
}
static String pval(AsyncWebServerRequest* r, const char* n) {
    return r->hasParam(n, true) ? r->getParam(n, true)->value() : String();
}
bool wantConfigPortal() { pinMode(PIN_CONFIG, INPUT_PULLUP); delay(50); return digitalRead(PIN_CONFIG) == LOW; }
void sendLan(WiFiClient& cli, lgw::Crypto* cr, const uint8_t* lan, size_t ln) {
    if (g_debugBus) {
        uint8_t c = (ln > 3) ? lan[3] : 0;   // FD | size | idx | cmd ...
        Serial.printf("# LAN-TX '%c'(%02X) [%u]", (c >= 32 && c < 127) ? c : '.', c, (unsigned)ln);
        for (size_t i = 0; i < ln; i++) Serial.printf(" %02X", lan[i]);
        Serial.println();
    }
    if (cr) { uint8_t enc[320]; cr->encrypt(lan, enc, ln); cli.write(enc, ln); }
    else cli.write(lan, ln);
}

// ============================== Web-Seiten ================================== //
const char* WEB_USER = "admin";          // Benutzername fuers Web-Login (Passwort = CFG.webPass)
// Zugriff erlaubt: im Portal (AP-PW schuetzt), wenn kein Passwort gesetzt, oder bei korrektem Login.
bool webAuthed(AsyncWebServerRequest* r) {
    return portalMode || CFG.webPass.length() == 0 || r->authenticate(WEB_USER, CFG.webPass.c_str());
}
bool authFail(AsyncWebServerRequest* r) {   // true => 401 gesendet, Handler abbrechen
    if (webAuthed(r)) return false;
    r->requestAuthentication();
    return true;
}

String pageHead(const String& title, int refresh = 0) {
    String h = F("<!DOCTYPE html><html><head><meta charset=utf-8>"
                 "<meta name=viewport content='width=device-width,initial-scale=1'>");
    if (refresh > 0) h += "<meta http-equiv=refresh content=" + String(refresh) + ">";
    h += F("<style>"
           ":root{--bg:#eef1f4;--card:#fff;--fg:#23272e;--mut:#6b7280;--line:#e4e7eb;--acc:#2c6fb3;--ok:#2e7d32;--bad:#c62828}"
           "*{box-sizing:border-box}body{font-family:system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--fg);margin:0;padding:1em}"
           ".card{max-width:34em;margin:1em auto;background:var(--card);border-radius:12px;box-shadow:0 1px 5px #0001;padding:1.1em 1.3em}"
           "h1{font-size:1.25em;margin:0 0 .2em}h2{font-size:.82em;text-transform:uppercase;letter-spacing:.04em;color:var(--acc);margin:1.3em 0 .3em}"
           "table{border-collapse:collapse;width:100%}td{padding:.38em .3em;border-bottom:1px solid var(--line)}"
           "td:first-child{color:var(--mut);width:11em}tr:last-child td{border-bottom:0}"
           "label{display:block;margin:.5em 0 .12em;font-weight:600;font-size:.9em}"
           "input{width:100%;padding:.45em;border:1px solid #cfd4da;border-radius:7px;font-size:1em}"
           "input[type=checkbox]{width:auto;margin-right:.45em;vertical-align:-1px}"
           ".badge{display:inline-block;padding:.08em .65em;border-radius:1em;color:#fff;font-size:.82em;font-weight:700}"
           ".ok{background:var(--ok)}.bad{background:var(--bad)}"
           "button{margin-top:1.3em;padding:.6em 1.5em;background:var(--acc);color:#fff;border:0;border-radius:7px;font-size:1em;cursor:pointer}"
           "a{color:var(--acc);text-decoration:none}.links{margin-top:1.3em;font-size:.95em}"
           "</style><title>");
    h += title;
    h += F("</title></head><body><div class=card><h1>HMW-LGW</h1>");
    return h;
}
String pageFoot() {
    return F("<div style='margin-top:1.4em;font-size:.72em;color:var(--mut);text-align:center'>"
             "HMW-LGW v" FW_VERSION " &middot; Build " __DATE__ " " __TIME__ "</div>"
             "</div></body></html>");
}

String formHtml() {
    String h = pageHead("HMW-LGW Setup");
    h += F("<form method=POST action=/save><h2>Netzwerk-Interface</h2>");
    h += "<label><input type=checkbox name=eth " + String(CFG.useEth ? "checked" : "") +
         "> Ethernet statt WLAN (LAN8720, z.B. ESP32-ETH01)</label>";
    h += F("<h2>WLAN (nur ohne Ethernet)</h2>");
    h += "<label>SSID</label><input name=ssid value='" + esc(CFG.ssid) + "'>";
    h += "<label>Passwort</label><input name=pass type=password value='" + esc(CFG.pass) + "'>";
    h += F("<h2>LGW / Identit&auml;t</h2>");
    h += "<label>Serial / Hostname</label><input name=serial value='" + esc(CFG.serial) + "'>";
    h += "<label>LGW-Port</label><input name=port type=number value='" + String(CFG.port) + "'>";
    h += F("<h2>AES</h2>");
    h += "<label>Passphrase (Sicherheitsschl&uuml;ssel)</label><input name=pass2 value='" + esc(CFG.passphrase) + "'>";
    h += "<label><input type=checkbox name=aes " + String(CFG.useAes ? "checked" : "") + "> AES-Verschl&uuml;sselung aktiv</label>";
    h += F("<h2>Netzwerk</h2>");
    h += "<label><input type=checkbox name=staticip " + String(CFG.useStaticIp ? "checked" : "") + "> feste IP statt DHCP</label>";
    h += "<label>IP</label><input name=ip value='" + ipStr(CFG.ip) + "'>";
    h += "<label>Gateway</label><input name=gw value='" + ipStr(CFG.gw) + "'>";
    h += "<label>Subnetz</label><input name=sn value='" + ipStr(CFG.sn) + "'>";
    h += F("<h2>RS485-Bus</h2>");
    h += "<label>RX-Pin (DevKit 16 &middot; ESP32-ETH01 5)</label><input name=busrx type=number value='" + String(CFG.rs485Rx) + "'>";
    h += "<label>TX-Pin (Standard 17)</label><input name=bustx type=number value='" + String(CFG.rs485Tx) + "'>";
    h += "<label>DE/RE-Pin (&minus;1 = Auto-Direction-Modul)</label><input name=busde type=number value='" + String(CFG.rs485De) + "'>";
    h += "<label><input type=checkbox name=businv " + String(CFG.rs485DeInv ? "checked" : "") +
         "> DE invertiert (aktiv-LOW, Inverter-Hardware)</label>";
    h += F("<h2>Verbindungs-&Uuml;berwachung</h2>");
    h += "<label>CCU-Inaktivit&auml;ts-Timeout (s, 20&ndash;600)</label><input name=rxto type=number value='" + String(CFG.rxTimeoutS) + "'>";
    h += "<label><input type=checkbox name=ka " + String(CFG.useKeepAlive ? "checked" : "") + "> TCP-Keepalive (toten Peer aktiv erkennen)</label>";
    h += "<label>Keepalive Idle (s)</label><input name=kaidle type=number value='" + String(CFG.kaIdleS) + "'>";
    h += "<label>Keepalive Intervall (s)</label><input name=kaintvl type=number value='" + String(CFG.kaIntvlS) + "'>";
    h += "<label>Keepalive Count</label><input name=kacnt type=number value='" + String(CFG.kaCount) + "'>";
    h += "<label>Unicast-Antwort-Wartezeit (ms, 50&ndash;2000)</label><input name=ackwait type=number value='" + String(CFG.ackWaitMs) + "'>";
    h += F("<h2>Web-Login</h2>");
    h += "<label>Passwort (leer = kein Login &middot; Benutzer = <b>admin</b>)</label><input name=webpass type=password value='" + esc(CFG.webPass) + "'>";
    h += F("<button type=submit>Speichern &amp; Neustart</button></form>"
           "<h2>Diagnose</h2>");
    h += g_debugBus ? F("<p>Bus-Debug: <b>AN</b> (Bus-Hexdumps im Log) &middot; <a href='/config?debug=0'>ausschalten</a></p>")
                    : F("<p>Bus-Debug: <b>aus</b> &middot; <a href='/config?debug=1'>einschalten</a> (Bus-Hexdumps im Log)</p>");
    h += F("<div class=links><a href=/>&larr; Status</a> &middot; <a href=/log>System-Log</a></div>");
    h += pageFoot();
    return h;
}

String statusHtml() {
    uint32_t up = (millis() - bootMs) / 1000;
    String h = pageHead("HMW-LGW Status", 5);
    h += F("<table>");
    auto row = [&](const String& k, const String& v) { h += "<tr><td>" + k + "</td><td>" + v + "</td></tr>"; };
    row("Serial/Host", esc(CFG.serial));
    row("IP",          netIp().toString());
    if (CFG.useEth)
        row("Ethernet", ETH.linkUp() ? String(ETH.linkSpeed()) + " Mbit/s" + (ETH.fullDuplex() ? ", Voll-Duplex" : "")
                                     : String("<span class='badge bad'>kein Link</span>"));
    else
        row("WLAN",    esc(WiFi.SSID()) + " (" + String(WiFi.RSSI()) + " dBm)");
    row("RS485-Pins",  "RX " + String(CFG.rs485Rx) + " / TX " + String(CFG.rs485Tx) +
                       (CFG.rs485De >= 0 ? " / DE " + String(CFG.rs485De) : " / Auto-Dir"));
    row("AES",         CFG.useAes ? "an" : "aus");
    row("LGW-Port",    String(CFG.port));
    row("CCU",         ccuConn ? "<span class='badge ok'>verbunden</span>" : "<span class='badge bad'>getrennt</span>");
    if (ccuConn) { char rb[16]; snprintf(rb, sizeof(rb), "%lu s", (unsigned long)((millis() - lastCcuRxMs) / 1000));
                   row("Letztes CCU-RX vor", rb); }
    row("Geraete",     String(devCount));
    String devs; for (int i = 0; i < devCount && i < 32; i++) { char b[12]; snprintf(b, sizeof(b), "%08X ", devAddr[i]); devs += b; }
    row("Adressen",    devs.length() ? devs : "-");
    row("LAN-Frames",  String(lanFrames));
    row("Letztes Bus-Event", esc(String(lastEvent)));
    char ut[28]; snprintf(ut, sizeof(ut), "%lud %02lu:%02lu:%02lu",
                          (unsigned long)(up/86400),(unsigned long)((up/3600)%24),
                          (unsigned long)((up/60)%60),(unsigned long)(up%60));
    row("Uptime",      ut);
    row("Freier Heap", String(ESP.getFreeHeap()) + " B");
    row("Firmware",    F("v" FW_VERSION " &middot; " __DATE__ " " __TIME__));
    h += F("</table><div class=links><a href=/config>Konfiguration &auml;ndern</a> &middot; <a href=/log>System-Log</a> &middot; <a href=/update>Firmware-Update</a></div>");
    h += pageFoot();
    return h;
}

String updateHtml() {
    // Solange ein Check/Install laeuft (Flag gesetzt ODER Status "pruefe.."/"installiere.."),
    // automatisch nachladen -> das Ergebnis erscheint ohne manuelles F5.
    bool busy = g_doCheckUpd || g_doInstall
                || strncmp(g_updStatus, "pruefe", 6) == 0
                || strncmp(g_updStatus, "installiere", 11) == 0;
    String h = pageHead("Firmware-Update", busy ? 3 : 0);
    // --- Auto-Update ueber GitHub ---
    h += F("<h2>Auto-Update (GitHub)</h2><table>");
    h += "<tr><td>Installiert</td><td>v" FW_VERSION "</td></tr>";
    if (strlen(g_updLatest)) h += "<tr><td>Neuestes Release</td><td>v" + esc(g_updLatest) + "</td></tr>";
    h += "<tr><td>Status</td><td>" + esc(g_updStatus) + "</td></tr></table>";
    h += F("<form method=POST action=/checkupdate style='display:inline'>"
           "<button type=submit>Nach Update suchen</button></form>");
    if (g_updAvail)
        h += " <form method=POST action=/doupdate style='display:inline'>"
             "<button type=submit style='background:var(--ok)'>v" + esc(g_updLatest) + " installieren</button></form>";
    // --- manueller Upload (Fallback) ---
    h += F("<h2>Manuell (.bin hochladen)</h2>"
           "<form method=POST action=/update enctype='multipart/form-data'>"
           "<input type=file name=fw accept='.bin'>"
           "<button type=submit>Hochladen &amp; Flashen</button></form>"
           "<p style='color:var(--mut);font-size:.9em'>.bin aus dem GitHub-Release oder lokal via CI.</p>"
           "<div class=links><a href=/>&larr; Status</a></div>");
    h += pageFoot();
    return h;
}

void handleForm  (AsyncWebServerRequest* r) {
    if (authFail(r)) return;
    if (r->hasParam("debug")) { g_debugBus = (r->getParam("debug")->value().toInt() != 0); r->redirect("/config"); return; }
    r->send(200, "text/html", formHtml());
}
void handleStatus(AsyncWebServerRequest* r) { if (authFail(r)) return; r->send(200, "text/html", statusHtml()); }
void handleSave  (AsyncWebServerRequest* r) {
    if (authFail(r)) return;
    CFG.ssid = pval(r,"ssid"); CFG.pass = pval(r,"pass"); CFG.serial = pval(r,"serial");
    CFG.passphrase = pval(r,"pass2");
    CFG.useAes = r->hasParam("aes", true);
    int port = pval(r,"port").toInt(); CFG.port = port > 0 ? (uint16_t)port : 1000;
    CFG.useStaticIp = r->hasParam("staticip", true);
    CFG.ip = parseIp(pval(r,"ip")); CFG.gw = parseIp(pval(r,"gw")); CFG.sn = parseIp(pval(r,"sn"));
    CFG.useEth = r->hasParam("eth", true);
    auto clampP = [](long v, long lo, long hi){ return (int8_t)(v < lo ? lo : (v > hi ? hi : v)); };
    CFG.rs485Rx    = clampP(pval(r,"busrx").toInt(),  0, 39);
    CFG.rs485Tx    = clampP(pval(r,"bustx").toInt(),  0, 33);   // GPIO34-39 sind input-only
    CFG.rs485De    = clampP(pval(r,"busde").toInt(), -1, 33);
    CFG.rs485DeInv = r->hasParam("businv", true);
    auto clampU = [](long v, long lo, long hi){ return (uint16_t)(v < lo ? lo : (v > hi ? hi : v)); };
    CFG.rxTimeoutS  = clampU(pval(r,"rxto").toInt(),   20, 600);
    CFG.useKeepAlive= r->hasParam("ka", true);
    CFG.kaIdleS     = clampU(pval(r,"kaidle").toInt(),  5, 300);
    CFG.kaIntvlS    = clampU(pval(r,"kaintvl").toInt(), 1, 120);
    CFG.kaCount     = (uint8_t)clampU(pval(r,"kacnt").toInt(), 1, 20);
    CFG.ackWaitMs   = clampU(pval(r,"ackwait").toInt(), 50, 2000);
    CFG.webPass     = pval(r,"webpass");
    cfg::save(CFG);
    r->send(200, "text/html", pageHead("Gespeichert") +
            F("<meta http-equiv=refresh content='7;url=/'>"
              "<h2>Gespeichert</h2><p>Gateway startet neu &ndash; diese Seite l&auml;dt in ein paar Sekunden den Status.</p>")
            + pageFoot());
    rebootPending = true; rebootAt = millis() + 1200;
}

// System-Log-Seite: zeigt den RAM-Ring (Serial-Mirror). Fuer den Anlernmitschnitt:
// vor dem Anlernen ueber "Leeren" zuruecksetzen, dann anlernen, dann hier mitlesen.
void handleLog(AsyncWebServerRequest* r) {
    if (authFail(r)) return;
    if (r->hasParam("clear")) { Logger.clear(); r->redirect("/log"); return; }
    bool paused = r->hasParam("pause");
    String h = pageHead("System-Log", paused ? 0 : 3);   // Standard: alle 3 s automatisch nachladen
    h += F("<h2>System-Log</h2><div class=links>");
    h += paused ? F("<a href=/log>&#8635; Live (3 s)</a>") : F("<a href='/log?pause'>&#10073;&#10073; Pause</a>");
    h += F(" &middot; <a href=/log>Aktualisieren</a> &middot; <a href='/log?clear'>Leeren</a> &middot; <a href=/>&larr; Status</a></div>");
    h += F("<pre id=lg style='white-space:pre-wrap;word-break:break-all;font-size:.78em;background:#111;color:#3f3;"
           "padding:.7em;border-radius:8px;max-height:70vh;overflow:auto;margin-top:.8em'>");
    h += esc(Logger.snapshot());
    h += F("</pre><script>var e=document.getElementById('lg');e.scrollTop=e.scrollHeight;</script>");
    h += pageFoot();
    r->send(200, "text/html", h);
}

void startConfigPortal() {
    portalMode = true;
    WiFi.mode(WIFI_AP);
    char ap[24]; snprintf(ap, sizeof(ap), "HMW-GW-%04X", (uint16_t)(ESP.getEfuseMac() >> 32));
    WiFi.softAP(ap, AP_PASS);
    IPAddress apIp = WiFi.softAPIP();
    dns.start(53, "*", apIp);
    Serial.printf("# Config-Portal: WLAN '%s' / PW '%s' -> http://%s/\n", ap, AP_PASS, apIp.toString().c_str());
    webServer.on("/", HTTP_GET, handleForm);
    webServer.on("/save", HTTP_POST, handleSave);
    webServer.onNotFound([](AsyncWebServerRequest* r) { r->redirect("/"); });
    webServer.begin();
}

// ============================== Watchdog ==================================== //
void watchdogBegin() {
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3,0,0)
    esp_task_wdt_config_t c; c.timeout_ms = WDT_TIMEOUT_S * 1000; c.idle_core_mask = 0; c.trigger_panic = true;
    esp_task_wdt_reconfigure(&c);
#else
    esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
    esp_task_wdt_add(NULL);                 // loopTask ueberwachen
}
inline void watchdogFeed() { esp_task_wdt_reset(); }

// ============================== RS485-Bus =================================== //
inline void busTx(bool on) { if (CFG.rs485De >= 0) digitalWrite(CFG.rs485De, (on != CFG.rs485DeInv) ? HIGH : LOW); }
void busSend(const uint8_t* data, size_t len) {
    busTx(true);
    Serial2.write(data, len);
    Serial2.flush();                              // wartet (modern) auf TX-Done
    if (CFG.rs485De >= 0) delayMicroseconds(700); // letztes Byte sicher draussen, bevor DE auf RX faellt
    busTx(false);
}
size_t busRead(uint8_t* buf, size_t maxlen, uint32_t windowMs) {
    size_t n = 0; uint32_t last = millis();
    while (millis() - last < windowMs)
        while (Serial2.available() && n < maxlen) { buf[n++] = Serial2.read(); last = millis(); }
    return n;
}
// Wie busRead, aber wartet bis firstWaitMs auf das ERSTE Byte (Geraet hat Carrier-
// Sense-Backoff!), danach nur kurze Inter-Byte-Luecke. Verhindert, dass langsame
// Antworten das Unicast-Fenster verpassen und faelschlich als 'e' (statt 'r') gehen.
size_t busReadResponse(uint8_t* buf, size_t maxlen, uint32_t firstWaitMs, uint32_t gapMs) {
    size_t n = 0; uint32_t start = millis();
    while (n == 0 && millis() - start < firstWaitMs)
        while (Serial2.available() && n < maxlen) buf[n++] = Serial2.read();
    if (n == 0) return 0;
    uint32_t last = millis();
    while (millis() - last < gapMs)
        while (Serial2.available() && n < maxlen) { buf[n++] = Serial2.read(); last = millis(); }
    return n;
}
// len02-ACK an ein Geraet. Basis 0x19 + txSeqNum (Bits 6-5 des Geraete-Frames).
// Das echte LGW quittiert jede adressierte Geraete-Antwort auf Bus-Ebene -- ohne
// das sendet das Geraet 3x neu (ACKWAITTIME) und gibt auf. Aus hmw_bus._ack portiert.
void busAck(uint32_t dev, uint8_t devControl) {
    uint8_t ackCtrl = 0x19 | (devControl & 0x60);
    uint8_t out[16];
    size_t n = hmw::buildFrame(dev, ackCtrl, hmw::CENTRAL, nullptr, 0, out, true);
    dbgHex("BUS-TX(ack)", out, n);
    busSend(out, n);
}
bool busProbe(uint32_t prefix, uint8_t validBits) {
    uint8_t out[16];
    uint8_t ctrl = (uint8_t)((((validBits - 1) & 0x1F) << 3) | 0x03);
    size_t n = hmw::buildFrame(prefix, ctrl, 0, nullptr, 0, out, false);
    while (Serial2.available()) Serial2.read();
    busSend(out, n);
    uint8_t tmp[64];
    size_t got = busRead(tmp, sizeof(tmp), PROBE_WINDOW_MS);
    if (g_debugBus) {
        Serial.printf("# PROBE %08lX/%u  TX=%u RX=%u\n",
                      (unsigned long)prefix, validBits, (unsigned)n, (unsigned)got);
        if (got) dbgHex("  PROBE-RX", tmp, got);
    }
    return got > 0;
}
void busDiscover(uint32_t prefix, int fixed, uint32_t* found, int* nf) {
    if (fixed == 32) { if (*nf < 32) found[(*nf)++] = prefix; return; }
    for (int bit = 0; bit < 2; bit++) {
        uint32_t cand = prefix | ((uint32_t)bit << (31 - fixed));
        if (busProbe(cand, fixed + 1)) busDiscover(cand, fixed + 1, found, nf);
    }
}

// ============================== Bridge ====================================== //
void handleLan(WiFiClient& cli, lgw::Crypto* cr, uint8_t idx, const uint8_t* pl, uint8_t plen, uint32_t& lgwIdx) {
    if (plen == 0) return;
    if (g_debugBus)
        Serial.printf("# LAN cmd '%c'(%02X) plen=%u\n",
                      (pl[0] >= 32 && pl[0] < 127) ? pl[0] : '.', pl[0], plen);
    uint8_t lan[320];
    if (pl[0] == lgw::CMD_KEEPALIVE) {
        uint8_t p[2] = { lgw::CMD_ALIVE, 0x00 };
        sendLan(cli, cr, lan, lgw::lanEncode(idx, p, 2, lan)); return;
    }
    if (pl[0] == lgw::CMD_SEND) {
        uint8_t mode = pl[1];
        lastQueryAddr = ((uint32_t)pl[2]<<24)|((uint32_t)pl[3]<<16)|((uint32_t)pl[4]<<8)|pl[5];
        lastQueryMs   = millis();
        uint8_t bus[300]; size_t bl = lgw::embeddedToBus(pl + 2, plen - 2, bus);
        dbgHex("BUS-TX(send)", bus, bl);
        busSend(bus, bl);
        if (mode == lgw::MODE_UNICAST_ACK) {
            uint8_t rb[256]; size_t rn = busReadResponse(rb, sizeof(rb), CFG.ackWaitMs, 20);
            for (size_t i = 0; i < rn; i++) if (rb[i] == hmw::START || rb[i] == hmw::START_SHORT) {
                hmw::Frame f;
                bool isShort = (rb[i] == hmw::START_SHORT);
                bool ok = isShort ? hmw::parseSystemFrame(rb + i, rn - i, &f)
                                  : hmw::parseFrame(rb + i, rn - i, &f);
                if (ok) {
                    if (!isShort && f.hasSender && (f.control & 0x03) != 0x01) busAck(f.sender, f.control);
                    uint8_t rp[260]; uint8_t rpl = lgw::frameToResponse(f, rp);
                    sendLan(cli, cr, lan, lgw::lanEncode(idx, rp, rpl, lan));
                    if (g_debugBus)
                        Serial.printf("# resp 'r' via %s ctrl=%02X len=%u\n",
                                      isShort ? "system" : "unicast", f.control, f.dataLen);
                }
                break;
            }
        }
        return;
    }
    if (pl[0] == lgw::CMD_DISCOVERY) {
        uint32_t found[32]; int nf = 0;
        Serial.println("# DISCOVERY start");
        busDiscover(0, 0, found, &nf);
        Serial.printf("# DISCOVERY done, %d Geraete\n", nf);
        for (int i = 0; i < nf; i++) {
            devAddr[i] = found[i];
            uint8_t p[5] = { lgw::CMD_DEVFOUND, (uint8_t)(found[i]>>24),(uint8_t)(found[i]>>16),
                             (uint8_t)(found[i]>>8),(uint8_t)found[i] };
            sendLan(cli, cr, lan, lgw::lanEncode((uint8_t)(++lgwIdx), p, 5, lan));
        }
        devCount = nf;
        uint8_t dp[4] = { lgw::CMD_DISCDONE, 0, 0, 1 };
        sendLan(cli, cr, lan, lgw::lanEncode((uint8_t)(++lgwIdx), dp, 4, lan));
        return;
    }
}
void pollBusEvents(WiFiClient& cli, lgw::Crypto* cr, uint32_t& lgwIdx) {
    if (!Serial2.available()) return;
    uint8_t rb[256]; size_t rn = busRead(rb, sizeof(rb), 10);
    if (g_debugBus && rn) dbgHex("BUS-RX(evt)", rb, rn);
    for (size_t i = 0; i < rn; i++) if (rb[i] == hmw::START) {
        hmw::Frame f;
        if (hmw::parseFrame(rb + i, rn - i, &f) && f.target == hmw::CENTRAL && f.hasSender) {
            // De-Dup: Antwort des gerade unicast-abgefragten Geraets NICHT als spontanes
            // 'e' weiterreichen (kam schon als 'r'). Sonst deutet hs485d das Duplikat als
            // Re-Announce und verheddert sich in der Inklusion. Nur quittieren.
            bool dup = (f.sender == lastQueryAddr) && (millis() - lastQueryMs < 600);
            if (!dup) {
                snprintf(lastEvent, sizeof(lastEvent), "%08lX cmd=%02X",
                         (unsigned long)f.sender, f.dataLen ? f.data[0] : 0);
                uint8_t ep[290]; uint8_t epl = lgw::frameToReceived(f, ep);
                uint8_t lan[320]; sendLan(cli, cr, lan, lgw::lanEncode((uint8_t)(++lgwIdx), ep, epl, lan));
            }
            else if (g_debugBus) Serial.printf("# (dup von %08lX unterdrueckt)\n", (unsigned long)f.sender);
            if ((f.control & 0x03) != 0x01) busAck(f.sender, f.control);
        }
        break;
    }
}
void handleClient(WiFiClient& cli) {
    if (CFG.useKeepAlive) {                    // toten Peer auf TCP-Ebene erkennen
        int fd = cli.fd();
        if (fd >= 0) {
            int on = 1, idle = CFG.kaIdleS, intvl = CFG.kaIntvlS, cnt = CFG.kaCount;
            setsockopt(fd, SOL_SOCKET,  SO_KEEPALIVE,  &on,    sizeof(on));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof(idle));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt));
        }
    }
    char buf[80];
    int gl = snprintf(buf, sizeof(buf), "H42,01,eQ3-HMW-LGW,1.0.5,%s\r\n", CFG.serial.c_str());
    cli.write((const uint8_t*)buf, gl);

    lgw::Crypto crypto;
    lgw::Crypto* cr = nullptr;
    if (CFG.useAes) {
        uint8_t ownIv[16]; for (int i = 0; i < 16; i++) ownIv[i] = (uint8_t)esp_random();
        int il = snprintf(buf, sizeof(buf), "V43,");
        for (int i = 0; i < 16; i++) il += snprintf(buf + il, 4, "%02x", ownIv[i]);
        buf[il++] = '\r'; buf[il++] = '\n';
        cli.write((const uint8_t*)buf, il);
        String line = cli.readStringUntil('\n');
        int comma = line.indexOf(','); if (comma < 0) return;
        uint8_t peerIv[16];
        for (int i = 0; i < 16; i++)
            peerIv[i] = (uint8_t)strtol(line.substring(comma + 1 + i*2, comma + 3 + i*2).c_str(), nullptr, 16);
        crypto.begin(aesKey, ownIv, peerIv); cr = &crypto;
        const char* s = "S44\r\n"; uint8_t tmp[16]; cr->encrypt((const uint8_t*)s, tmp, 5); cli.write(tmp, 5);
        uint32_t t0 = millis();
        while (millis() - t0 < 1000) { if (cli.available()) { uint8_t b = cli.read(), d; cr->decrypt(&b, &d, 1); if (d == '\n') break; } }
    } else {
        // AES AUS (Klartext) -- noch nicht gegen echte AES-off-Session verifiziert
        cli.write((const uint8_t*)"S44\r\n", 5);
        String r = cli.readStringUntil('\n');
        Serial.printf("# (AES off) CCU-Antwort nach H/S: '%s'\n", r.c_str());
    }
    Serial.println("# Handshake ok");
    ccuConn = true;

    uint8_t rx[640]; size_t rxlen = 0; uint32_t lgwIdx = 0x70;
    uint32_t lastRx = millis(); lastCcuRxMs = lastRx;
    while (cli.connected()) {
        watchdogFeed(); ArduinoOTA.handle();
        // "Last connect wins": Will die CCU neu verbinden (hs485d-Neustart, Netzwechsel
        // oder lautlos weggebrochene Verbindung), wartet ihr SYN am Listen-Socket.
        // hasClient() akzeptiert es non-destruktiv (cached es fuer das naechste accept()
        // im loop) -> die alte, hoechstwahrscheinlich tote Verbindung sofort aufgeben,
        // statt bis zum rxTimeout am Zombie-Socket zu haengen. Loest das bisherige
        // "nach CCU-Wechsel Gateway neu starten"-Problem.
        if (lgwServer.hasClient()) {
            Serial.println("# Neue CCU-Verbindung wartet -> alte ersetzen (last connect wins)");
            break;
        }
        if (g_doCheckUpd || g_doInstall) {   // Update-Aktion -> Sitzung verlassen, loop fuehrt sie aus
            Serial.println("# Update angefordert -> CCU-Sitzung verlassen");
            break;
        }
        while (cli.available()) {
            uint8_t enc[256]; int got = cli.read(enc, sizeof(enc));
            if (got > 0) { lastRx = millis(); lastCcuRxMs = lastRx; }
            if (got > 0 && rxlen + got < sizeof(rx)) {
                if (cr) cr->decrypt(enc, rx + rxlen, got); else memcpy(rx + rxlen, enc, got);
                rxlen += got;
            }
        }
        size_t off = 0;
        while (off < rxlen && rx[off] == hmw::START) {
            uint8_t idx, pl[300], plen;
            size_t used = lgw::lanExtract(rx + off, rxlen - off, &idx, pl, &plen);
            if (!used) break;
            handleLan(cli, cr, idx, pl, plen, lgwIdx); lanFrames++;
            off += used;
        }
        if (off) { memmove(rx, rx + off, rxlen - off); rxlen -= off; }
        pollBusEvents(cli, cr, lgwIdx);
        // Keine Keepalives mehr (CCU schickt 'K' alle 20s) -> Verbindung tot -> sauber schliessen,
        // damit die CCU das Close sieht und neu verbindet (statt am Zombie-Socket zu haengen).
        if (millis() - lastRx > (uint32_t)CFG.rxTimeoutS * 1000UL) {
            Serial.println("# CCU inaktiv -> trennen (Reconnect ermoeglichen)"); break;
        }
        delay(2);
    }
    ccuConn = false;
}

// ============================== Netzwerk ==================================== //
// CFG.useEth: LAN8720-Ethernet (ESP32-ETH01) statt WLAN. Darunter dieselben
// lwIP-Sockets -- LGW-Server, Web, OTA und Keepalive laufen unveraendert.
IPAddress netIp() { return CFG.useEth ? ETH.localIP() : WiFi.localIP(); }
bool      netUp() { return CFG.useEth ? ETH.linkUp() : (WiFi.status() == WL_CONNECTED); }

bool netStart() {                       // true = IP bezogen
    if (CFG.useEth) {
        WiFi.mode(WIFI_OFF);            // Funk aus im Kabelbetrieb
        WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t) {
            ETH.setHostname(CFG.serial.c_str());   // ETH_START feuert aus begin() -> vor DHCP
        }, ARDUINO_EVENT_ETH_START);
        bool ok =
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3,0,0)
            ETH.begin(ETH01_PHY_TYPE, ETH01_PHY_ADDR, ETH01_PIN_MDC, ETH01_PIN_MDIO, ETH01_PIN_POWER, ETH01_CLK_MODE);
#else       // core 2.x: gleiche Parameter, andere Reihenfolge
            ETH.begin(ETH01_PHY_ADDR, ETH01_PIN_POWER, ETH01_PIN_MDC, ETH01_PIN_MDIO, ETH01_PHY_TYPE, ETH01_CLK_MODE);
#endif
        if (!ok) { Serial.println("# ETH.begin() fehlgeschlagen (PHY nicht gefunden?)"); return false; }
        if (CFG.useStaticIp)
            ETH.config(IPAddress(CFG.ip), IPAddress(CFG.gw), IPAddress(CFG.sn), IPAddress(CFG.gw));
        Serial.print("# Ethernet");
        uint32_t t0 = millis();
        while ((uint32_t)ETH.localIP() == 0 && millis() - t0 < 20000) { delay(300); Serial.print("."); }
        Serial.println();
        return (uint32_t)ETH.localIP() != 0;
    }
    WiFi.persistent(false);
    WiFi.setHostname(CFG.serial.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(CFG.serial.c_str());
    WiFi.setSleep(false);              // WLAN-Power-Save AUS: Modem-Sleep verzoegert/verwirft Pakete und
                                       // laesst die TCP-Dauerverbindung zur CCU still abbrechen. Pflicht fuer LGW-Betrieb.
    WiFi.setAutoReconnect(true);
    if (CFG.useStaticIp)
        WiFi.config(IPAddress(CFG.ip), IPAddress(CFG.gw), IPAddress(CFG.sn), IPAddress(CFG.gw));
    WiFi.begin(CFG.ssid.c_str(), CFG.pass.c_str());
    Serial.print("# WiFi");
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { delay(300); Serial.print("."); }
    Serial.println();
    return WiFi.status() == WL_CONNECTED;
}

// ============================== Auto-Update ================================= //
// Fragt das neueste GitHub-Release ab und vergleicht dessen Version mit FW_VERSION.
// BLOCKIEREND (HTTPS, ~2 s) -> nur aus dem loop aufrufen, nie aus dem Async-Web-Callback.
bool updateCheck() {
    if (!netUp()) { setUpdStatus("kein Netz"); return false; }
    WiFiClientSecure client; client.setInsecure();   // GitHub, kein Cert-Check; .bin-Integritaet prueft Update.h
    HTTPClient http;
    String url = "https://api.github.com/repos/" GH_OWNER "/" GH_REPO "/releases/latest";
    if (!http.begin(client, url)) { setUpdStatus("begin fehlgeschlagen"); return false; }
    http.addHeader("User-Agent", GH_REPO);            // GitHub-API verlangt einen User-Agent
    http.addHeader("Accept", "application/vnd.github+json");
    int code = http.GET();
    if (code != 200) { setUpdStatus("GitHub HTTP %d", code); http.end(); return false; }
    String body = http.getString();
    http.end();
    int t = body.indexOf("\"tag_name\"");
    if (t < 0) { setUpdStatus("kein Release gefunden"); return false; }
    int q1 = body.indexOf('"', body.indexOf(':', t) + 1);
    int q2 = body.indexOf('"', q1 + 1);
    if (q1 < 0 || q2 <= q1) { setUpdStatus("Parse-Fehler"); return false; }
    String tag = body.substring(q1 + 1, q2);          // z.B. "v1.1.1"
    String ver = tag.startsWith("v") ? tag.substring(1) : tag;
    snprintf(g_updLatest, sizeof(g_updLatest), "%s", ver.c_str());
    g_updAvail = (strlen(g_updLatest) > 0) && (strcmp(g_updLatest, FW_VERSION) != 0);
    if (g_updAvail) setUpdStatus("Update verfuegbar: v%s", g_updLatest);
    else            setUpdStatus("aktuell (v" FW_VERSION ")");
    Serial.printf("# Update-Check: installiert v%s, neuestes v%s -> %s\n",
                  FW_VERSION, g_updLatest, g_updAvail ? "UPDATE" : "aktuell");
    return true;
}

// Laedt die .bin des neuesten Release und flasht sie. BLOCKIEREND (Minuten) -> nur aus
// dem loop. Bei Erfolg kontrollierter Reboot in die neue Firmware.
void updateInstall() {
    if (strlen(g_updLatest) == 0) { setUpdStatus("erst pruefen"); return; }
    String url = "https://github.com/" GH_OWNER "/" GH_REPO "/releases/download/v"
                 + String(g_updLatest) + "/hmw-gateway-pro-" + String(g_updLatest) + ".bin";
    Serial.printf("# Auto-Update: lade %s\n", url.c_str());
    setUpdStatus("installiere v%s ...", g_updLatest);
    WiFiClientSecure client; client.setInsecure();
    httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);   // GitHub -> objects.githubusercontent.com
    httpUpdate.rebootOnUpdate(false);                             // wir rebooten selbst
    esp_task_wdt_delete(NULL);                                   // Download+Flash dauert Minuten -> loopTask solange nicht ueberwachen
    t_httpUpdate_return ret = httpUpdate.update(client, url);
    if (ret == HTTP_UPDATE_OK) {
        Serial.println("# Auto-Update OK -> Reboot");
        setUpdStatus("OK - Neustart ...");
        rebootPending = true; rebootAt = millis() + 800;
    } else {
        esp_task_wdt_add(NULL);                                  // Update fehlgeschlagen -> WDT wieder aktiv
        setUpdStatus("Fehler: %s", httpUpdate.getLastErrorString().c_str());
        Serial.printf("# Auto-Update Fehler (%d): %s\n", ret, httpUpdate.getLastErrorString().c_str());
    }
}

// ============================== Gateway-Setup =============================== //
void runGateway() {
    if (CFG.rs485De >= 0) { pinMode(CFG.rs485De, OUTPUT); busTx(false); }   // Idle = Empfangen (respektiert Invert)
    Serial2.begin(BUS_BAUD, SERIAL_8E1, CFG.rs485Rx, CFG.rs485Tx);   // HMW-Bus = 8E1 (even parity)! Strenge HW-UARTs (z.B. ATmega32A) verwerfen sonst jeden Frame
    lgw::lanKey(CFG.passphrase.c_str(), aesKey);

    if (!netStart()) {
        Serial.println(CFG.useEth ? "# Ethernet fehlgeschlagen -> Portal" : "# WLAN fehlgeschlagen -> Portal");
        startConfigPortal(); return;
    }
    Serial.printf("# Gateway bereit: %s  Host: %s  Port %u  AES=%d  Netz=%s\n",
                  netIp().toString().c_str(), CFG.serial.c_str(), CFG.port, CFG.useAes,
                  CFG.useEth ? "Ethernet" : "WLAN");

    ArduinoOTA.setHostname(CFG.serial.c_str());
    ArduinoOTA.setPassword(AP_PASS);
    ArduinoOTA.begin();
    webServer.on("/", HTTP_GET, handleStatus);
    webServer.on("/config", HTTP_GET, handleForm);
    webServer.on("/save", HTTP_POST, handleSave);
    webServer.on("/log", HTTP_GET, handleLog);
    webServer.on("/update", HTTP_GET, [](AsyncWebServerRequest* r) {
        if (authFail(r)) return; r->send(200, "text/html", updateHtml()); });
    webServer.on("/update", HTTP_POST,
        [](AsyncWebServerRequest* r) {
            if (authFail(r)) return;
            bool ok = !Update.hasError();
            String body = pageHead(ok ? "Update ok" : "Update fehlgeschlagen") +
                (ok ? F("<meta http-equiv=refresh content='8;url=/'><h2>Update ok</h2>"
                        "<p>Gateway startet neu &ndash; Status l&auml;dt gleich.</p>")
                    : F("<h2>Update fehlgeschlagen</h2><p><a href=/update>Nochmal versuchen</a></p>")) + pageFoot();
            AsyncWebServerResponse* resp = r->beginResponse(200, "text/html", body);
            resp->addHeader("Connection", "close");
            r->send(resp);
            if (ok) { rebootPending = true; rebootAt = millis() + 1500; }
        },
        [](AsyncWebServerRequest* r, String fn, size_t idx, uint8_t* data, size_t len, bool fin) {
            if (!webAuthed(r)) return;
            if (idx == 0) { Serial.printf("# Web-OTA: %s\n", fn.c_str()); Update.begin(UPDATE_SIZE_UNKNOWN); }
            if (len) Update.write(data, len);
            if (fin) { if (Update.end(true)) Serial.println("# Web-OTA ok"); else Update.printError(Serial); }
        });
    // Auto-Update: Handler setzen nur Flags, die Ausfuehrung (blockierend) macht der loop.
    webServer.on("/checkupdate", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (authFail(r)) return;
        setUpdStatus("pruefe ..."); g_doCheckUpd = true;
        r->redirect("/update");
    });
    webServer.on("/doupdate", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (authFail(r)) return;
        if (g_updAvail) g_doInstall = true;
        r->redirect("/update");
    });
    webServer.begin();
    watchdogBegin();
    Serial.println("# OTA + Status-Web (Port 80) + Watchdog aktiv");
    lgwServer.begin(CFG.port);
}

void setup() {
    Serial.begin(115200);
    bootMs = millis();
    Serial.printf("# HMW-LGW Firmware v%s (Build %s %s)\n", FW_VERSION, __DATE__, __TIME__);
    cfg::load(CFG);
    Serial.printf("# Config: valid=%d ssid=%s serial=%s port=%u aes=%d eth=%d bus=RX%d/TX%d/DE%d\n",
                  CFG.valid, CFG.ssid.c_str(), CFG.serial.c_str(), CFG.port, CFG.useAes,
                  CFG.useEth, CFG.rs485Rx, CFG.rs485Tx, CFG.rs485De);
    if (!CFG.valid || wantConfigPortal()) startConfigPortal();
    else                                  runGateway();
}

void loop() {
    if (rebootPending && (int32_t)(millis() - rebootAt) >= 0) { delay(100); ESP.restart(); }
    if (portalMode) { dns.processNextRequest(); return; }
    watchdogFeed();
    static bool netDown = false;
    if (!netUp()) {                          // WLAN weg -> reconnect; ETH-Link weg -> auf Kabel warten
        netDown = true;
        if (!CFG.useEth) WiFi.reconnect();
        delay(500); return;
    }
    if (netDown) {                           // Netz war weg, ist wieder da -> Server neu aufsetzen
        netDown = false;
        lgwServer.end(); lgwServer.begin(CFG.port);
        Serial.println("# Netzwerk zurueck -> LGW-Server neu gestartet");
    }
    ArduinoOTA.handle();
    if (g_doCheckUpd) { g_doCheckUpd = false; updateCheck(); }   // blockierend, aber nur zwischen CCU-Sitzungen
    if (g_doInstall)  { g_doInstall  = false; updateInstall(); }
    WiFiClient cli = lgwServer.accept();   // accept() = ehem. available(); holt auch den in handleClient via hasClient() vorgemerkten Neu-Client
    if (cli) {
        Serial.println("# CCU verbunden");
        handleClient(cli);
        cli.stop();
        Serial.println("# CCU getrennt");
    }
}
