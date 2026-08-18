// VENDORED (copied) file from the WiFlash project (github.com/joubertattila/WiFlash,
// v1.0.0, app/src/WiFlashOta.cpp) - UNCHANGED content. See doc/LuxFlash_terv.md.

// See WiFlashOta.h for the full design rationale (why not the built-in
// HTTPUpdate, why a chunked download, why signature verification is
// needed).
#include "WiFlashOta.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <PicoOTA.h>
#include <MD5Builder.h>
#include <BearSSLHelpers.h>

#include "WiFlashSigningKey.h"

namespace {

// The actual block size is read at RUNTIME from the FIRST line of the
// .crc32 file (see wiflashDownloadAndApply() "step 3") - not from a
// constant kept manually in sync here. This is only a SAFETY LIMIT: if
// CRC_BLOCK_SIZE in publish_firmware.py ever grew past this, this would
// need to grow too (the blockBuf stack buffer has a fixed size because
// of it) - but the COMMON case (the Python-side value changes within
// this limit) works on its own, with no C++ change needed.
const size_t MAX_BLOCK_SIZE = 8192;

// Fixed length of an RSA-2048 signature, in bytes (PKCS1v1.5 - the size
// of the modulus). See WiFlashSigningKey.h and publish_firmware.py
// "sign_file()".
const size_t RSA_SIGNATURE_LEN = 256;

// If no new data arrives within a request for this long (while the
// connection is technically still "connected"), we treat it as a stalled
// transfer and abort the attempt - whatever arrived up to that point is
// kept.
const uint32_t STALL_TIMEOUT_MS = 5000;

// If this many CONSECUTIVE attempts bring back absolutely no new data (0
// bytes of "progress") WITHIN a single block, we give up on that block -
// this distinguishes a "slow but working" connection from a genuinely
// unreachable server/network.
const int MAX_NO_PROGRESS_ATTEMPTS = 10;
const uint32_t RETRY_DELAY_MS = 1000;

// A block is retried at most this many times (including CRC-failed
// attempts) before we give up on it entirely.
const int MAX_BLOCK_RETRIES = 8;

// Standard CRC32 (the same polynomial/convention as Python's
// zlib.crc32()) - this is what lets publish_firmware.py and this code
// compute compatible checksums.
uint32_t crc32(const uint8_t *data, size_t len) {
    static uint32_t table[256];
    static bool tableReady = false;
    if (!tableReady) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        tableReady = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

// Attempts to download AT MOST maxLen bytes starting at offset, directly
// into the `dst` RAM buffer (NOT flash/LittleFS - a block only makes it
// into a file once its CRC32 matches, see downloadBlock()). Returns how
// many bytes were ACTUALLY received - this can be 0, and doesn't
// necessarily reach maxLen: any amount of partial data is fine, the
// caller keeps it and continues from there with a fresh connection.
size_t fetchSome(Esp8285Client &client, const char *host, uint16_t port, const char *uri,
                  size_t offset, size_t maxLen, uint8_t *dst) {
    HTTPClient http;
    if (!http.begin(client, host, port, uri)) {
        Serial.println("[OTA] ERROR: http.begin() failed");
        return 0;
    }

    char rangeHeader[48];
    snprintf(rangeHeader, sizeof(rangeHeader), "bytes=%u-%u",
             (unsigned)offset, (unsigned)(offset + maxLen - 1));
    http.addHeader("Range", String(rangeHeader));

    int code = http.GET();
    // 206 = Partial Content (the expected response). 200 could only
    // happen if the server ignored the Range header.
    if (code != 206) {
        Serial.print("[OTA] Unexpected HTTP code: ");
        Serial.println(code);
        http.end();
        return 0;
    }

    int announcedLen = http.getSize();
    if (announcedLen <= 0 || announcedLen > (int)maxLen) {
        Serial.print("[OTA] Unexpected announced size: ");
        Serial.println(announcedLen);
        http.end();
        return 0;
    }

    WiFiClient *stream = http.getStreamPtr();
    size_t received = 0;
    size_t wantLen = (size_t)announcedLen;
    uint32_t lastDataMs = millis();

    while (received < wantLen) {
        int avail = stream->available();
        if (avail > 0) {
            size_t toRead = wantLen - received;
            if ((size_t)avail < toRead) {
                toRead = (size_t)avail;
            }
            int n = stream->read(dst + received, toRead);
            if (n > 0) {
                received += (size_t)n;
                lastDataMs = millis();
            }
        } else if (!http.connected()) {
            // The modem reported "CLOSED", or the connection dropped for
            // some other reason - this is an expected case observed
            // during WiFlash development (see WiFlashOta.h). Whatever
            // arrived up to this point is kept.
            break;
        } else if (millis() - lastDataMs > STALL_TIMEOUT_MS) {
            break;
        }
    }

    http.end();
    return received;
}

// Downloads ONE block (range [blockStart, blockStart+blockLen)), and
// CHECKS its CRC32 against the expected value BEFORE writing it to
// flash/LittleFS. If the CRC doesn't match, the WHOLE block is DISCARDED
// and retried - see the comment at the top of this file: this guards
// against the occasional, silent byte-level corruption that can happen
// on the raw UART connection, on a per-block basis (not the whole file
// at once).
bool downloadBlock(Esp8285Client &client, const char *host, uint16_t port, const char *uri,
                    size_t blockStart, size_t blockLen, uint32_t expectedCrc, uint8_t *blockBuf) {
    for (int blockAttempt = 0; blockAttempt < MAX_BLOCK_RETRIES; blockAttempt++) {
        if (blockAttempt > 0) {
            Serial.print("[OTA] Retrying block (offset=");
            Serial.print((unsigned)blockStart);
            Serial.print(", ");
            Serial.print(blockAttempt + 1);
            Serial.print("/");
            Serial.print(MAX_BLOCK_RETRIES);
            Serial.println(")");
        }

        size_t received = 0;
        int noProgressCount = 0;
        while (received < blockLen) {
            size_t n = fetchSome(client, host, port, uri,
                                  blockStart + received, blockLen - received,
                                  blockBuf + received);
            if (n > 0) {
                received += n;
                noProgressCount = 0;
            } else {
                noProgressCount++;
                if (noProgressCount >= MAX_NO_PROGRESS_ATTEMPTS) {
                    Serial.println("[OTA] ERROR: too many failed attempts within one block.");
                    break;
                }
                delay(RETRY_DELAY_MS);
            }
        }

        if (received != blockLen) {
            // Didn't manage to fully receive the block - the OUTER loop
            // (blockAttempt) retries from scratch.
            continue;
        }

        uint32_t actualCrc = crc32(blockBuf, blockLen);
        if (actualCrc != expectedCrc) {
            Serial.print("[OTA] CRC mismatch in a block (offset=");
            Serial.print((unsigned)blockStart);
            Serial.print("): expected=");
            Serial.print(expectedCrc, HEX);
            Serial.print(" got=");
            Serial.println(actualCrc, HEX);
            continue;  // discard it, retry the whole block
        }

        return true;  // block received successfully and the CRC matches
    }

    return false;
}

// Downloads the small "<uri>.md5" or "<uri>.crc32" text sidecar file
// into a single String. IMPORTANT: DELIBERATELY does not use
// http.getString() - it turned out its internal StreamSendSize() helper
// reads byte by byte, and gives up the FIRST moment a byte isn't
// IMMEDIATELY available - that's incompatible with our slow AT data
// stream, which arrives in +IPD chunks. We use our own, patient read
// loop instead.
String fetchTextFile(Esp8285Client &client, const char *host, uint16_t port, const String &uri,
                      size_t maxExpectedLen) {
    HTTPClient http;
    if (!http.begin(client, host, port, uri)) {
        Serial.print("[OTA] ERROR: failed to connect: ");
        Serial.println(uri);
        return "";
    }
    int code = http.GET();
    if (code != 200) {
        Serial.print("[OTA] ERROR: query HTTP code (");
        Serial.print(uri);
        Serial.print("): ");
        Serial.println(code);
        http.end();
        return "";
    }

    int announcedLen = http.getSize();
    if (announcedLen <= 0 || (size_t)announcedLen > maxExpectedLen) {
        Serial.print("[OTA] ERROR: unexpected size (");
        Serial.print(uri);
        Serial.print("): ");
        Serial.println(announcedLen);
        http.end();
        return "";
    }

    WiFiClient *stream = http.getStreamPtr();
    String result;
    result.reserve(announcedLen + 1);
    size_t received = 0;
    uint32_t lastDataMs = millis();
    while (received < (size_t)announcedLen) {
        int avail = stream->available();
        if (avail > 0) {
            int c = stream->read();
            if (c >= 0) {
                result += (char)c;
                received++;
                lastDataMs = millis();
            }
        } else if (!http.connected()) {
            break;
        } else if (millis() - lastDataMs > STALL_TIMEOUT_MS) {
            Serial.print("[OTA] ERROR: timeout while reading response: ");
            Serial.println(uri);
            break;
        }
    }
    http.end();
    return result;
}

// Downloads a small, PRE-KNOWN-SIZE binary file (used for the signature
// file) into a fixed-size buffer. Similar to fetchTextFile() (a single,
// Range-less GET, since the file is small, well below the ~28-80 KB
// range where downloads tend to get interrupted - see the comment at the
// top of this file), but writes raw bytes into a uint8_t buffer instead
// of a String - a signature's binary content would get CORRUPTED if
// treated as a String (e.g. String's usual trim() would strip off bytes
// that happen to have whitespace values).
size_t fetchBinaryFile(Esp8285Client &client, const char *host, uint16_t port, const String &uri,
                        uint8_t *dst, size_t expectedLen) {
    HTTPClient http;
    if (!http.begin(client, host, port, uri)) {
        Serial.print("[OTA] ERROR: failed to connect: ");
        Serial.println(uri);
        return 0;
    }
    int code = http.GET();
    if (code != 200) {
        Serial.print("[OTA] ERROR: query HTTP code (");
        Serial.print(uri);
        Serial.print("): ");
        Serial.println(code);
        http.end();
        return 0;
    }

    int announcedLen = http.getSize();
    if (announcedLen != (int)expectedLen) {
        Serial.print("[OTA] ERROR: unexpected size (");
        Serial.print(uri);
        Serial.print("): ");
        Serial.println(announcedLen);
        http.end();
        return 0;
    }

    WiFiClient *stream = http.getStreamPtr();
    size_t received = 0;
    uint32_t lastDataMs = millis();
    while (received < expectedLen) {
        int avail = stream->available();
        if (avail > 0) {
            size_t toRead = expectedLen - received;
            if ((size_t)avail < toRead) {
                toRead = (size_t)avail;
            }
            int n = stream->read(dst + received, toRead);
            if (n > 0) {
                received += (size_t)n;
                lastDataMs = millis();
            }
        } else if (!http.connected()) {
            break;
        } else if (millis() - lastDataMs > STALL_TIMEOUT_MS) {
            Serial.print("[OTA] ERROR: timeout while reading response: ");
            Serial.println(uri);
            break;
        }
    }
    http.end();
    return received;
}

// Name of the LittleFS file that holds the MD5 of the currently applied
// (running) firmware. DELIBERATELY not "firmware.bin" (that's the
// download's temporary staging file, overwritten on every attempt) -
// this is a SEPARATE, persistent marker that survives the OTA apply (the
// bootloader only reads and writes "firmware.bin" to flash, it doesn't
// touch the rest of LittleFS).
const char *CURRENT_VERSION_FILE = "current.md5";

String readCurrentVersion() {
    File f = LittleFS.open(CURRENT_VERSION_FILE, "r");
    if (!f) {
        return "";  // no successful OTA has ever happened on this board yet
    }
    String result = f.readString();
    f.close();
    result.trim();
    result.toLowerCase();
    return result;
}

bool writeCurrentVersion(const String &md5) {
    File f = LittleFS.open(CURRENT_VERSION_FILE, "w");
    if (!f) {
        return false;
    }
    size_t written = f.print(md5);
    f.close();
    return written == md5.length();
}

}  // namespace

WiflashOtaResult wiflashDownloadAndApply(Esp8285Client &client, const char *host, uint16_t port, const char *uri) {
    if (!LittleFS.begin()) {
        Serial.println("[OTA] ERROR: LittleFS.begin() failed");
        return WiflashOtaResult::Failed;
    }

    // Step 1: fetch the expected MD5 - BEFORE even querying the full
    // download size, so that the "no new firmware" case is as cheap as
    // possible (only this one small request runs, nothing else).
    String expectedMd5 = fetchTextFile(client, host, port, String(uri) + ".md5", 64);
    expectedMd5.trim();
    expectedMd5.toLowerCase();
    if (expectedMd5.length() != 32) {
        Serial.println("[OTA] ERROR: failed to fetch a valid .md5, aborting.");
        return WiflashOtaResult::Failed;
    }

    String currentVersion = readCurrentVersion();
    if (currentVersion == expectedMd5) {
        Serial.print("[OTA] Already running the latest version (MD5 ");
        Serial.print(expectedMd5);
        Serial.println("), nothing to do.");
        return WiflashOtaResult::UpToDate;
    }
    Serial.print("[OTA] Expected MD5: ");
    Serial.print(expectedMd5);
    Serial.print(" (current: ");
    Serial.print(currentVersion.length() ? currentVersion : "unknown");
    Serial.println(") - starting update.");

    // Step 1b: fetch the signature - again BEFORE the EXPENSIVE
    // block-by-block download, so that if it's missing/invalid, that's
    // discovered cheaply (see the comment on step 1). The ACTUAL
    // verification can only happen AFTER the full file has been
    // downloaded (SHA256 needs the whole content) - in step 6b - but we
    // fetch the bytes already here.
    uint8_t signatureBuf[RSA_SIGNATURE_LEN];
    size_t signatureLen = fetchBinaryFile(client, host, port, String(uri) + ".sig", signatureBuf, RSA_SIGNATURE_LEN);
    if (signatureLen != RSA_SIGNATURE_LEN) {
        Serial.println("[OTA] ERROR: failed to fetch a valid .sig signature, aborting.");
        return WiflashOtaResult::Failed;
    }

    // Step 2: query the total file size with a plain (Range-less)
    // request - we don't read the response body, we close the
    // connection right away, then download in chunks afterwards.
    size_t totalSize = 0;
    {
        HTTPClient http;
        if (!http.begin(client, host, port, uri)) {
            Serial.println("[OTA] ERROR: begin() failed for the size query");
            return WiflashOtaResult::Failed;
        }
        int code = http.GET();
        if (code != 200) {
            Serial.print("[OTA] ERROR: size query HTTP code: ");
            Serial.println(code);
            http.end();
            return WiflashOtaResult::Failed;
        }
        totalSize = (size_t)http.getSize();
        http.end();
    }

    if (totalSize == 0) {
        Serial.println("[OTA] ERROR: server reported size 0");
        return WiflashOtaResult::Failed;
    }
    Serial.print("[OTA] Total size: ");
    Serial.println((unsigned)totalSize);

    // Step 3: fetch the per-block CRC32 list. Its FIRST line is the
    // block size (see WiFlash_py/publish_firmware.py's ".crc32" comment)
    // - we read this out, rather than relying on a constant kept
    // manually in sync here. The file's size is roughly numBlocks * 9
    // bytes (8 hex digits + '\n') - requested with a generous margin
    // (e.g. up to 2000 blocks, good for firmware up to ~1 MB).
    String crcList = fetchTextFile(client, host, port, String(uri) + ".crc32", 2000 * 9 + 16);
    if (crcList.length() == 0) {
        Serial.println("[OTA] ERROR: failed to fetch the .crc32 list, aborting.");
        return WiflashOtaResult::Failed;
    }

    int crcLineStart = crcList.indexOf('\n');
    if (crcLineStart < 0) {
        Serial.println("[OTA] ERROR: invalid .crc32 list (missing block-size line).");
        return WiflashOtaResult::Failed;
    }
    String blockSizeStr = crcList.substring(0, crcLineStart);
    blockSizeStr.trim();
    size_t blockSize = (size_t)strtoul(blockSizeStr.c_str(), nullptr, 10);
    crcLineStart += 1;  // the CRC32 lines start after the block-size line

    if (blockSize == 0 || blockSize > MAX_BLOCK_SIZE) {
        Serial.print("[OTA] ERROR: invalid or too large block size in the .crc32 list: ");
        Serial.println((unsigned)blockSize);
        return WiflashOtaResult::Failed;
    }

    size_t numBlocks = (totalSize + blockSize - 1) / blockSize;

    // Step 4: prepare LittleFS, "firmware.bin" as a fresh (empty) file.
    {
        File f = LittleFS.open("firmware.bin", "w");
        if (!f) {
            Serial.println("[OTA] ERROR: failed to create the firmware.bin file");
            return WiflashOtaResult::Failed;
        }
        f.close();
    }

    // Step 5: per-block download with CRC32 verification - a block only
    // makes it into the file if its checksum matches, see
    // downloadBlock(). blockBuf is sized according to the MAX_BLOCK_SIZE
    // safety limit, but only blockSize bytes of it are actually used.
    uint8_t blockBuf[MAX_BLOCK_SIZE];
    MD5Builder md5;
    md5.begin();
    for (size_t blockIndex = 0; blockIndex < numBlocks; blockIndex++) {
        size_t blockStart = blockIndex * blockSize;
        size_t blockLen = blockSize;
        if (blockStart + blockLen > totalSize) {
            blockLen = totalSize - blockStart;
        }

        int lineEnd = crcList.indexOf('\n', crcLineStart);
        if (lineEnd < 0) {
            lineEnd = crcList.length();
        }
        String crcHex = crcList.substring(crcLineStart, lineEnd);
        crcHex.trim();
        crcLineStart = lineEnd + 1;
        if (crcHex.length() != 8) {
            Serial.print("[OTA] ERROR: incomplete .crc32 list at block ");
            Serial.print((unsigned)blockIndex);
            Serial.println(".");
            return WiflashOtaResult::Failed;
        }
        uint32_t expectedCrc = (uint32_t)strtoul(crcHex.c_str(), nullptr, 16);

        if (!downloadBlock(client, host, port, uri, blockStart, blockLen, expectedCrc, blockBuf)) {
            Serial.print("[OTA] ERROR: block ");
            Serial.print((unsigned)blockIndex);
            Serial.println(" could not be downloaded validly even after repeated attempts.");
            return WiflashOtaResult::Failed;
        }

        File wf = LittleFS.open("firmware.bin", "r+");
        if (!wf) {
            Serial.println("[OTA] ERROR: failed to open firmware.bin for writing");
            return WiflashOtaResult::Failed;
        }
        wf.seek(blockStart);
        size_t written = wf.write(blockBuf, blockLen);
        wf.close();
        if (written != blockLen) {
            Serial.println("[OTA] ERROR: short flash write for a verified block.");
            return WiflashOtaResult::Failed;
        }
        md5.add(blockBuf, (uint16_t)blockLen);

        Serial.print("[OTA] Block OK (");
        Serial.print((unsigned)(blockIndex + 1));
        Serial.print("/");
        Serial.print((unsigned)numBlocks);
        Serial.print("), progress: ");
        Serial.print((unsigned)(blockStart + blockLen));
        Serial.print(" / ");
        Serial.println((unsigned)totalSize);
    }

    // Step 6: final size and MD5 verification (after the per-block
    // CRC32, this is just a double-check, but it's cheap and reassuring).
    size_t finalSize = 0;
    {
        File verify = LittleFS.open("firmware.bin", "r");
        if (verify) {
            finalSize = verify.size();
            verify.close();
        }
    }
    if (finalSize != totalSize) {
        Serial.print("[OTA] ERROR: final file size mismatch (");
        Serial.print((unsigned)finalSize);
        Serial.print(" != ");
        Serial.print((unsigned)totalSize);
        Serial.println(")");
        return WiflashOtaResult::Failed;
    }

    md5.calculate();
    String actualMd5 = md5.toString();
    Serial.print("[OTA] Computed MD5: ");
    Serial.println(actualMd5);
    if (actualMd5 != expectedMd5) {
        Serial.println("[OTA] ERROR: final MD5 mismatch (despite per-block CRC32) - NOT applying.");
        return WiflashOtaResult::Failed;
    }
    Serial.println("[OTA] MD5 matches.");

    // Step 6b: cryptographic signature verification - the per-block
    // CRC32 and the whole-file MD5 only filter out ACCIDENTAL data
    // corruption, not a DELIBERATE swap (the .md5/.crc32 sidecar files
    // are plain, unprotected text files on the server). This step
    // verifies that the firmware was ACTUALLY signed with our matching
    // private key (see WiFlashSigningKey.h) - only AFTER this do we call
    // picoOTA.commit(). We reuse the blockBuf buffer (the per-block
    // download is already done, its contents aren't needed anymore) -
    // no need to allocate another MAX_BLOCK_SIZE-sized stack buffer.
    {
        BearSSL::HashSHA256 hash;
        hash.begin();
        File hf = LittleFS.open("firmware.bin", "r");
        if (!hf) {
            Serial.println("[OTA] ERROR: failed to open firmware.bin for signature verification");
            return WiflashOtaResult::Failed;
        }
        int n;
        while ((n = hf.read(blockBuf, MAX_BLOCK_SIZE)) > 0) {
            hash.add(blockBuf, (uint32_t)n);
        }
        hf.close();
        hash.end();

        BearSSL::PublicKey pubKey(WIFLASH_SIGNING_PUBLIC_KEY_PEM);
        if (!pubKey.isRSA()) {
            Serial.println("[OTA] ERROR: the embedded public key is not a valid RSA key");
            return WiflashOtaResult::Failed;
        }
        BearSSL::SigningVerifier verifier(&pubKey);
        if (!verifier.verify(&hash, signatureBuf, signatureLen)) {
            Serial.println("[OTA] ERROR: signature INVALID - NOT applying.");
            return WiflashOtaResult::Failed;
        }
        Serial.println("[OTA] Signature valid.");
    }

    // Step 7: save the version marker BEFORE the bootloader applies the
    // update - if this failed silently, the NEXT boot's loop() wouldn't
    // know it's already running this version, and would needlessly
    // re-download it - that's why we return an error here (not
    // afterwards, since afterwards the NEW firmware would already be
    // running) if it can't be saved reliably.
    if (!writeCurrentVersion(expectedMd5)) {
        Serial.println("[OTA] ERROR: failed to save the version marker, aborting.");
        return WiflashOtaResult::Failed;
    }

    // Step 8: set up and apply the OTA command - the same mechanism that
    // Update.end() also uses internally (see framework-arduinopico
    // Updater.cpp: picoOTA.begin()+addFile()+commit()).
    Serial.println("[OTA] Setting up the OTA command...");
    picoOTA.begin();
    picoOTA.addFile("firmware.bin");
    if (!picoOTA.commit()) {
        Serial.println("[OTA] ERROR: picoOTA.commit() failed");
        return WiflashOtaResult::Failed;
    }

    Serial.println("[OTA] Restarting to apply the new firmware...");
    delay(200);
    rp2040.restart();
    return WiflashOtaResult::Applied;  // in principle never reached - rp2040.restart() does not return
}
