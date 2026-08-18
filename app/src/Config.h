// LuxFlash - NEW, combined configuration (not a vendored file). Takes
// over the contents of C++/RFLink/WFLink_test/Config.h unchanged
// (WiFlash itself has no such Config.h - it uses wifi_secrets.ini's
// build_flags instead, under the names WIFLASH_WIFI_SSID/PASSWORD - see
// WiFlashApp.cpp).

#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// Device addressing - IMPORTANT: this is a SEPARATE address range from
// the HC-12 device fleet's (RFLink_test/Config.h, DEVICE_ID=1 there).
// Since this Pico talks over WiFi, on its own TCP port, there is no
// address-collision risk with the HC-12 fleet - but to avoid confusion
// we don't just copy the same ID without thinking about it.
//
// LUXFLASH: same value (1) as in WFLink_test/Config.h - DELIBERATELY,
// so that light_test.py/config.py do NOT need to be modified (see
// doc/LuxFlash_terv.md "Python-oldal: nincs teendo" / "Python side: no
// changes needed").
const uint8_t DEVICE_ID    = 1;      // The first (and currently only) WiFi device.
const uint8_t BROADCAST_ID = 0xFF;   // Same convention as in the HC-12 protocol.

// TCP port on which the Pico accepts commands (the "cube server" will
// connect to this, occasionally via cron, the same way it used to poll
// over HC-12).
const uint16_t WFLINK_PORT = 3000;

// ADC resolution (Raspberry Pi Pico: 12-bit -> 0-4095), same convention
// as in the HC-12-based Config.h. IMPORTANT: this requires
// analogReadResolution(12) to be called in main.cpp's setup() (this was
// missing from WFLink_test.ino - see doc/LuxFlash_terv.md, fixed here).
const float ADC_MAX_VAL = 4095.0;

#endif
