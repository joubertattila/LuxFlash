/*
  LuxFlash
  --------
  Combines WiFlash (a self-healing, WiFi-based OTA firmware updater) with
  an earlier light-intensity-sensor project (originally WFLink_test.ino) -
  on the same hardware (RP2040 Pico W-2023 clone, ESP8285 AT modem). For
  the full architecture rationale, see doc/LuxFlash_terv.md (in Hungarian
  - kept as a design-decision history log, the same way WiFlash's own
  doc/history/ folder is intentionally left untranslated).

  WHY THE TWO SYSTEMS DON'T CONFLICT even though both use WiFi: WiFlash's
  own OTA check (wiflashAppSetup()) runs ONCE, down in setup(), BEFORE
  loop() ever starts - after that, loop() is free to do anything,
  including its own network logic (see WiFlashApp.h's header comment,
  which explicitly anticipates exactly this).

  FIRST ATTEMPT, AND WHY IT CHANGED: the light-sensor server was
  originally built with the WiFiEspAT library (following the pattern of
  the old WFLink_test.ino), running ALONGSIDE WiFlash's own (client-only)
  driver. This failed at actual compile time (a real PlatformIO build)
  with a LINKER ERROR: WiFiEspAT and the framework's own WiFiClient
  (which WiFlash's Esp8285Client also derives from) both define the SAME
  global class name - the two cannot coexist in one firmware. See
  doc/LuxFlash_terv.md's "FONTOS FELFEDEZES build kozben" ("important
  discovery during the build") section for the full story.

  FINAL SOLUTION: the light-sensor server also uses WiFlash's OWN
  Esp8285WiFi-based AT layer - via a small, new companion module
  (Esp8285Server.h/.cpp) that switches the modem into CIPMUX=1
  (multi-connection) mode once wiflashAppSetup() has finished, and opens
  a TCP server with AT+CIPSERVER. Benefit: no linker conflict, AND there
  is no need to join WiFi a second time either (the connection already
  established by wiflashAppSetup() simply changes mode).

  WiFi password: we use WiFlash's compile-time
  wifi_secrets.ini/WIFLASH_WIFI_SSID and WIFLASH_WIFI_PASSWORD macros.

  Usage: same protocol as WFLink_test.ino - the light sensor can be wired
  to any ADC-capable pin, the pin number to query arrives in the Python-
  side (light_test.py) command, it is NOT hardcoded here in the
  firmware. The protocol (6 bytes, CMD_READ_ADC) and the port
  (WFLINK_PORT, Config.h) are UNCHANGED - light_test.py does not need to
  be modified.

  Usage:
    1. Copy wifi_secrets.ini.example to wifi_secrets.ini, and fill in
       your own WiFi SSID/password (see the comment there).
    2. Check WiFlashApp.cpp's HTTP_UPDATE_HOST/PORT/URI.
    3. Build and upload over USB with PlatformIO (Upload).
    4. The Serial Monitor (115200 baud) prints the IP address it
       received ("IP address: ...") and whether the server started
       ("Light sensor server running, port: ..." - if it doesn't
       succeed on the first try, the code retries on its own, see
       startServer()).
    5. Enter this IP into LuxFlash_py/config.py's WFLINK_HOST field,
       then light_test.py (from the cube server, or anywhere else) can
       query the light level.

  The built-in LED (GPIO25) gives a visual signal for when the board is
  "busy": it lights up for the whole duration of the OTA check/download
  (in setup()), and it also lights up while a light-sensor query is
  being served (from accepting the connection through sending the
  response and closing the connection, in loop()) - added on request, so
  it's visible even without a USB/Serial Monitor connection what the
  board is currently doing.
*/

#include <LittleFS.h>
#include "Config.h"
#include "HC12Link.h"
#include "WiFlashApp.h"
#include "Esp8285WiFi.h"
#include "Esp8285Server.h"

HC12Link link;
Esp8285ServerClient serverClient;

