// hmw_protocol.h -- HomeMatic-Wired Bus-Codec (CRC/Escaping/Frames) in C++.
// Portierung des verifizierten Python-hmw_protocol.py. Header-only, laeuft
// auf Host (g++) und ESP32/Arduino. Verifiziert gegen echte Hardware-Frames.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace hmw {

static const uint8_t  START     = 0xFD;
static const uint8_t  START_SHORT = 0xFE;   // system/short-Frame (Booter-Antworten w/p/r/g)
static const uint8_t  ESC       = 0xFC;
static const uint16_t CRC_POLY  = 0x1002;
static const uint16_t CRC_INIT  = 0xF1E2;   // == HBWired init 0xFFFF + 2 Null-Byte-Augmentation
static const uint32_t CENTRAL   = 0x00000001;
static const uint32_t BROADCAST = 0xFFFFFFFF;

// CRC16, MSB-first, Polynom 0x1002, ueber den UNESCAPETEN Frame inkl. Start-FD.
inline uint16_t crc16(const uint8_t* data, size_t len, uint16_t crc = CRC_INIT) {
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ CRC_POLY)
                                 : (uint16_t)(crc << 1);
    }
    return crc;
}

// Escaping (nach dem Start-FD): FC/FD/FE -> FC, (byte & 0x7F). Gibt Ausgabelaenge.
inline size_t escape(const uint8_t* in, size_t len, uint8_t* out) {
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = in[i];
        if (b == 0xFC || b == 0xFD || b == 0xFE) { out[o++] = ESC; out[o++] = b & 0x7F; }
        else                                     { out[o++] = b; }
    }
    return o;
}

inline size_t unescape(const uint8_t* in, size_t len, uint8_t* out) {
    size_t o = 0, i = 0;
    while (i < len) {
        if (in[i] == ESC && i + 1 < len) { out[o++] = in[i + 1] | 0x80; i += 2; }
        else                             { out[o++] = in[i];            i += 1; }
    }
    return o;
}

// Bit 3 = HAS_SENDER, ausser Discovery-Frames (Bits 1,0 == 11).
inline bool hasSenderFlag(uint8_t control) {
    return ((control & 0x08) != 0) && ((control & 0x03) != 0x03);
}

// Baut ein Bus-Frame (escaped, mit CRC). Rueckgabe: Wire-Laenge in out.
inline size_t buildFrame(uint32_t target, uint8_t control, uint32_t sender,
                         const uint8_t* data, uint8_t dataLen,
                         uint8_t* out, bool hasSender = true) {
    uint8_t body[300];
    size_t n = 0;
    body[n++] = START;
    body[n++] = target >> 24; body[n++] = target >> 16; body[n++] = target >> 8; body[n++] = (uint8_t)target;
    body[n++] = control;
    if (hasSender) {
        body[n++] = sender >> 24; body[n++] = sender >> 16; body[n++] = sender >> 8; body[n++] = (uint8_t)sender;
    }
    body[n++] = (uint8_t)(dataLen + 2);          // len = data + CRC
    memcpy(body + n, data, dataLen); n += dataLen;
    uint16_t crc = crc16(body, n);
    body[n++] = (uint8_t)(crc >> 8); body[n++] = (uint8_t)(crc & 0xFF);
    out[0] = START;                              // Start-FD nicht escapen
    return 1 + escape(body + 1, n - 1, out + 1);
}

struct Frame {
    uint32_t target;
    uint8_t  control;
    bool     hasSender;
    uint32_t sender;
    uint8_t  data[260];
    uint8_t  dataLen;
};

// Parst ein rohes (escaped) Segment ab 0xFD + prueft CRC. true bei Erfolg.
inline bool parseFrame(const uint8_t* raw, size_t rawLen, Frame* f) {
    uint8_t full[300];
    full[0] = START;
    size_t fl = 1 + unescape(raw + 1, rawLen - 1, full + 1);
    if (fl < 8) return false;
    uint8_t control = full[5];
    bool hs = hasSenderFlag(control);
    size_t lenIdx = hs ? 10 : 6;
    if (fl <= lenIdx) return false;
    uint8_t ln = full[lenIdx];
    size_t crcPos = hs ? (size_t)(9 + ln) : (size_t)(5 + ln);
    if (crcPos + 2 > fl) return false;
    if (crc16(full, crcPos) != (uint16_t)(((uint16_t)full[crcPos] << 8) | full[crcPos + 1]))
        return false;
    f->target  = ((uint32_t)full[1] << 24) | ((uint32_t)full[2] << 16) | ((uint32_t)full[3] << 8) | full[4];
    f->control = control;
    f->hasSender = hs;
    if (hs) {
        f->sender  = ((uint32_t)full[6] << 24) | ((uint32_t)full[7] << 16) | ((uint32_t)full[8] << 8) | full[9];
        f->dataLen = (uint8_t)(crcPos - 11);
        memcpy(f->data, full + 11, f->dataLen);
    } else {
        f->sender  = 0;
        f->dataLen = (uint8_t)(crcPos - 7);
        memcpy(f->data, full + 7, f->dataLen);
    }
    return true;
}

// Parst ein 0xFE-system/short-Frame (Booter-Antworten): FE destAddr(1B) control len payload... crc16.
// Fuellt f mit control+payload (target = 1-Byte-Zieladresse, ohne sender). true bei gueltiger CRC.
inline bool parseSystemFrame(const uint8_t* raw, size_t rawLen, Frame* f) {
    uint8_t full[300];
    full[0] = START_SHORT;
    size_t fl = 1 + unescape(raw + 1, rawLen - 1, full + 1);
    if (fl < 6) return false;                          // FE dest ctrl len + >=2 (payload/crc)
    uint8_t ln = full[3];                              // len = payload + 2 (CRC)
    if (ln < 2) return false;
    size_t crcPos = (size_t)ln + 2;                    // FE(1)+dest(1)+ctrl(1)+len(1)+payload(ln-2)
    if (crcPos + 2 > fl) return false;
    if (crc16(full, crcPos) != (uint16_t)(((uint16_t)full[crcPos] << 8) | full[crcPos + 1]))
        return false;
    f->target    = full[1];
    f->control   = full[2];
    f->hasSender = false;
    f->sender    = 0;
    f->dataLen   = (uint8_t)(ln - 2);
    memcpy(f->data, full + 4, f->dataLen);
    return true;
}

} // namespace hmw
