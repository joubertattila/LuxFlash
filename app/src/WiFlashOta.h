// VENDORED (copied) file from the WiFlash project (github.com/joubertattila/WiFlash,
// v1.0.0, app/src/WiFlashOta.h) - UNCHANGED content. See doc/LuxFlash_terv.md.

// WiFlash - chunked (HTTP Range-based), reconnecting OTA downloader.
//
// WHY IS THIS NEEDED instead of the built-in HTTPUpdate.update()?
// Hardware testing revealed that the ESP8285 AT modem (v1.6.2.0
// firmware) cannot reliably carry a single, continuous TCP connection
// through the full download of a ~140 KB firmware - the connection drops
// on its own at a variable point (observed at roughly 28-78 KB in),
// most likely due to a limit of the modem itself/WiFi-radio timing
// jitter, NOT a bug in our own code (we confirmed this: the "CLOSED"
// string that appears is actually present, verbatim, in the downloaded
// binary file, but ELSEWHERE, nowhere near the 28-78 KB range - so it's
// not a false match in our own state machine).
//
// The solution: download the file in smaller (BLOCK_SIZE) chunks, each
// over its OWN TCP connection, requested with an HTTP "Range: bytes=X-Y"
// header, and verified against a per-block CRC32 precomputed by
// WiFlash_py/publish_firmware.py - if a chunk gets interrupted OR is
// corrupt (the raw UART connection can occasionally cause a silent bit
// error too), only THAT chunk needs to be retried, not the whole thing.
// Apache (confirmed with curl -I) natively supports Range requests
// (Accept-Ranges: bytes), no server-side changes needed.
//
// The actual flash apply step does NOT happen here: the downloaded bytes
// are written to a plain LittleFS file ("firmware.bin"), then the
// picoOTA class is used to perform the same "set the OTA command +
// restart" step that Update.end() also does internally (see
// framework-arduinopico Updater.cpp: picoOTA.begin()+addFile()+commit())
// - so the actual flash write/apply goes through the same proven, tested
// path as the built-in mechanism, we've only rewritten the DOWNLOAD
// logic.
//
// VERSION TRACKING: before every successful apply, we save (into a
// separate "current.md5" file on LittleFS - NOT the "firmware.bin"
// staging file, which gets overwritten on every download attempt) which
// MD5 we applied. The next call FIRST compares this against the
// server's current .md5 sidecar file - if they match, nothing gets
// downloaded, it just returns UpToDate. Without this, the caller
// (main.cpp's setup(), via WiFlashApp) would re-download and re-apply
// the same firmware on every single check, pointlessly.
//
// CRYPTOGRAPHIC SIGNATURE VERIFICATION: the per-block CRC32 and the
// whole-file MD5 only filter out ACCIDENTAL data corruption - not a
// DELIBERATE swap, since the .md5/.crc32 sidecar files are plain,
// unprotected text files on the server (anyone could recompute them if
// they replaced the .bin). That's why a third check, an RSA-2048/SHA256
// signature verification, also runs before the final apply (the
// downloaded ".sig" sidecar, checked with BearSSL::SigningVerifier,
// against the public key embedded in WiFlashSigningKey.h) - the
// picoOTA.commit() is only called if the signature is valid. On an
// invalid signature (just like on a CRC32/MD5 error), we simply return
// Failed - the board keeps running its current, not-yet-replaced (and
// therefore trusted) firmware.

#ifndef WIFLASH_OTA_H
#define WIFLASH_OTA_H

#include "Esp8285Client.h"

enum class WiflashOtaResult {
    UpToDate,  // the version on the server matches what's currently running - nothing was downloaded
    Applied,   // in PRACTICE this is never actually returned to the caller: after a
               // successful download+verification, the OTA command was set and
               // rp2040.restart() runs, which does not return
    Failed,    // an error occurred during download/verification (CRC32/MD5/signature) -
               // it's up to the caller to decide when to retry (e.g. after a delay in loop())
};

WiflashOtaResult wiflashDownloadAndApply(Esp8285Client &client, const char *host, uint16_t port, const char *uri);

#endif // WIFLASH_OTA_H
