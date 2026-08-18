// VENDORED (copied) file from the WiFlash project (github.com/joubertattila/WiFlash,
// v1.0.0, app/src/WiFlashApp.h) - UNCHANGED content. See doc/LuxFlash_terv.md.

// WiFlash - ties WiFi connection + OTA update check together into a
// single function callable from setup().
//
// IMPORTANT DESIGN DECISION: the OTA check does NOT run continuously,
// polling in the background alongside loop() - it runs only ONCE, in
// setup(), at power-on/restart. Reason: a continuously (e.g. every 30
// seconds) polling version would erratically disrupt the timing of the
// actual application logic (e.g. LED blinking, sensor reading) -
// Esp8285WiFi::isJoined()'s AT+CWJAP? query can occasionally take a
// multi-second timeout, which would stall everything else in loop() too.
// To check again whether an update is available, the board needs to be
// restarted - in exchange, loop() is guaranteed to run undisturbed from
// there on, with no network calls.
//
// The actual low-level work is done by Esp8285WiFi (modem + AP
// connection) and WiFlashOta (chunked downloader) - this file just ties
// them together into a function callable from setup(), which main.cpp
// calls.
//
// LUXFLASH NOTE: here, main.cpp calls this wiflashAppSetup() FIRST, and
// then - because of the design decision above (WiFlash's own AT driver
// is client-only, it can't open a server) - switches the modem into
// CIPMUX=1 with the new Esp8285Server module so it can run the
// light-sensor-query TCP server in loop(). See doc/LuxFlash_terv.md.

#ifndef WIFLASH_APP_H
#define WIFLASH_APP_H

// Call once, from setup(): resets the modem, connects to WiFi, then runs
// a SINGLE OTA check/apply. On error (no modem/WiFi, or a failed
// download) it just prints a Serial message and the board keeps running
// its current firmware - a restart is needed for the next check.
void wiflashAppSetup();

#endif // WIFLASH_APP_H