// CPU core temperature reading, UNCHANGED from the already-validated
// pattern in RFLink_test.ino (C++/RFLink/RFLink_test/RFLink_test.ino's
// readTempRaw()) - the arduino-pico core already returns a ready,
// calibrated Celsius value (analogReadTemp()), no need for manual
// ADMUX-style register handling like the old AVR version. The wire
// protocol only carries an integer, so we encode it as Celsius*100
// (centi-degrees) to keep one decimal digit of precision - the Python
// side (light_test.py) simply divides by 100.0 to convert it back.
//
// IMPORTANT thing that was deliberately NOT copied here, because it
// wasn't requested: CMD_READ_SUPPLY (supply-voltage measurement,
// VSYS/GPIO29) is CURRENTLY DISABLED in RFLink_test.ino - the author
// measured with a multimeter that on this particular board family, the
// VSYS/GPIO29 pin carries roughly 5V directly (there is no real 3:1
// voltage divider like on the official Pico(W) boards), which exceeds
// the RP2040 ADC/GPIO's ~3.3V safe limit and could damage the chip.
// This measurement was NOT ported to LuxFlash either - if an external
// voltage divider is ever added to the board, this can be added back as
// a separate, deliberate decision.
uint16_t readTempRaw() {
  float tempC = analogReadTemp();
  return (uint16_t)round(tempC * 100.0f);
}

unsigned long lastReconnectAttempt = 0;
const unsigned long RECONNECT_INTERVAL_MS = 5000;  // don't retry every single second
const unsigned long RECONNECT_TIMEOUT_MS = 20000;  // AT+CWJAP can be slow

// Whether the light-sensor server is currently running (i.e. whether
// Esp8285Server::begin() last succeeded). See startServer() and the
// comment attached to it.
bool serverRunning = false;

// The light-sensor server's (CIPMUX=1+CIPSERVER) startup/restart logic
// is gathered into one place because it needs to be called from
// SEVERAL spots: from setup() (first start), from
// maintainWifiConnection() after a WiFi reconnect (see Esp8285Server.h -
// the CIPMUX mode may not survive an AP reconnect), AND - this was
// ADDED LATER (after a real hardware test where the CIPSERVER startup
// failed once in setup(), and then WiFi stayed STABLE, so
// maintainWifiConnection()'s reconnect branch would never have run
// again - the server would have stayed down PERMANENTLY until someone
// physically restarted the board) - ALSO periodically, if the server
// didn't start successfully earlier, even if WiFi never actually drops.
bool startServer() {
  if (Esp8285Server::begin(WFLINK_PORT)) {
    serverRunning = true;
    Serial.print("Light sensor server running, port: ");
    Serial.println(WFLINK_PORT);
    return true;
  }
  serverRunning = false;
  Serial.println("ERROR: failed to (re)start the light sensor server (CIPSERVER).");
  return false;
}

// Queries and prints the modem's current IP address to the Serial
// Monitor (AT+CIFSR - "+CIFSR:STAIP,\"<ip>\"" in the response, in
// station/client mode). THIS WAS MISSING from the first version:
// WFLink_test.ino printed it with WiFiEspAT's WiFi.localIP(), but
// WiFlash's own Esp8285WiFi driver (which we switched to because of the
// linker conflict, see doc/LuxFlash_terv.md) has no ready-made function
// for this - added here as its own AT command. This is needed so the
// user can fill in LuxFlash_py/config.py's WFLINK_HOST.
void printIpAddress() {
  String response;
  if (Esp8285WiFi::sendCommand("AT+CIFSR", "OK", 3000, &response)) {
    int start = response.indexOf("+CIFSR:STAIP,\"");
    if (start >= 0) {
      start += strlen("+CIFSR:STAIP,\"");
      int end = response.indexOf('"', start);
      if (end > start) {
        Serial.print("IP address: ");
        Serial.println(response.substring(start, end));
        return;
      }
    }
  }
  Serial.println("Failed to query the IP address (AT+CIFSR).");
}

