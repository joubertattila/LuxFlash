// LuxFlash - NEW file (not vendored). See Esp8285Server.h for the
// design rationale (why this is needed, what it deliberately
// simplifies, not yet tested on hardware).

#include "Esp8285Server.h"
#include "Esp8285WiFi.h"
#include <string.h>

// LUXFLASH: temporarily turned on during hardware debugging on
// 2026-08-17 (light_test.py was timing out). The debugging was
// SUCCESSFUL: this log was what first showed that the "+IPD,0,6:" frame
// and the correct response actually make it through ("[server] NEW
// CONNECTION, link_id=0, length=6" -> "READ_ADC pin=27 -> ..." ->
// "[server] sending response...") - the CIPSERVER logic was thereby
// VALIDATED ON REAL HARDWARE. TURNED BACK OFF - it would be noisy if
// left on during every loop() iteration.
#define LUXFLASH_SERVER_DEBUG 0

namespace {

// Same helper as in Esp8285Client.cpp: waits for a given substring on
// the serial port, sends nothing itself - needed to wait for CIPSEND's
// "> " prompt / "SEND OK" response.
bool waitForSubstring(Stream &s, const char *expect, uint32_t timeoutMs) {
    String resp;
    resp.reserve(32);
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        while (s.available()) {
            resp += (char)s.read();
            if (strstr(resp.c_str(), expect) != nullptr) {
                return true;
            }
        }
    }
    return false;
}

const char IPD_PREFIX[] = "+IPD,";
const size_t IPD_PREFIX_LEN = 5;

// State of the "+IPD,<link_id>,<length>:<data>" parsing state machine -
// a SINGLE, global instance is enough (there's only one modem). The
// ScanForIpd/ReadLength/CopyPayload trio is the same pattern as in
// Esp8285Client.cpp, just SPLIT to also read the link_id AND the length
// (CIPMUX=0 has no link_id field).
enum class IpdState { ScanForIpd, ReadLinkId, ReadLength, CopyPayload, DiscardPayload };

IpdState g_state = IpdState::ScanForIpd;
uint8_t g_matchPos = 0;
uint8_t g_incomingLinkId = 0;
uint32_t g_payloadLen = 0;
uint32_t g_payloadRemaining = 0;

}  // namespace

// Defined as a member function (not in the anonymous namespace above),
// so that "friend class Esp8285Server;" (see Esp8285Server.h) grants
// access to Esp8285ServerClient's private fields - it still reaches the
// state-machine variables above (g_state etc.) unchanged, since we're
// in the same translation unit.
void Esp8285Server::feedByte(Esp8285ServerClient &client, uint8_t c) {
    switch (g_state) {
        case IpdState::ScanForIpd: {
            if (c == (uint8_t)IPD_PREFIX[g_matchPos]) {
                g_matchPos++;
                if (g_matchPos == IPD_PREFIX_LEN) {
                    g_state = IpdState::ReadLinkId;
                    g_incomingLinkId = 0;
                    g_matchPos = 0;
                }
            } else {
                g_matchPos = (c == '+') ? 1 : 0;
            }
            break;
        }

        case IpdState::ReadLinkId: {
            if (c >= '0' && c <= '9') {
                g_incomingLinkId = (uint8_t)(g_incomingLinkId * 10 + (c - '0'));
            } else if (c == ',') {
                g_state = IpdState::ReadLength;
                g_payloadLen = 0;
            } else {
                // Unexpected character - defensive reset.
                g_state = IpdState::ScanForIpd;
                g_matchPos = (c == '+') ? 1 : 0;
            }
            break;
        }

        case IpdState::ReadLength: {
            if (c >= '0' && c <= '9') {
                g_payloadLen = g_payloadLen * 10 + (uint32_t)(c - '0');
            } else if (c == ':') {
                g_payloadRemaining = g_payloadLen;

                // If 'client' isn't active yet, this is a NEW
                // connection's first data - set it up for this link_id
                // (see Esp8285Server.h's "DELIBERATE SIMPLIFICATION" -
                // we react to the first +IPD, not to the CONNECT line).
                if (!client._active) {
                    client.reset();
                    client._active = true;
                    client._linkId = g_incomingLinkId;
#if LUXFLASH_SERVER_DEBUG
                    Serial.print("\n[server] NEW CONNECTION, link_id=");
                    Serial.print(g_incomingLinkId);
                    Serial.print(", length=");
                    Serial.println(g_payloadRemaining);
#endif
                }

                bool belongsToThisConnection = (g_incomingLinkId == client._linkId);

                if (g_payloadRemaining == 0) {
                    g_state = IpdState::ScanForIpd;
                } else if (belongsToThisConnection) {
                    g_state = IpdState::CopyPayload;
                } else {
                    // A SECOND connection's data arrived while we're
                    // still serving the first one - its bytes MUST be
                    // consumed (otherwise the state machine would
                    // misinterpret them as "+IPD," lookups), but we do
                    // NOT store them. An acceptable simplification for
                    // a rarely-queried sensor endpoint.
                    g_state = IpdState::DiscardPayload;
                }
            } else {
                g_state = IpdState::ScanForIpd;
                g_matchPos = (c == '+') ? 1 : 0;
            }
            break;
        }

        case IpdState::CopyPayload: {
            if (client._rxTail < Esp8285ServerClient::BUF_SIZE) {
                client._rxBuf[client._rxTail++] = c;
            }
            // If the buffer fills up, the extra bytes are lost - the
            // HC12 frame is a fixed 6 bytes, the 32-byte buffer is
            // generously enough, this is only a safety limit.
            g_payloadRemaining--;
            if (g_payloadRemaining == 0) {
                g_state = IpdState::ScanForIpd;
            }
            break;
        }

        case IpdState::DiscardPayload: {
            g_payloadRemaining--;
            if (g_payloadRemaining == 0) {
                g_state = IpdState::ScanForIpd;
            }
            break;
        }
    }
}

