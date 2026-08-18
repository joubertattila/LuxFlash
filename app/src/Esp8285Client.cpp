// VENDORED (copied) file from the WiFlash project (github.com/joubertattila/WiFlash,
// v1.0.0, app/src/Esp8285Client.cpp) - UNCHANGED content. See doc/LuxFlash_terv.md.

// See Esp8285Client.h for the design rationale (why inherit from
// WiFiClient, why clone() must be overridden, the point of the +IPD
// state machine).
#include "Esp8285Client.h"
#include "Esp8285WiFi.h"
#include <string.h>

// MODERATE (max 1x/s) state printout for hardware debugging - the chain
// is proven to work, so this is off by default. If re-enabled (set to
// 1), make sure NOT to make it more verbose than this (e.g. per-byte
// printing): a prior, over-verbose version of this itself caused a
// UART buffer overflow, because data can keep arriving while
// Serial.print() is running, and nobody reads it out in the meantime.
#define WIFLASH_CLIENT_DEBUG 0

namespace {

// Just waits for a given substring on the stream, does NOT send anything
// itself (unlike Esp8285WiFi::sendCommand()) - needed to wait for the
// CIPSEND "> " prompt and the "SEND OK" response, since those are NOT
// responses to a standalone AT command, but intermediate/closing signals
// of an already-in-progress command.
bool waitForSubstring(Stream &s, const char *expect, uint32_t timeoutMs) {
    String resp;
    resp.reserve(64);
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
const size_t IPD_PREFIX_LEN = 5;  // sizeof(IPD_PREFIX)-1, without the terminating '\0'

}  // namespace

Esp8285Client::Esp8285Client() {}

std::unique_ptr<WiFiClient> Esp8285Client::clone() const {
    // See the comment in Esp8285Client.h: HTTPClient::begin(WiFiClient&, ...)
    // calls this to make its own, owned copy - if this override were
    // missing, the base WiFiClient::clone() would run, returning a plain,
    // non-functional WiFiClient - all subsequent AT traffic would just
    // silently fail with no error.
    return std::make_unique<Esp8285Client>(*this);
}

int Esp8285Client::connect(IPAddress ip, uint16_t port) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return connect(buf, port);
}

int Esp8285Client::connect(const char *host, uint16_t port) {
    // Close and reset any previous connection/state before opening a new
    // one - in CIPMUX=0 mode there can only be one connection at a time
    // anyway.
    stop();

    String cmd = "AT+CIPSTART=\"TCP\",\"";
    cmd += host;
    cmd += "\",";
    cmd += port;

    String response;
    bool ok = Esp8285WiFi::sendCommand(cmd.c_str(), "OK", 10000, &response);

    // "ALREADY CONNECTED" shouldn't be possible in principle after the
    // stop() above, but as a safety net we accept it as success too, if
    // it does happen to respond that way.
    if (!ok && response.indexOf("ALREADY CONNECT") < 0) {
        _connected = false;
        return 0;
    }

    _connected = true;
    return 1;
}

bool Esp8285Client::cipSendChunk(const uint8_t *data, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        size_t n = len - offset;
        if (n > TX_CHUNK_MAX) {
            n = TX_CHUNK_MAX;
        }

        char cmd[24];
        snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%u", (unsigned)n);

        // Discard any old, unread bytes, so they don't confuse the
        // search for the "> " prompt.
        while (Esp8285WiFi::uart().available()) {
            Esp8285WiFi::uart().read();
        }
        Esp8285WiFi::uart().print(cmd);
        Esp8285WiFi::uart().print("\r\n");

        // CIPSEND's prompt is "> " (WITHOUT a CRLF) - the raw data can
        // follow right after.
        if (!waitForSubstring(Esp8285WiFi::uart(), ">", 5000)) {
            return false;
        }

        Esp8285WiFi::uart().write(data + offset, n);

        if (!waitForSubstring(Esp8285WiFi::uart(), "SEND OK", 5000)) {
            return false;
        }

        offset += n;
    }
    return true;
}