// If the WiFi connection has dropped (e.g. due to a flaky signal
// repeater), this function periodically (every RECONNECT_INTERVAL_MS)
// retries - same pattern as WFLink_test.ino, just built on Esp8285WiFi's
// LIVE isJoined() (instead of WiFiEspAT's WiFi.status() - see
// Esp8285WiFi.h's "IMPORTANT: this is a LIVE query" note).
//
// CRITICAL FIX (after the first hardware test): ORIGINALLY, the
// isJoined() call ran on EVERY SINGLE loop() iteration (only the ACTUAL
// reconnect ATTEMPT was rate-limited) - this was wrong for two reasons:
// 1) isJoined() sends a LIVE AT+CWJAP? command, which - with AT
//    diagnostics turned on (WIFLASH_AT_DEBUG) - flooded the Serial
//    Monitor.
// 2) MUCH MORE SERIOUS: every call to Esp8285WiFi::sendCommand()
//    discards ALL bytes still waiting in the UART buffer BEFORE sending
//    the command ("discard any bytes possibly still stuck in the
//    buffer" - see Esp8285WiFi.cpp). If a light_test.py connection's
//    "+IPD,..." data happens to arrive exactly while an isJoined() call
//    is in progress - which, given the non-stop calling, was nearly
//    guaranteed to happen sooner or later - those bytes get LOST before
//    Esp8285Server::poll() ever sees them. This was LIKELY THE ACTUAL
//    CAUSE of light_test.py's timeouts. Fixed: now the isJoined() check
//    itself is also rate-limited (not just the reconnect), using the
//    same timer.
void maintainWifiConnection() {
  unsigned long now = millis();
  if (now - lastReconnectAttempt < RECONNECT_INTERVAL_MS) {
    return;
  }
  lastReconnectAttempt = now;

  if (Esp8285WiFi::isJoined()) {
    // WiFi is stable - but if the server didn't start successfully
    // earlier (e.g. in setup()), retry it here periodically, even if
    // there was no actual WiFi drop. See startServer()'s header comment.
    if (!serverRunning) {
      startServer();
    }
    return;
  }

  Serial.println("WiFi connection lost, reconnecting...");
  if (Esp8285WiFi::joinAP(WIFLASH_WIFI_SSID, WIFLASH_WIFI_PASSWORD, RECONNECT_TIMEOUT_MS)) {
    Serial.println("Reconnected.");
    // The IP may have changed with a new DHCP lease - print it again so
    // the user doesn't end up trying an already-stale address in
    // config.py.
    printIpAddress();
    // The CIPSERVER also needs to be restarted on the fresh connection,
    // just to be safe, even if the CIPMUX=1 mode itself survives an
    // AP reconnect (this part hasn't been tested on hardware yet - see
    // Esp8285Server.h).
    startServer();
  } else {
    Serial.println("Reconnect failed, will retry later.");
  }
}

