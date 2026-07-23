// frame_tap.h -- strukturierter Ringpuffer fuer decodierte Bus-Telegramme.
// Gemeinsamer Unterbau fuer Live-Sniffer (/sniffer), spaeter Aufzeichnung/Download
// und Timing-/Fehleranalyse. Jedes decodierte Frame (TX = an den Bus, RX = vom Bus)
// wird mit Zeitstempel + Richtung + Adressen + Nutzdaten abgelegt; zusaetzlich laufen
// ein paar Zaehler (RX/TX/CRC-Fehler) fuer die spaetere Statistik-Seite.
//
// Ein Schreiber (loopTask ueber die Tap-Aufrufe in der Gateway-Schleife), ein Leser
// (Async-Web-Task ueber copyOut()). Der Ring ist mutex-geschuetzt wie der LogTee: der
// Schreiber verwirft im seltenen Contention-Fall lieber einen Eintrag, als den Bus-
// Loop zu bremsen (Diagnose, kein kritischer Pfad). Die Zaehler sind einfache
// 32-bit-Zaehler (auf dem ESP32 atomar) und laufen auch ohne Mutex.
#pragma once
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

class FrameTap {
public:
    static const size_t RING = 100;   // Ring-Eintraege (aeltere fallen hinten raus)
    static const size_t DATA = 48;    // gespeicherte Datenbytes/Frame (Rest wird abgeschnitten)

    // Richtung/Art des Telegramms. SEND/ACK = wir senden auf den Bus (TX),
    // EVENT/RESP = vom Bus empfangen (RX), BADCRC = empfangenes Frame mit CRC-Fehler.
    enum Kind : uint8_t { SEND = 0, EVENT, RESP, ACK, BADCRC };

    struct Entry {
        uint32_t ms;         // millis() beim Erfassen
        uint32_t target;     // Zieladresse
        uint32_t sender;     // Quelladresse (nur gueltig bei hasSender)
        uint8_t  control;    // Control-Byte
        uint8_t  dataLen;    // echte Nutzdatenlaenge des Frames
        uint8_t  stored;     // wie viele Datenbytes hier liegen (<= DATA)
        uint8_t  kind;       // Kind
        bool     hasSender;  // Quelladresse vorhanden?
        bool     crcOk;      // CRC gueltig? (BADCRC = false)
        uint8_t  data[DATA];
    };

    // Muss vor dem ersten add() laufen (aus setup(), Scheduler steht): legt den Mutex an.
    void begin() { if (!_mtx) _mtx = xSemaphoreCreateMutex(); }

    // Ein decodiertes Frame ablegen. data/dataLen duerfen leer sein (z.B. ACK).
    void add(uint8_t kind, uint32_t target, uint8_t control, bool hasSender,
             uint32_t sender, const uint8_t* data, uint8_t dataLen, bool crcOk) {
        if (kind == EVENT || kind == RESP)      _rx++;      // Zaehler lockfrei fuehren --
        else if (kind == SEND || kind == ACK)   _tx++;      // unabhaengig davon, ob der
        if (kind == BADCRC || !crcOk)           _crcErr++;  // Ring-Eintrag gespeichert wird.
        if (!_mtx) return;
        if (xSemaphoreTake(_mtx, pdMS_TO_TICKS(10)) != pdTRUE) { _dropped++; return; }
        Entry& e = _buf[_head];
        e.ms = millis(); e.target = target; e.sender = sender; e.control = control;
        e.dataLen = dataLen; e.kind = kind; e.hasSender = hasSender; e.crcOk = crcOk;
        e.stored = dataLen > DATA ? (uint8_t)DATA : dataLen;
        if (data && e.stored) memcpy(e.data, data, e.stored);
        _head = (_head + 1) % RING;
        if (_head == 0) _full = true;
        xSemaphoreGive(_mtx);
    }

    // Empfangenes Frame mit CRC-Fehler: roh (undecodiert) ablegen, damit die Stoerung
    // im Sniffer sichtbar wird. Adressfelder bleiben 0 (nicht vertrauenswuerdig).
    void badCrc(const uint8_t* raw, size_t n) {
        add(BADCRC, 0, 0, false, 0, raw, n > 255 ? 255 : (uint8_t)n, false);
    }

    // Kopiert die neuesten min(avail, maxN) Eintraege in chronologischer Reihenfolge
    // (aeltestes zuerst) nach dst. Der Mutex wird nur fuer die Kopie gehalten; das
    // Formatieren macht der Aufrufer danach lockfrei. Rueckgabe: Anzahl kopierter Eintraege.
    size_t copyOut(Entry* dst, size_t maxN) {
        if (!_mtx || xSemaphoreTake(_mtx, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
        size_t avail = _full ? RING : _head;
        size_t start = _full ? _head : 0;
        size_t n = avail < maxN ? avail : maxN;
        size_t skip = avail - n;                 // aelteste ueberspringen, nur die neuesten n
        for (size_t i = 0; i < n; i++) dst[i] = _buf[(start + skip + i) % RING];
        xSemaphoreGive(_mtx);
        return n;
    }

    // Ring UND Zaehler zuruecksetzen -- fuer einen frischen Mitschnitt (z.B. vor dem Anlernen).
    void clear() {
        if (_mtx && xSemaphoreTake(_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
            _head = 0; _full = false;
            xSemaphoreGive(_mtx);
        }
        _rx = _tx = _crcErr = _dropped = 0;
    }

    uint32_t rx()      const { return _rx; }
    uint32_t tx()      const { return _tx; }
    uint32_t crcErr()  const { return _crcErr; }
    uint32_t dropped() const { return _dropped; }

private:
    Entry  _buf[RING];
    size_t _head = 0;
    bool   _full = false;
    volatile uint32_t _rx = 0, _tx = 0, _crcErr = 0, _dropped = 0;
    SemaphoreHandle_t _mtx = nullptr;
};
