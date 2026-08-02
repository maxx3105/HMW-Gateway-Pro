// capture_log.h -- RAM-Puffer fuer eine Telegramm-Aufzeichnung (Start/Stopp/Download)
// plus der gemeinsame Zeilen-Formatierer fuer Recorder UND Schnell-Download.
//
// Anders als der Sniffer-Ring (frame_tap.h, immer die letzten N) fuellt sich die
// Aufzeichnung LINEAR von vorne und stoppt, wenn der Puffer voll ist -- so bleibt der
// ANFANG eines Mitschnitts (z.B. der Anlern-Announce) garantiert erhalten. Der Puffer
// wird erst bei start() per malloc angelegt, im Ruhezustand kostet er also kein RAM.
//
// Mutex-geschuetzt: Schreiber ist der loopTask (ueber den Frame-Tap-Sink), Leser der
// Async-Web-Task (Download/Status). Ein RAM-Puffer laesst sich -- anders als ein
// LittleFS-File-Handle -- so gefahrlos zwischen beiden Tasks teilen.
#pragma once
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "frame_tap.h"

// Kurzbezeichnung der Telegramm-Art als reiner Text (ohne HTML) fuer Datei/Download.
static inline const char* captureKindText(uint8_t k) {
    switch (k) {
        case FrameTap::SEND:   return "SEND";
        case FrameTap::EVENT:  return "EVT";
        case FrameTap::RESP:   return "RSP";
        case FrameTap::ACK:    return "ACK";
        case FrameTap::BADCRC: return "CRC";
        default:               return "?";
    }
}

// Formatiert einen Tap-Eintrag als tab-getrennte Textzeile (mit abschliessendem \n):
//   t(s)  typ  ziel  quelle  ctrl  len  daten(hex)
// Adress-/Ctrl-Felder bei CRC-Fehlern und fehlendem Sender als "-". Ist der Frame
// laenger als gespeichert (stored < dataLen), markiert ein '~' die Kuerzung.
// Rueckgabe: geschriebene Bytes (immer < cap, nie ueber den Puffer hinaus).
static inline size_t captureEntryLine(const FrameTap::Entry& e, char* out, size_t cap) {
    if (cap == 0) return 0;
    bool bad = (e.kind == FrameTap::BADCRC);
    char tgt[12], snd[12], ctl[4];
    if (bad) { strcpy(tgt, "-"); strcpy(snd, "-"); strcpy(ctl, "-"); }
    else {
        snprintf(tgt, sizeof(tgt), "%08lX", (unsigned long)e.target);
        if (e.hasSender) snprintf(snd, sizeof(snd), "%08lX", (unsigned long)e.sender);
        else             strcpy(snd, "-");
        snprintf(ctl, sizeof(ctl), "%02X", e.control);
    }
    int r = snprintf(out, cap, "%lu.%03lu\t%s\t%s\t%s\t%s\t%u\t",
                     (unsigned long)(e.ms / 1000), (unsigned long)(e.ms % 1000),
                     captureKindText(e.kind), tgt, snd, ctl, e.dataLen);
    size_t o = (r < 0) ? 0 : ((size_t)r >= cap ? cap - 1 : (size_t)r);
    for (uint8_t k = 0; k < e.stored && o + 3 < cap; k++)
        o += (size_t)snprintf(out + o, cap - o, "%02X ", e.data[k]);
    if (e.stored < e.dataLen && o + 1 < cap) out[o++] = '~';
    if (o + 1 < cap) out[o++] = '\n';
    out[o < cap ? o : cap - 1] = '\0';
    return o;
}

class CaptureLog {
public:
    void begin() { if (!_mtx) _mtx = xSemaphoreCreateMutex(); }

    // Neue Aufzeichnung: alten Puffer freigeben, neuen der Groesse cap anlegen.
    // false, wenn kein RAM verfuegbar. Bei Erfolg laeuft die Aufzeichnung.
    bool start(size_t cap) {
        if (!_mtx) return false;
        xSemaphoreTake(_mtx, portMAX_DELAY);
        free(_buf);
        _buf = (char*)malloc(cap);
        _cap = _buf ? cap : 0; _len = 0; _full = false;
        _recording = (_buf != nullptr);
        bool ok = _recording;
        xSemaphoreGive(_mtx);
        return ok;
    }
    void stop() { _recording = false; }   // Puffer bleibt zum Download erhalten

    // Textzeile anhaengen (aus dem Frame-Tap-Sink, loopTask). Passt der Rest nicht mehr,
    // wird abgeschnitten und die Aufzeichnung automatisch gestoppt (Anfang bleibt heil).
    void append(const char* s, size_t n) {
        if (!_recording || !_mtx) return;
        if (xSemaphoreTake(_mtx, pdMS_TO_TICKS(10)) != pdTRUE) return;
        if (_buf && _recording) {
            if (_len + n > _cap) { n = _cap - _len; _full = true; _recording = false; }
            if (n) { memcpy(_buf + _len, s, n); _len += n; }
        }
        xSemaphoreGive(_mtx);
    }

    // Inhalt als String (fuer den Download). Kopie unter Mutex, danach lockfrei.
    String snapshot() {
        if (!_mtx || xSemaphoreTake(_mtx, pdMS_TO_TICKS(100)) != pdTRUE) return String();
        String out;
        if (_buf && _len) { out.reserve(_len); out.concat(_buf, _len); }
        xSemaphoreGive(_mtx);
        return out;
    }

    // Puffer freigeben, Aufzeichnung aus (RAM zurueckgeben).
    void discard() {
        if (_mtx && xSemaphoreTake(_mtx, portMAX_DELAY) == pdTRUE) {
            _recording = false;
            free(_buf); _buf = nullptr; _cap = _len = 0; _full = false;
            xSemaphoreGive(_mtx);
        }
    }

    bool   recording() const { return _recording; }
    bool   full()      const { return _full; }
    size_t len()       const { return _len; }
    size_t cap()       const { return _cap; }
    bool   hasData()   const { return _buf != nullptr && _len > 0; }

private:
    char*  _buf = nullptr;
    size_t _cap = 0, _len = 0;
    volatile bool _recording = false;
    bool   _full = false;
    SemaphoreHandle_t _mtx = nullptr;
};