void setup() {
  Serial.begin(115200);

  // Wait for the Serial Monitor with a timeout (max 2s) - if the board
  // is powered from a supply with no USB host attached, don't get stuck
  // here forever.
  unsigned long serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 2000) {
    delay(10);
  }

  // 12-bit ADC resolution (0..4095) - Config.h's ADC_MAX_VAL assumes
  // this. This was MISSING from WFLink_test.ino (only
  // ESP8285_WebServer_Test.ino called it) - added here, see Config.h's
  // comment.
  analogReadResolution(12);

  // The built-in LED (GPIO25, same as in WiFlash's original demo - see
  // WiFlash's app/src/main.cpp) for visual feedback: lights up 1) for
  // the whole duration of the OTA check/download, and 2) while a
  // light-sensor query is being served - see both spots below.
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  // 1) WiFlash: a SINGLE OTA check with its own AT driver
  //    (Esp8285WiFi/Esp8285Client). If it finds and applies new
  //    firmware, it restarts (this call then never returns) -
  //    otherwise, on success, the modem stays CONNECTED to WiFi.
  // wiflashAppSetup() itself lives in the VENDORED WiFlashApp.cpp (see
  // the "vendored, unchanged" note there) - THAT'S WHY the LED control
  // is placed AROUND this call instead of inside it, so the vendored
  // file doesn't need to be touched. On a successful APPLY, the call
  // never returns (the board restarts before reaching the LED-off line)
  // - this is intentional, the LED simply stays lit until the restart,
  // which happens almost immediately anyway.
  digitalWrite(LED_BUILTIN, HIGH);
  wiflashAppSetup();
  digitalWrite(LED_BUILTIN, LOW);

  // Safety net: if wiflashAppSetup() didn't connect for any reason (e.g.
  // a modem hiccup), try once more here - so the light-sensor server
  // can still start even if only the OTA part failed.
  if (!Esp8285WiFi::isJoined()) {
    Serial.println("One more attempt at connecting to WiFi...");
    // The return value wasn't logged before either - another blind spot
    // found during debugging (see doc/LuxFlash_terv.md), now fixed.
    if (Esp8285WiFi::joinAP(WIFLASH_WIFI_SSID, WIFLASH_WIFI_PASSWORD)) {
      Serial.println("Succeeded.");
    } else {
      Serial.println("ERROR: failed to connect even on the second attempt.");
    }
  }

  if (Esp8285WiFi::isJoined()) {
    printIpAddress();
  }

  // 2) Light-sensor server: CIPMUX=1 + CIPSERVER - see Esp8285Server.h.
  // The AT modem is occasionally stubborn (during debugging we
  // repeatedly saw transient "busy p..."-like responses) - so a few
  // IMMEDIATE retries before giving up. If all of these also fail,
  // maintainWifiConnection() in loop() keeps retrying LATER too (see
  // startServer()'s header comment - this covers the earlier gap where,
  // if WiFi stayed stable, nothing would have retried the server at
  // all).
  for (int attempt = 0; attempt < 3 && !startServer(); attempt++) {
    delay(500);
  }
}

void loop() {
  maintainWifiConnection();

  if (!Esp8285Server::poll(serverClient)) {
    return;  // no active connection/data
  }

  // LED turned on as soon as a client's (e.g. light_test.py) connection
  // becomes active - turned back off at the end of this block (AFTER
  // sending the response AND closing the connection), see below.
  digitalWrite(LED_BUILTIN, HIGH);

  // HC12Link is built on a Stream*, just like with the HC-12 - here we
  // hand it the Esp8285ServerClient instead of the radio.
  link.init(&serverClient, DEVICE_ID, BROADCAST_ID);

  HC12Command command;
  bool commandReceived = false;
  unsigned long start = millis();

  // Wait at most 1 second for the 6-byte command, while continuously
  // collecting any further incoming bytes - otherwise we'd get stuck
  // here forever on a stalled/interrupted connection.
  while (millis() - start < 1000) {
    Esp8285Server::poll(serverClient);
    if (link.checkForCommand(&command)) {
      commandReceived = true;
      break;
    }
  }

  if (commandReceived && !command.broadcast) {
    switch (command.cmd) {
      case CMD_READ_ADC: {
        uint16_t value = analogRead(command.pin);
        link.sendResponse(CMD_READ_ADC, value);
        Serial.print("READ_ADC pin=");
        Serial.print(command.pin);
        Serial.print(" -> ");
        Serial.println(value);
        break;
      }
      case CMD_READ_TEMP: {
        // No PIN parameter (HC12Link.h calls it "padding" for this
        // command) - same pattern as in RFLink_test.ino.
        uint16_t value = readTempRaw();
        link.sendResponse(CMD_READ_TEMP, value);
        Serial.print("READ_TEMP raw=");
        Serial.println(value);
        break;
      }
      default:
        Serial.print("Unsupported command: 0x");
        Serial.println(command.cmd, HEX);
        break;
    }
  }

  Esp8285Server::closeClient(serverClient);

  // The response has been sent AND the connection closed - as far as
  // the requester (light_test.py) is concerned, the query is "done",
  // turn the LED back off here.
  digitalWrite(LED_BUILTIN, LOW);
}
