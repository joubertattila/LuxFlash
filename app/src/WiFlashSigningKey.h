// VENDORED (copied) file from the WiFlash project (github.com/joubertattila/WiFlash,
// v1.0.0, app/src/WiFlashSigningKey.h) - UNCHANGED content, see doc/LuxFlash_terv.md.
// This is the PUBLIC key, the same one other WiFlash-based devices
// (e.g. PicoMaster) also use - the matching PRIVATE key
// (~/Keys/wiflash_signing_private.pem) does NOT go here, or into any
// synced folder at all - it only ever lives on the publishing machine.

// WiFlash - PUBLIC RSA key used for signature verification.
//
// This key is NOT secret - the public key is embedded in the firmware
// precisely so it can VERIFY the signature of a downloaded update (see
// WiFlashOta.cpp). The matching PRIVATE key must be generated separately
// and kept OUTSIDE this repository and any cloud-synced folder - see
// doc/*/install_*.md section 5 ("Generating the signing key pair") for
// the exact commands and where to store it. Only the publishing script
// (tools/publish_firmware.py) ever needs to read the private key.
//
// If the key ever needs to be rotated (e.g. the private key is suspected
// to be compromised), compile in a NEW public key here, and reflash ALL
// affected boards over USB (BOOTSEL) - this cannot be done over WiFi OTA,
// since a "new" firmware signed with the old (assumed-compromised)
// private key would be checked by the board against its CURRENTLY
// running, old public key, which would expectedly turn out INVALID. See
// doc/*/upload_*.md section 7 ("Rotating the signing key") for details.

#ifndef WIFLASH_SIGNING_KEY_H
#define WIFLASH_SIGNING_KEY_H

#include <pgmspace.h>

static const char WIFLASH_SIGNING_PUBLIC_KEY_PEM[] PROGMEM =
    "-----BEGIN PUBLIC KEY-----\n"
    "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuqdPk9la+PVz5qlAdcDU\n"
    "3V1Eu3ALfa0dZQtmnKlS1Jo7XG3xZuAHztE5Zm62SnAd9cRrOj/fQ4Bm3C2nHXoj\n"
    "bNseSN234KqyCMmThRgKf+Xc2cWjHBaCGz7mDSqsnSLAOidA328HLSANjbmFwnc3\n"
    "v3efuYd7wXijFmMob7BWxWllMDx1luxnx65dVIjkOY7pm6cDWDnUmabNEwuCfqvO\n"
    "RchCDAFIoVpys4GKuZnhn4BxhAuqplEtVROqjYOPkZh6Anu1JG8XifDNECyalFl5\n"
    "dCUv8rwOfxgQHNOqqx9wds7kONUnisddGiG9WBAsUpZF1iScAYAXzTdz594/VV5V\n"
    "wQIDAQAB\n"
    "-----END PUBLIC KEY-----\n";

#endif // WIFLASH_SIGNING_KEY_H
