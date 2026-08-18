// VENDORED (copied) file from the WiFlash project (github.com/joubertattila/WiFlash,
// v1.0.0, app/src/WiFlashVersion.h) - UNCHANGED content. See doc/LuxFlash_terv.md.
// IMPORTANT: this is the WiFlash FRAMEWORK's version, NOT the LuxFlash
// application's - the latter doesn't have its own separate version number yet.

// WiFlash - human-readable project version.
//
// This is INDEPENDENT of the OTA system's own, MD5-based version
// tracking (WiFlashOta.cpp "current.md5") - that only answers "is this
// the same as what the server offers", with no human-meaningful version
// number. This macro, on the other hand, gets printed to the serial log
// at boot (see WiFlashApp.cpp), so a single glance at a connected board
// tells you which WiFlash release it's running - useful for bug reports
// or when running several boards at once.
//
// Bump this on release (see doc/*/upload_*.md), and note what changed in
// doc/*/intro_*.md (or a CHANGELOG).

#ifndef WIFLASH_VERSION_H
#define WIFLASH_VERSION_H

#define WIFLASH_VERSION "1.0.0"

#endif // WIFLASH_VERSION_H