void Esp8285ServerClient::reset() {
    _active = false;
    _linkId = 0;
    _rxHead = 0;
    _rxTail = 0;
    _txLen = 0;
}

int Esp8285ServerClient::available() {
    return (int)(_rxTail - _rxHead);
}

int Esp8285ServerClient::read() {
    if (_rxHead >= _rxTail) {
        return -1;
    }
    return _rxBuf[_rxHead++];
}

int Esp8285ServerClient::peek() {
    if (_rxHead >= _rxTail) {
        return -1;
    }
    return _rxBuf[_rxHead];
}

size_t Esp8285ServerClient::write(uint8_t b) {
    return write(&b, 1);
}

size_t Esp8285ServerClient::write(const uint8_t *buf, size_t size) {
    size_t space = BUF_SIZE - _txLen;
    size_t n = (size > space) ? space : size;  // safety limit - the 6-byte HC12 response fits comfortably
    memcpy(_txBuf + _txLen, buf, n);
    _txLen += n;
    return n;
}

void Esp8285ServerClient::flush() {
    if (_txLen == 0 || !_active) {
        return;
    }

#if LUXFLASH_SERVER_DEBUG
    Serial.print("[server] sending response, link_id=");
    Serial.print(_linkId);
    Serial.print(", length=");
    Serial.println(_txLen);
#endif

    char cmd[24];
    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%u,%u", (unsigned)_linkId, (unsigned)_txLen);

    while (Esp8285WiFi::uart().available()) {
        Esp8285WiFi::uart().read();
    }
    Esp8285WiFi::uart().print(cmd);
    Esp8285WiFi::uart().print("\r\n");

    if (waitForSubstring(Esp8285WiFi::uart(), ">", 5000)) {
        Esp8285WiFi::uart().write(_txBuf, _txLen);
        waitForSubstring(Esp8285WiFi::uart(), "SEND OK", 5000);
    }

    _txLen = 0;
}

bool Esp8285Server::begin(uint16_t port) {
    if (!Esp8285WiFi::sendCommand("AT+CIPMUX=1", "OK", 2000)) {
        return false;
    }

    char cmd[24];
    snprintf(cmd, sizeof(cmd), "AT+CIPSERVER=1,%u", (unsigned)port);
    return Esp8285WiFi::sendCommand(cmd, "OK", 3000);
}

bool Esp8285Server::poll(Esp8285ServerClient &client) {
    while (Esp8285WiFi::uart().available()) {
        uint8_t b = (uint8_t)Esp8285WiFi::uart().read();
#if LUXFLASH_SERVER_DEBUG
        // Print every raw byte coming from the modem - printable
        // characters verbatim, everything else as [hex]. This shows
        // whether the modem sends anything AT ALL (e.g. "+IPD,..." or a
        // "0,CONNECT" line) when a client tries to connect.
        if (b >= 32 && b < 127) {
            Serial.write(b);
        } else {
            Serial.print('[');
            Serial.print(b, HEX);
            Serial.print(']');
        }
#endif
        feedByte(client, b);
    }
    return client._active;
}

void Esp8285Server::closeClient(Esp8285ServerClient &client) {
    if (client._active) {
        char cmd[24];
        snprintf(cmd, sizeof(cmd), "AT+CIPCLOSE=%u", (unsigned)client._linkId);
        Esp8285WiFi::sendCommand(cmd, "OK", 2000);
    }
    client.reset();

    // Defensive reset: if we were in the middle of an interrupted frame
    // (e.g. the connection dropped unexpectedly mid-service), don't get
    // stuck there - the next "+IPD," search starts fresh.
    g_state = IpdState::ScanForIpd;
    g_matchPos = 0;
}
