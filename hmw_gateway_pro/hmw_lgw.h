// hmw_lgw.h -- LGW-LAN-Schicht fuer ESP32: LAN-Frames, Bridge-Uebersetzung, AES.
// Portierung der verifizierten hmw_lgw.py / hmw_lgw_crypto.py.
#pragma once
#include "hmw_protocol.h"
#include "mbedtls/aes.h"
#include "mbedtls/md5.h"

namespace lgw {

enum {  // LAN-Kommando-Bytes (am Mitschnitt verifiziert)
    CMD_KEEPALIVE = 0x4B, CMD_ALIVE = 0x61, CMD_SEND = 0x53, CMD_DISCOVERY = 0x44,
    CMD_DEVFOUND = 0x64,  CMD_DISCDONE = 0x63, CMD_RECEIVED = 0x65, CMD_RESPONSE = 0x72
};
enum { MODE_UNICAST_ACK = 0xC8, MODE_BROADCAST = 0x00 };

// LAN-Frame: FD | size | index | payload   (size = index+payload, FC-Escaping, KEIN CRC)
inline size_t lanEncode(uint8_t idx, const uint8_t* payload, uint8_t plen, uint8_t* out) {
    uint8_t body[300]; size_t n = 0;
    body[n++] = (uint8_t)(plen + 1);
    body[n++] = idx;
    memcpy(body + n, payload, plen); n += plen;
    out[0] = hmw::START;
    return 1 + hmw::escape(body, n, out + 1);
}

// Extrahiert EIN LAN-Frame ab buf[0]==FD. Rueckgabe: verbrauchte Wire-Bytes (0 = unvollstaendig).
inline size_t lanExtract(const uint8_t* buf, size_t len, uint8_t* idx, uint8_t* payload, uint8_t* plen) {
    if (len < 1 || buf[0] != hmw::START) return 0;
    uint8_t logical[300]; size_t lo = 0, j = 1; int size = -1;
    while (j < len) {
        uint8_t b = buf[j];
        if (b == hmw::ESC) { if (j + 1 >= len) return 0; logical[lo++] = buf[j + 1] | 0x80; j += 2; }
        else if (b == hmw::START) return 0;
        else { logical[lo++] = b; j++; }
        if (size < 0) size = logical[0];
        if ((int)lo >= 1 + size) { *idx = logical[1]; *plen = (uint8_t)(size - 1);
                                   memcpy(payload, logical + 2, *plen); return j; }
    }
    return 0;
}

// Eingebettetes Frame (recv ctrl sender busdaten) -> vollstaendiges Bus-Frame (FD+len+CRC).
inline size_t embeddedToBus(const uint8_t* emb, size_t elen, uint8_t* out) {
    uint32_t recv  = ((uint32_t)emb[0]<<24)|((uint32_t)emb[1]<<16)|((uint32_t)emb[2]<<8)|emb[3];
    uint8_t  ctrl  = emb[4];
    uint32_t sndr  = ((uint32_t)emb[5]<<24)|((uint32_t)emb[6]<<16)|((uint32_t)emb[7]<<8)|emb[8];
    return hmw::buildFrame(recv, ctrl, sndr, emb + 9, (uint8_t)(elen - 9), out);
}

// Bus-Event -> LAN 'e'-Payload (volle Adressierung)
inline uint8_t frameToReceived(const hmw::Frame& f, uint8_t* out) {
    size_t n = 0; out[n++] = CMD_RECEIVED;
    out[n++]=f.target>>24; out[n++]=f.target>>16; out[n++]=f.target>>8; out[n++]=(uint8_t)f.target;
    out[n++]=f.control;
    out[n++]=f.sender>>24; out[n++]=f.sender>>16; out[n++]=f.sender>>8; out[n++]=(uint8_t)f.sender;
    memcpy(out + n, f.data, f.dataLen); n += f.dataLen; return (uint8_t)n;
}
// Bus-Antwort -> LAN 'r'-Payload (ctrl + busdaten)
inline uint8_t frameToResponse(const hmw::Frame& f, uint8_t* out) {
    out[0]=CMD_RESPONSE; out[1]=f.control; memcpy(out+2, f.data, f.dataLen); return (uint8_t)(2 + f.dataLen);
}

// ---- AES-128-CFB Krypto (mbedTLS) ---------------------------------------- //
inline void lanKey(const char* passphrase, uint8_t out[16]) {
    mbedtls_md5((const unsigned char*)passphrase, strlen(passphrase), out);
}

struct Crypto {                     // ausgehend mit Peer(CCU)-IV, eingehend mit eigener(LGW)-IV
    mbedtls_aes_context aes;
    uint8_t eiv[16], div[16];
    size_t  eoff = 0, doff = 0;
    void begin(const uint8_t key[16], const uint8_t own_iv[16], const uint8_t peer_iv[16]) {
        mbedtls_aes_init(&aes);
        mbedtls_aes_setkey_enc(&aes, key, 128);     // CFB nutzt den Block-Cipher immer im Enc-Modus
        memcpy(eiv, peer_iv, 16); eoff = 0;
        memcpy(div, own_iv, 16);  doff = 0;
    }
    void encrypt(const uint8_t* in, uint8_t* out, size_t n) {   // LGW -> CCU
        mbedtls_aes_crypt_cfb128(&aes, MBEDTLS_AES_ENCRYPT, n, &eoff, eiv, in, out);
    }
    void decrypt(const uint8_t* in, uint8_t* out, size_t n) {   // CCU -> LGW
        mbedtls_aes_crypt_cfb128(&aes, MBEDTLS_AES_DECRYPT, n, &doff, div, in, out);
    }
};

} // namespace lgw
