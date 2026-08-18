// VENDORED (copied) file from the WiFlash project (github.com/joubertattila/WiFlash,
// v1.0.0, app/src/Esp8285WiFi.h) - UNCHANGED content. See doc/LuxFlash_terv.md's
// "Forraskod-ujrahasznositas" ("source code reuse") section: this is the same
// "two copies, manual sync" pattern used for the RFLink/PicoMaster Python
// scripts. If this file is ever fixed/improved in WiFlash itself, the new
// version needs to be manually copied over here too.

// WiFlash - low-level AT-command layer for the ESP8285 WiFi modem.
//
// On the board (an "RP2040 W-2023" clone), the WiFi is NOT the official
// CYW43, but an ESP8285, driven with AT commands over the RP2040's UART0
// (Serial1 under arduino-pico). This class ties together the modem's
// "lifetime-level" concerns: wiring, reset, AP connection, and a shared,
// low-level "send a command, wait for the response" helper, which
// Esp8285Client (the Client subclass implementing the TCP connection)
// also uses for the CIPSTART/CIPSEND/CIPCLOSE commands.
//
// IMPORTANT: AT firmware v1.6.2.0 (SDK 2.2.1, 2018) has NO AT+HTTPCLIENT,
// so the HTTP download is done by the HTTPUpdate/Update library plus
// this Esp8285Client, over a raw TCP connection (CIPSTART/CIPSEND/+IPD).
//
// This layer deliberately assumes only ONE concurrent TCP connection
// (AT+CIPMUX=0) - that's enough for the OTA download, and makes parsing
// the AT responses far simpler than the multi-connection
// (+IPD,<id>,<len>:) mode would.

#ifndef WIFLASH_ESP8285_WIFI_H
#define WIFLASH_ESP8285_WIFI_H

#include <Arduino.h>

class Esp8285WiFi {
public:
    // Wires up Serial1 (GP0=TX -> ESP RX, GP1=RX -> from the ESP's TX),
    // then sends ATE0 (turns off command echo - without this, every AT
    // response would have the sent command copied in front of it, which
    // would needlessly complicate substring parsing) and sets
    // AT+CIPMUX=0. Returns whether the modem was successfully reset.
    static bool begin(uint32_t baud = 115200);

    // AT+CWJAP="ssid","password" - connects to an existing WiFi network.
    // Blocking call, waits up to timeoutMs for the "WIFI GOT IP" / "OK"
    // response.
    static bool joinAP(const char *ssid, const char *password, uint32_t timeoutMs = 20000);

    // IMPORTANT: this is a LIVE query (sends AT+CWJAP? to the modem on
    // every call), NOT a cached flag set once and never updated. The
    // ESP8285 AT firmware doesn't reliably send a noticeable "WIFI
    // DISCONNECT" notification in every case that we could passively
    // watch for (e.g. if the WiFi router restarts) - so we ACTIVELY
    // re-query the modem's actual, current state on every call.
    static bool isJoined();

    // Gives Esp8285Client raw access to Serial1 (it parses the +IPD
    // blocks itself, and needs the stream directly for that).
    static HardwareSerial &uart() { return Serial1; }

    // Low-level, blocking AT command send: sends cmd + "\r\n", then reads
    // the response for up to timeoutMs, checking whether any of the
    // "expect" substrings appear in it (e.g. "OK"). Also returns the full
    // raw response (if responseOut != nullptr) - useful for debugging.
    //
    // IMPORTANT: this function does NOT recognize +IPD blocks - it may
    // only be used for commands during which no inbound TCP data can
    // possibly arrive (e.g. CWJAP, CIPSTART, CIPCLOSE, CIPMUX). The
    // responses that occur DURING data transfer (SEND OK, +IPD) need
    // Esp8285Client's own, IPD-aware parser.
    static bool sendCommand(const char *cmd, const char *expect,
                             uint32_t timeoutMs, String *responseOut = nullptr);

private:
    static bool _joined;
};

#endif // WIFLASH_ESP8285_WIFI_H
