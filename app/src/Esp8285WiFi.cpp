// VENDORED (copied) file from the WiFlash project (github.com/joubertattila/WiFlash,
// v1.0.0, app/src/Esp8285WiFi.cpp) - UNCHANGED content. See doc/LuxFlash_terv.md.

// See Esp8285WiFi.h for the design rationale (why an AT-command modem,
// why isJoined() is a live query, etc.).
#include "Esp8285WiFi.h"

// Raw AT traffic debug printout to USB Serial - useful for hardware
// debugging (raw AT commands/responses), off by default since the chain
// is proven to work. Can be switched back on (set to 1) if something
// breaks.
//
// LUXFLASH: temporarily turned on on 2026-08-17 for hardware debugging
// of Esp8285Server (CIPMUX=1/CIPSERVER) - light_test.py was timing out.
// The debugging was SUCCESSFUL (found and fixed both isJoined() being
// called too often and a wrong OTA host name); light_test.py has been
// working flawlessly on real hardware ever since. TURNED BACK OFF to 0,
// as WiFlash's own original file also prescribes ("off by default").
#define WIFLASH_AT_DEBUG 0

bool Esp8285WiFi::_joined = false;

bool Esp8285WiFi::begin(uint32_t baud) {
    // GP0 = TX (towards the ESP's RX), GP1 = RX (from the ESP's TX) -
    // see doc/*/intro_*.md "Hardware".
    Serial1.setTX(0);
    Serial1.setRX(1);

    // IMPORTANT, found during hardware testing: arduino-pico's
    // HardwareSerial default RX buffer is only 32 bytes (see
    // SerialUART.h _fifoSize = 32). At 115200 baud that fills up in
    // about 2.7 ms - if loop() is doing anything else in the meantime
    // (e.g. Update writing a flash page to LittleFS), the overflow
    // silently gets LOST, and the +IPD state machine (Esp8285Client)
    // irrecoverably desyncs from the binary data stream. setFIFOSize()
    // must be called BEFORE Serial1.begin().
    Serial1.setFIFOSize(4096);
    Serial1.begin(baud);

    // The ESP8285 needs a bit of time after power-on/reset before it
    // responds to AT commands - avoid needlessly running into a "no
    // response" error.
    delay(500);

    // Turn off command echo - without this, every response would have
    // the sent command copied in front of it, which would make the
    // response-substring search harder.
    if (!sendCommand("ATE0", "OK", 2000)) {
        return false;
    }

    // Single, concurrent-TCP-connection mode - this way inbound data
    // always arrives as a plain "+IPD,<len>:" (no channel ID in front).
    if (!sendCommand("AT+CIPMUX=0", "OK", 2000)) {
        return false;
    }

    return true;
}

bool Esp8285WiFi::joinAP(const char *ssid, const char *password, uint32_t timeoutMs) {
    // AT+CWMODE=1: station mode (we connect as a client to an existing
    // network, rather than creating an access point).
    if (!sendCommand("AT+CWMODE=1", "OK", 2000)) {
        return false;
    }

    String cmd = "AT+CWJAP=\"";
    cmd += ssid;
    cmd += "\",\"";
    cmd += password;
    cmd += "\"";

    // Connecting (together with DHCP) can take several seconds, so this
    // gets the caller-supplied, generous timeout. On this AT version a
    // successful response typically looks like
    // "WIFI CONNECTED\r\nWIFI GOT IP\r\n\r\nOK\r\n" - it's enough to
    // look for the closing "OK".
    _joined = sendCommand(cmd.c_str(), "OK", timeoutMs);
    return _joined;
}

bool Esp8285WiFi::isJoined() {
    // AT+CWJAP? - queries the CURRENT AP-connection state. A connected
    // modem replies with "+CWJAP:<ssid>,<bssid>,<channel>,<rssi>\r\n\r\nOK\r\n" -
    // the presence of "+CWJAP:" is the clear, version-independent sign
    // that there's ACTUALLY an active connection (as opposed to just the
    // command ending in "OK", since "No AP"+"OK" can also occur WITHOUT
    // a connection).
    String response;
    bool ok = sendCommand("AT+CWJAP?", "OK", 3000, &response);
    _joined = ok && (response.indexOf("+CWJAP:") >= 0);
    return _joined;
}

bool Esp8285WiFi::sendCommand(const char *cmd, const char *expect,
                               uint32_t timeoutMs, String *responseOut) {
    // Discard any bytes possibly still stuck in the buffer from before,
    // so they don't get mixed into parsing the new response.
    while (Serial1.available()) {
        Serial1.read();
    }

    Serial1.print(cmd);
    Serial1.print("\r\n");

    String response;
    response.reserve(128);

    uint32_t start = millis();
    bool found = false;
    while (millis() - start < timeoutMs) {
        while (Serial1.available()) {
            char c = (char)Serial1.read();
            response += c;
            if (strstr(response.c_str(), expect) != nullptr) {
                found = true;
            }
            if (strstr(response.c_str(), "ERROR") != nullptr) {
                // Explicit error response - no point waiting any longer.
                if (responseOut) {
                    *responseOut = response;
                }
#if WIFLASH_AT_DEBUG
                Serial.print("[AT] >> ");
                Serial.println(cmd);
                Serial.print("[AT] << ");
                Serial.println(response);
#endif
                return false;
            }
        }
        if (found) {
            break;
        }
    }

#if WIFLASH_AT_DEBUG
    Serial.print("[AT] >> ");
    Serial.println(cmd);
    Serial.print("[AT] << ");
    Serial.println(response);
    if (!found) {
        Serial.println("[AT] (timeout, expected response not found)");
    }
#endif

    if (responseOut) {
        *responseOut = response;
    }
    return found;
}