size_t Esp8285Client::write(uint8_t b) {
    return write(&b, 1);
}

size_t Esp8285Client::write(const uint8_t *buf, size_t size) {
    size_t written = 0;
    while (written < size) {
        size_t space = TX_BUF_SIZE - _txLen;
        if (space == 0) {
            flush();
            space = TX_BUF_SIZE;
        }
        size_t chunk = size - written;
        if (chunk > space) {
            chunk = space;
        }
        memcpy(_txBuf + _txLen, buf + written, chunk);
        _txLen += chunk;
        written += chunk;
    }
    return written;
}

void Esp8285Client::flush() {
    if (_txLen > 0) {
        cipSendChunk(_txBuf, _txLen);
        _txLen = 0;
    }
}

void Esp8285Client::rxCompact() {
    if (_rxHead == 0) {
        return;
    }
    size_t remaining = _rxTail - _rxHead;
    if (remaining > 0) {
        memmove(_rxBuf, _rxBuf + _rxHead, remaining);
    }
    _rxHead = 0;
    _rxTail = remaining;
}

void Esp8285Client::processIncomingByte(uint8_t c) {
    switch (_ipdState) {
        case IpdState::ScanForIpd: {
            // Byte-by-byte matching of the "+IPD," header.
            if (c == (uint8_t)IPD_PREFIX[_matchPos]) {
                _matchPos++;
                if (_matchPos == IPD_PREFIX_LEN) {
                    _ipdState = IpdState::ReadLength;
                    _payloadLen = 0;
                    _matchPos = 0;
                    _lineLen = 0;
                    return;
                }
                // Still in the middle of the header - also feed the
                // "CLOSED" line detector, in case we're actually in a
                // standalone status line.
            } else {
                _matchPos = (c == '+') ? 1 : 0;
            }

            // Recognizing the "CLOSED" line (the peer closed the
            // connection).
            //
            // IMPORTANT, found during hardware testing: this used to be
            // matched with strstr() (substring match) - that produced a
            // false positive whenever the downloaded BINARY firmware
            // data (which can contain this exact text, e.g. from a
            // linked library's error message) happened to contain this
            // byte sequence while the state machine was transiently in
            // the ScanForIpd state (i.e. between chunk boundaries). That
            // prematurely flipped the client into "connection closed",
            // even though the download would actually have continued
            // fine.
            //
            // In CIPMUX=0 mode, the ESP8266/85 AT firmware always sends
            // the close notification as a STANDALONE "CLOSED\r\n" line
            // (nothing else on that line) - so here we require an EXACT
            // (strcmp) match, AND that the line is actually terminated
            // by "\r\n" (not a bare '\n', which can easily occur in
            // binary noise), AND that the line hasn't overflowed
            // (_lineOverflowed) - this way a "CLOSED" byte sequence that
            // happens to occur somewhere in a longer stretch of binary
            // garbage (e.g. from a linked library's error message)
            // CANNOT trigger a false close detection, unless it appears
            // EXACTLY, bounded by \r\n, on its own.
            if (c == '\r') {
                _lastWasCR = true;
            } else if (c == '\n') {
                _lineBuf[_lineLen] = '\0';
                if (_lastWasCR && !_lineOverflowed && strcmp(_lineBuf, "CLOSED") == 0) {
                    _connected = false;
                }
                _lineLen = 0;
                _lineOverflowed = false;
                _lastWasCR = false;
            } else {
                _lastWasCR = false;
                if (_lineLen < sizeof(_lineBuf) - 1) {
                    _lineBuf[_lineLen++] = (char)c;
                } else {
                    // Line too long - definitely can't be the standalone
                    // "CLOSED" message, mark the rest of the line as
                    // invalid until the next '\n' closes it.
                    _lineOverflowed = true;
                }
            }
            break;
        }

        case IpdState::ReadLength: {
            if (c >= '0' && c <= '9') {
                _payloadLen = _payloadLen * 10 + (uint32_t)(c - '0');
            } else if (c == ':') {
                _payloadRemaining = _payloadLen;
                _ipdState = (_payloadRemaining > 0) ? IpdState::CopyPayload
                                                     : IpdState::ScanForIpd;
            } else {
                // Unexpected character in the length field - defensive
                // reset.
                _ipdState = IpdState::ScanForIpd;
                _matchPos = (c == '+') ? 1 : 0;
                _lineLen = 0;
            }
            break;
        }

        case IpdState::CopyPayload: {
            if (_rxTail >= RX_BUF_SIZE) {
                // Buffer full - try to free up space at the start before
                // losing a byte.
                rxCompact();
            }
            if (_rxTail < RX_BUF_SIZE) {
                _rxBuf[_rxTail++] = c;
            }
            // If the buffer is TRULY full (the caller isn't reading fast
            // enough), this byte is lost here. See the RX_BUF_SIZE
            // comment in Esp8285Client.h: this can happen if
            // Update.writeStream() doesn't call read() during a long
            // flash-write operation, and meanwhile the amount of
            // incoming data exceeds the buffer size.
            _payloadRemaining--;
            if (_payloadRemaining == 0) {
                _ipdState = IpdState::ScanForIpd;
            }
            break;
        }
    }
}

