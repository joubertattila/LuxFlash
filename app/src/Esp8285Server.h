// LuxFlash - NEW file (NOT vendored from WiFlash - WiFlash has no
// equivalent, because WiFlash's own Esp8285Client is deliberately
// OUTBOUND-only (client), see doc/LuxFlash_terv.md's "FONTOS FELFEDES
// build kozben" / "important discovery during the build" section). This
// module provides the TCP SERVER capability needed for the light-sensor
// query - built on the SAME Esp8285WiFi AT layer as Esp8285Client, which
// AVOIDS the linker conflict with the WiFiEspAT library (discovered
// during a trial build).
//
// NOT YET TESTED ON HARDWARE - this is a completely new, hand-written
// AT-command layer (CIPMUX=1, AT+CIPSERVER), NOT a copy of an
// already-validated component. It needs to be verified on live hardware
// before you rely on it (the same way PicoMasterLink's echo test was
// also first tried out on live HC-12 hardware - see the project-
// picomaster memory note in the original author's notes).
//
// HOW IT WORKS: in CIPMUX=0 mode (which wiflashAppSetup() sets up with
// WiFlash's own driver), the ESP8285 AT modem can only handle ONE
// connection, and cannot act as a server either. In CIPMUX=1
// (multi-connection) mode, however, AT+CIPSERVER=1,<port> opens a TCP
// server, and incoming data arrives in the format
// "+IPD,<link_id>,<length>:<data>" (instead of CIPMUX=0's simpler
// "+IPD,<length>:<data>" - see Esp8285Client.cpp). This module handles
// THIS link_id-carrying format.
//
// DELIBERATE SIMPLIFICATION: we don't separately watch for the modem's
// "<id>,CONNECT" notification - a connection is only recognized as
// "active" once its FIRST data (its +IPD block) arrives. Since
// light_test.py sends the 6-byte request immediately (right after
// connecting), this doesn't cause any practical delay - in exchange, we
// don't need to also build in a second, line-based pattern match (to
// recognize the "<id>,CONNECT\r\n" line), which would add more code and
// more failure modes. If a client ever connected but never sent
// anything, this layer would NOT notice (its link_id would stay open
// until the other side closes it itself) - on a private, rarely-queried
// sensor endpoint, this is an acceptable risk.

#ifndef LUXFLASH_ESP8285_SERVER_H
#define LUXFLASH_ESP8285_SERVER_H

#include <Arduino.h>
#include <Stream.h>

// Represents a single accepted TCP connection (by the modem's link_id,
// 0-4 in CIPMUX=1 mode). Derived from Stream so it can be plugged into
// HC12Link::init() UNCHANGED (see HC12Link.h) - same pattern as how
// WiFlash's Esp8285Client also derives from the framework's WiFiClient
// for the HTTPClient API.
class Esp8285ServerClient : public Stream {
public:
    int available() override;
    int read() override;
    int peek() override;

    size_t write(uint8_t b) override;
    size_t write(const uint8_t *buf, size_t size) override;
    void flush() override;

    bool connected() const { return _active; }

    friend class Esp8285Server;

private:
    // 32 bytes is plenty: the HC12 protocol frame is a fixed 6 bytes
    // (see HC12Link.h's HC12_FRAME_SIZE), this is just a comfortable
    // margin.
    static const size_t BUF_SIZE = 32;

    uint8_t _linkId = 0;
    bool _active = false;

    uint8_t _rxBuf[BUF_SIZE];
    size_t _rxHead = 0;
    size_t _rxTail = 0;

    uint8_t _txBuf[BUF_SIZE];
    size_t _txLen = 0;

    void reset();
};

// Static (class-level) API - there's only one modem, one server, same
// pattern as Esp8285WiFi.
class Esp8285Server {
public:
    // AT+CIPMUX=1, then AT+CIPSERVER=1,<port>. IMPORTANT: this SWITCHES
    // the modem into CIPMUX=1 mode - from this point on, Esp8285Client
    // (WiFlash's OTA client, which assumes CIPMUX=0) can no longer be
    // used for the rest of this power-on cycle. Only call this AFTER
    // wiflashAppSetup() (see main.cpp and doc/LuxFlash_terv.md).
    static bool begin(uint16_t port);

    // Non-blocking - must be called from every loop() iteration (or
    // every round of a waiting loop). Processes whatever UART bytes are
    // immediately available. If a NEW connection's data arrives, sets
    // up 'client' for it (discarding its previous state). Returns
    // whether 'client' currently represents an active connection.
    static bool poll(Esp8285ServerClient &client);

    // AT+CIPCLOSE=<link_id> - closes the connection, and resets
    // 'client' back into a state ready to accept a new one.
    static void closeClient(Esp8285ServerClient &client);

private:
    // A single byte-step of the "+IPD,<link_id>,<length>:<data>" state
    // machine - this has to be a member function (not a plain function
    // in an anonymous namespace), because "friend class Esp8285Server;"
    // (see above) only grants access to Esp8285ServerClient's private
    // fields this way.
    static void feedByte(Esp8285ServerClient &client, uint8_t c);
};

#endif // LUXFLASH_ESP8285_SERVER_H
