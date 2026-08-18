// VENDORED (copied) file from the WiFlash project (github.com/joubertattila/WiFlash,
// v1.0.0, app/src/Esp8285Client.h) - UNCHANGED content. See doc/LuxFlash_terv.md.

// WiFlash - a single-TCP-connection Arduino Client subclass for the
// ESP8285 AT modem (raw CIPSTART/CIPSEND/+IPD, since AT firmware
// v1.6.2.0 has no AT+HTTPCLIENT - see doc/*/intro_*.md).
//
// This class knows nothing about connecting to the WiFi network (that's
// handled once, in setup(), by Esp8285WiFi::begin()+joinAP()) - it can
// only open/manage/close a single TCP connection.
//
// IMPORTANT: the HTTPUpdate/HTTPClient library declares HTTPUpdate.update()
// and HTTPClient.begin() with a parameter type of "WiFiClient&"
// specifically, NOT the general "Client&" - in C++ a plain Client
// subclass (that isn't also a WiFiClient) cannot be passed where a
// WiFiClient& is declared. That's why Esp8285Client inherits directly
// from WiFiClient, and overrides EVERY virtual method that HTTPClient
// actually calls (connect/write/read/available/peek/flush/stop/connected)
// - WiFiClient's own (lwIP-based) internal state (_client = nullptr)
// simply stays untouched/unused this way. This isn't a hack - it's
// exactly how WiFiClientSecure does it too, which is why every relevant
// method in the framework is virtual.
//
// The trickiest part is handling inbound data: the ESP8285 AT firmware
// inserts TCP-received data into the UART stream as
// "+IPD,<length>:<raw bytes>", interleaved with everything ELSE too -
// this has to be recognized byte by byte with a simple state machine
// (ScanForIpd -> ReadLength -> CopyPayload), because the "+IPD," header
// and the payload itself can interrupt the UART data stream at an
// ARBITRARY point, even across multiple read()/available() calls (see
// Esp8285Client.cpp pollIncoming()/processIncomingByte()).
//
// IMPORTANT: HTTPClient::begin(WiFiClient&, ...) does NOT use the object
// we pass it directly - it first calls the (also virtual) clone() on it
// ("_clientIn = client.clone();"), and from THEN ON uses the resulting
// copy for every connect/write/read call. If clone() weren't overridden,
// the base WiFiClient::clone() would run, which returns a PLAIN
// WiFiClient instance (not an Esp8285Client!) - this would lead to a
// silent failure: HTTPUpdate would try to connect on the real (here,
// nonexistent) CYW43-based WiFiClient, which would fail immediately, with
// no AT traffic at all. That's why clone() is overridden too.

#ifndef WIFLASH_ESP8285_CLIENT_H
#define WIFLASH_ESP8285_CLIENT_H

#include <Arduino.h>
#include <WiFiClient.h>
#include <IPAddress.h>
#include <memory>

class Esp8285Client : public WiFiClient {
public:
    Esp8285Client();

    std::unique_ptr<WiFiClient> clone() const override;

    int connect(IPAddress ip, uint16_t port) override;
    int connect(const char *host, uint16_t port) override;

    size_t write(uint8_t b) override;
    size_t write(const uint8_t *buf, size_t size) override;

    int available() override;
    int read() override;
    int read(uint8_t *buf, size_t size) override;
    int peek() override;
    void flush() override;

    void stop() override;
    uint8_t connected() override;
    operator bool() override { return connected(); }
    int availableForWrite() override;

private:
    // --- Outbound (TX) side ---
    // write() calls get buffered, and only actually turn into an
    // AT+CIPSEND command once the buffer fills up, or the caller
    // explicitly requests a flush() (which we also call ourselves before
    // waiting on a read()/available() response) - so e.g. several small
    // print() calls building an HTTP request's headers do NOT each
    // generate a separate AT+CIPSEND.
    static const size_t TX_BUF_SIZE = 512;
    uint8_t _txBuf[TX_BUF_SIZE];
    size_t _txLen = 0;

    // A single AT+CIPSEND call sends at most this many bytes at once
    // (the ESP8266/85 AT firmware typically allows up to ~2048 bytes per
    // AT+CIPSEND - we use a smaller, more conservative value here).
    static const size_t TX_CHUNK_MAX = 1024;

    bool cipSendChunk(const uint8_t *data, size_t len);

    // --- Inbound (RX) side ---
    // A simple, linear (not ring) buffer for payload bytes that have
    // already arrived but haven't been read by the caller yet.
    //
    // IMPORTANT, found during hardware testing: Update.writeStream()
    // reads its own internal buffer (4096 bytes) in LARGE CHUNKS
    // (readBytes), then writes a flash page to LittleFS after EACH such
    // chunk - and during that time (while flash is being written),
    // nothing calls our read()/available(), so nothing drains either the
    // hardware UART FIFO or this buffer. If that "silence" lasts longer
    // than it takes for incoming data on the 115200-baud UART to fill
    // the buffer, bytes get lost, and the +IPD state machine
    // irrecoverably desyncs. That's why this buffer is much larger than
    // the seemingly-needed 4096 bytes (the Updater's own internal buffer
    // size) - a generous margin for one or two flash-write cycles worth
    // of stall.
    static const size_t RX_BUF_SIZE = 16384;
    uint8_t _rxBuf[RX_BUF_SIZE];
    size_t _rxHead = 0;  // index of the next byte to read
    size_t _rxTail = 0;  // index of the next writable slot

    size_t rxAvailable() const { return _rxTail - _rxHead; }
    void rxCompact();  // discard already-read bytes, freeing space at the start of the buffer

    // --- +IPD state machine ---
    enum class IpdState {
        ScanForIpd,     // looking for a "+IPD," header (or a "CLOSED" line)
        ReadLength,     // reading the decimal length up to the ':'
        CopyPayload     // copying the payload bytes into the RX buffer
    };
    IpdState _ipdState = IpdState::ScanForIpd;
    uint8_t _matchPos = 0;          // how many characters of "+IPD," have matched so far
    uint32_t _payloadLen = 0;       // length being built up during ReadLength
    uint32_t _payloadRemaining = 0; // bytes remaining during CopyPayload

    char _lineBuf[16];   // for recognizing the "CLOSED" line (while in ScanForIpd)
    uint8_t _lineLen = 0;
    bool _lineOverflowed = false;  // see the comment on processIncomingByte() in Esp8285Client.cpp
    bool _lastWasCR = false;       // was the previous byte '\r' (needed to require a "\r\n" line ending)

    bool _connected = false;

    // Reads and processes all Serial1 bytes that are IMMEDIATELY
    // available (non-blocking - returns right away if there's no data).
    // HTTPUpdate expects this pattern: available()/read() must not block
    // for long.
    void pollIncoming();
    void processIncomingByte(uint8_t c);
};

#endif // WIFLASH_ESP8285_CLIENT_H