void Esp8285Client::pollIncoming() {
    // Before waiting for inbound data, send off anything still sitting
    // in the TX buffer - otherwise HTTPUpdate's request would never
    // actually go out, and we'd wait forever for a response that could
    // never arrive.
    if (_txLen > 0) {
        cipSendChunk(_txBuf, _txLen);
        _txLen = 0;
    }

    while (Esp8285WiFi::uart().available()) {
        processIncomingByte((uint8_t)Esp8285WiFi::uart().read());
    }
    rxCompact();
}

int Esp8285Client::available() {
    pollIncoming();
    return (int)rxAvailable();
}

int Esp8285Client::read() {
    pollIncoming();
#if WIFLASH_CLIENT_DEBUG
    static uint32_t lastPrint = 0;
    if (millis() - lastPrint > 1000) {
        lastPrint = millis();
        Serial.print("[stat] rxAvail="); Serial.print((unsigned)rxAvailable());
        Serial.print(" hwUart="); Serial.print(Esp8285WiFi::uart().available());
        Serial.print(" ipdState="); Serial.print((int)_ipdState);
        Serial.print(" payloadRem="); Serial.print(_payloadRemaining);
        Serial.print(" connected="); Serial.println(_connected);
    }
#endif
    if (_rxHead >= _rxTail) {
        return -1;
    }
    return _rxBuf[_rxHead++];
}

int Esp8285Client::read(uint8_t *buf, size_t size) {
    pollIncoming();
    size_t n = rxAvailable();
    if (n > size) {
        n = size;
    }
    if (n > 0) {
        memcpy(buf, _rxBuf + _rxHead, n);
        _rxHead += n;
    }
    return (int)n;
}

int Esp8285Client::peek() {
    pollIncoming();
    if (_rxHead >= _rxTail) {
        return -1;
    }
    return _rxBuf[_rxHead];
}

void Esp8285Client::stop() {
    // This may respond with "ERROR" if there was no open connection to
    // begin with - from our point of view that's NOT an error, there's
    // just nothing to close.
    Esp8285WiFi::sendCommand("AT+CIPCLOSE", "OK", 2000);

    _connected = false;
    _txLen = 0;
    _rxHead = 0;
    _rxTail = 0;
    _ipdState = IpdState::ScanForIpd;
    _matchPos = 0;
    _lineLen = 0;
    _lineOverflowed = false;
    _lastWasCR = false;
    _payloadLen = 0;
    _payloadRemaining = 0;
}

uint8_t Esp8285Client::connected() {
    // Also need to process the inbound side here, otherwise we'd never
    // notice a "CLOSED" notification that arrived in the meantime, until
    // someone explicitly calls available()/read().
    pollIncoming();
    return (_connected || rxAvailable() > 0) ? 1 : 0;
}

int Esp8285Client::availableForWrite() {
    return (int)(TX_BUF_SIZE - _txLen);
}
