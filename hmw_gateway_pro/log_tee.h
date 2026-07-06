// log_tee.h -- spiegelt jede serielle Ausgabe zusaetzlich in einen RAM-Ringpuffer,
// der ueber die Weboberflaeche (/log) abrufbar ist. So laesst sich ein
// CCU<->Gateway-Anlernmitschnitt (LAN-Frames + Bus-TX/RX) ohne USB-Kabel direkt
// am Einbauort mitlesen. Muster aus HB-RF-ETH-ng (log_manager, 8-KB-Ring) auf
// Arduino uebertragen.
//
// Verwendung (im Sketch NACH allen System-Includes):
//     #include "log_tee.h"
//     LogTee Logger;
//     #define Serial Logger      // ab hier landet jedes Serial.print* zusaetzlich im Ring
// Der echte UART0 bleibt Mirror -- der Text erscheint weiterhin auch am USB-Monitor.
// (Serial2 = RS485-Bus ist ein eigenes Token und bleibt vom #define unberuehrt.)
#pragma once
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

class LogTee : public Print {
public:
    static const size_t RING = 8192;   // Ringgroesse in Byte (wie HB-RF-ETH-ng)

    // Muss vor der ersten Log-Ausgabe laufen (aus setup(), wo der Scheduler steht):
    // legt den Mutex an und startet den echten UART0.
    void begin(unsigned long baud) {
        Serial.begin(baud);
        if (!_mtx) _mtx = xSemaphoreCreateMutex();
    }

    size_t write(uint8_t c) override      { Serial.write(c);    push(&c, 1); return 1; }
    size_t write(const uint8_t* b, size_t n) override { Serial.write(b, n); push(b, n); return n; }

    // Ringinhalt in chronologischer Reihenfolge (aeltestes zuerst). Der Mutex wird
    // nur fuer den schnellen memcpy gehalten; der String wird danach lockfrei gebaut,
    // damit der loop-Task beim Loggen nicht auf den Web-Task wartet. HTML-Escaping
    // erledigt der Aufrufer.
    String snapshot() {
        if (!_mtx || xSemaphoreTake(_mtx, pdMS_TO_TICKS(100)) != pdTRUE) return String();
        size_t avail = _full ? RING : _head;
        size_t start = _full ? _head : 0;
        char* tmp = (char*)malloc(avail + 1);
        if (tmp) {
            for (size_t i = 0; i < avail; i++) tmp[i] = (char)_buf[(start + i) % RING];
            tmp[avail] = 0;
        }
        xSemaphoreGive(_mtx);
        String out(tmp ? tmp : "");
        free(tmp);
        return out;
    }

    void clear() {
        if (_mtx && xSemaphoreTake(_mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
            _head = 0; _full = false;
            xSemaphoreGive(_mtx);
        }
    }

private:
    // Nicht-blockierend fuer den Schreiber: bekommt er den Mutex nicht schnell (Web-Task
    // liest gerade), werden die paar Bytes verworfen statt den loop zu bremsen -- das Log
    // ist Diagnose, kein kritischer Pfad.
    void push(const uint8_t* b, size_t n) {
        if (!_mtx) return;                                  // vor begin(): nur Mirror
        if (xSemaphoreTake(_mtx, pdMS_TO_TICKS(20)) != pdTRUE) return;
        for (size_t i = 0; i < n; i++) {
            _buf[_head++] = b[i];
            if (_head >= RING) { _head = 0; _full = true; }
        }
        xSemaphoreGive(_mtx);
    }

    uint8_t _buf[RING];
    volatile size_t _head = 0;
    volatile bool   _full = false;
    SemaphoreHandle_t _mtx = nullptr;
};
