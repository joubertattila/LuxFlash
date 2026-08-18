// VENDORED (copied) file from the WiFlash project (github.com/joubertattila/WiFlash,
// v1.0.0, app/src/WiFlashApp.cpp) - the logic is UNCHANGED, only
// HTTP_UPDATE_HOST/PORT/URI were filled in (see below) in place of
// WiFlash's original "192.0.2.1" (RFC 5737, documentation) placeholder.
// See doc/LuxFlash_terv.md.
//
// LUXFLASH DECISION about HTTP_UPDATE_HOST/PORT/URI (2026-08-17,
// CONFIRMED by checking the cube's actual Apache config over an SFTP
// mount - /etc/apache2/sites-available/nextcloud.conf, the "3. RESZ:
// WIFLASH OTA FIRMWARE KISZOLGALAS" / "part 3: WiFlash OTA firmware
// hosting" section):
// - Host/port: port 8090, DocumentRoot=/var/www/html/wiflash - this is
//   the `vril.ddns.net:8090` vhost, plain HTTP (no HTTPS redirect,
//   since the ESP8285 AT firmware v1.6.2.0 can't do TLS). The same
//   server that PicoMaster also uses (a SHARED docroot, not a separate
//   one per device).
// - FIXED (2026-08-17, after the first hardware test): ORIGINALLY the
//   "cube" hostname was used here - this died with an AT+CIPSTART
//   timeout on real hardware (the HTTP_UPDATE_HOST value below).
//   Turned out: the ESP8285 AT modem (on this firmware/DNS setup)
//   CANNOT resolve the "cube" name - PicoMaster (`PicoMaster/Config.h`'s
//   OTA_UPDATE_HOST) therefore already used the RAW IP ("192.168.0.2 //
//   the cube's IP on the 'vidor' network"), not the hostname. Following
//   the same pattern, LuxFlash is also set to the raw IP.
// - The URI was DELIBERATELY set to "/luxflash/luxflash_firmware.bin"
//   (WITH A SUBDIRECTORY) - inspecting the docroot's actual contents
//   revealed that PicoMaster does NOT publish at the docroot's root,
//   but in its own "/picomaster/picomaster_firmware.bin" subdirectory
//   (the "firmware.bin" sitting at the root is a leftover from an
//   older, 2026-08-15 WiFlash demo, most likely no longer live).
//   Following this same subdirectory pattern avoids collisions AND
//   matches the convention already established. When publishing
//   (WiFlash's tools/publish_firmware.py), --docroot needs to be given
//   as "/var/www/html/wiflash/luxflash/" (the script creates it on its
//   own if it doesn't exist yet).

#include "WiFlashApp.h"

#include <Arduino.h>

#include "Esp8285WiFi.h"
#include "Esp8285Client.h"
#include "WiFlashOta.h"
#include "WiFlashVersion.h"

// The WIFLASH_WIFI_SSID / WIFLASH_WIFI_PASSWORD macros are supplied via
// build_flags (see platformio.ini "extra_configs = wifi_secrets.ini") -
// THIS IS WHY THE PASSWORD ISN'T IN THE SOURCE CODE. If wifi_secrets.ini
// is missing or misconfigured, the build should stop right here with a
// CLEAR error message - not silently keep running with a stale
// default.
#ifndef WIFLASH_WIFI_SSID
#error "WIFLASH_WIFI_SSID is not defined - fill in wifi_secrets.ini (see platformio.ini)"
#endif
#ifndef WIFLASH_WIFI_PASSWORD
#error "WIFLASH_WIFI_PASSWORD is not defined - fill in wifi_secrets.ini (see platformio.ini)"
#endif

// The update-hosting server (plain HTTP - see doc/*/install_*.md
// "server-side setup" and the LUXFLASH note at the top of this file).
static const char HTTP_UPDATE_HOST[] = "192.168.0.2";  // the cube's IP on the "vidor" network - NOT a hostname, see the note above
static const uint16_t HTTP_UPDATE_PORT = 8090;
static const char HTTP_UPDATE_URI[] = "/luxflash/luxflash_firmware.bin";

static Esp8285Client wifiClient;

void wiflashAppSetup() {
    Serial.print("WiFlash v");
    Serial.println(WIFLASH_VERSION);
    Serial.println("Starting ESP8285 modem...");
    if (!Esp8285WiFi::begin()) {
        Serial.println("ERROR: failed to reset the modem (ATE0/CIPMUX) - skipping OTA check, current firmware continues running.");
        return;
    }

    Serial.println("Connecting to WiFi...");
    if (!Esp8285WiFi::joinAP(WIFLASH_WIFI_SSID, WIFLASH_WIFI_PASSWORD)) {
        Serial.println("ERROR: failed to connect to WiFi - skipping OTA check, current firmware continues running.");
        return;
    }
    Serial.println("WiFi connected.");

    // IMPORTANT: HTTPClient::setTimeout() only sets our client's actual
    // stream timeout if we're ALREADY connected at the time it's called -
    // but reading response headers happens AFTER connect, and WITHOUT
    // our own setTimeout call the inherited WiFiClient default (5 s)
    // would stay in effect, which occasionally proves too short for the
    // AT-protocol round trips. So we set it DIRECTLY on our object here,
    // BEFORE any HTTPClient::begin() call (and the client.clone() that
    // happens inside it) - the cloned copy inherits this value.
    wifiClient.setTimeout(15000);

    Serial.println("Checking for OTA update (once, at boot)...");
    // On a successful APPLY, this call NEVER returns (rp2040.restart()
    // takes over) - we only reach the next line on "already up to date"
    // or an error, see WiFlashOta.h WiflashOtaResult.
    WiflashOtaResult result = wiflashDownloadAndApply(wifiClient, HTTP_UPDATE_HOST, HTTP_UPDATE_PORT, HTTP_UPDATE_URI);
    if (result == WiflashOtaResult::Failed) {
        Serial.println("OTA failed - current firmware continues running, next check will only happen on restart.");
    }
    // On UpToDate there's nothing to do, current firmware continues running.
}
