// VENDORED (copied) file from the C++/RFLink/WFLink_test project
// (HC12Link.h) - UNCHANGED content, see doc/LuxFlash_terv.md. The
// class/variable names still reference "hc12" (it originates from the
// HC-12-based RFLink_test) - this is just naming, it doesn't affect
// behavior: the 'hc12' pointer's type is Stream*, which a WiFiClient
// (or, in LuxFlash's case, Esp8285ServerClient) works with just as well
// as an actual HC-12 radio.

#ifndef HC12LINK_H // Same include-guard protection as in Config.h.
#define HC12LINK_H
#include <Arduino.h>
// No separate SoftwareSerial.h include: the 'hc12' pointer's type is
// Stream*, the common Arduino API ancestor of both the AVR-side
// SoftwareSerial and the Pico-side SerialPIO - so this file works on
// both platforms without any changes (this was the original goal: an
// MCU-independent communication layer).

// Command codes (Cube -> Device)
// '#define' is an instruction to the compiler: wherever it sees these
// names in the code, it substitutes the value that follows.
// The '0x' prefix denotes hexadecimal (base-16) notation, which is very
// common in data communication.
#define CMD_SET_DIGITAL  0x01 // Set a digital output (e.g. turn a relay or LED on/off)
#define CMD_SET_PWM      0x02 // Set a PWM (pulse-width modulation) signal
#define CMD_READ_ADC     0x03 // Read the value of an analog input
#define CMD_READ_DIGITAL 0x04 // Read the state of a digital input
#define CMD_READ_SUPPLY  0x05  // raw ADC (internal supply-voltage measurement), PIN field is padding
#define CMD_READ_TEMP    0x06  // raw ADC (internal temperature measurement), PIN field is padding
#define CMD_READ_BME_TEMP     0x07  // BME280 temperature (signed Celsius*100), PIN field is padding
#define CMD_READ_BME_HUMID    0x08  // BME280 humidity (%*100), PIN field is padding
#define CMD_READ_BME_PRESSURE 0x09  // BME280 air pressure (hPa*10), PIN field is padding
#define CMD_READ_RTC_YEAR     0x0A  // DS3231 RTC year (e.g. 2026), PIN field is padding
#define CMD_READ_RTC_MONTH    0x0B  // DS3231 RTC month (1-12), PIN field is padding
#define CMD_READ_RTC_DAY      0x0C  // DS3231 RTC day (1-31), PIN field is padding
#define CMD_READ_RTC_HOUR     0x0D  // DS3231 RTC hour (0-23), PIN field is padding
#define CMD_READ_RTC_MINUTE   0x0E  // DS3231 RTC minute (0-59), PIN field is padding
#define CMD_READ_RTC_SECOND   0x0F  // DS3231 RTC second (0-59), PIN field is padding

// RTC (DS3231) time set - fire-and-forget SET commands, same as
// SET_DIGITAL/SET_PWM (no response to them). Since the VALUE field is
// only 1 byte (0-255), the year can't be sent raw (e.g. 2026) - the
// YEAR FIELD EXCEPTIONALLY travels over the wire as an (year-2000)
// offset, the Pico adds the 2000 back. The DS3231/RTClib only reliably
// handles the 2000-2099 range anyway, so this isn't a real limitation.
// The other 5 fields (month/day/hour/minute/second) travel as raw,
// unencoded values, same as the corresponding READ_RTC_* commands'
// responses.
// The Pico stores the 5 fields arriving up through SECOND (year/month/
// day/hour/minute) in individual static variables, and only applies the
// ENTIRE date/time to the RTC (rtc.adjust) ALL AT ONCE, once the SECOND
// command arrives - so SECOND MUST be sent LAST from the Python side.
// DELAY COMPENSATION (2026-08-03): the Pico measures the time elapsed
// between the YEAR (first) and SECOND (last) commands, due to the radio
// transfer, using its OWN (not yet overwritten) RTC clock, and adds the
// difference to the time being set - see the detailed explanation at
// RFLink_test.ino's rtcSetStartUnixtime variable.
#define CMD_SET_RTC_YEAR       0x10  // VALUE = (year - 2000), PIN field is padding
#define CMD_SET_RTC_MONTH      0x11  // VALUE = month (1-12), PIN field is padding
#define CMD_SET_RTC_DAY        0x12  // VALUE = day (1-31), PIN field is padding
#define CMD_SET_RTC_HOUR       0x13  // VALUE = hour (0-23), PIN field is padding
#define CMD_SET_RTC_MINUTE     0x14  // VALUE = minute (0-59), PIN field is padding
#define CMD_SET_RTC_SECOND     0x15  // VALUE = second (0-59) - THIS APPLIES the full date/time to the RTC, see RFLink_test.ino

#define CMD_READ_EARTH_TEMP    0x16  // DS18B20 soil temperature (signed Celsius*100), PIN field is padding

#define HC12_START_BYTE 0xAA // Every valid data packet starts with this byte (this is the synchronization marker).
#define HC12_FRAME_SIZE 6   // Fixed length of a data packet, in bytes: [0xAA][DEVICE_ID][CMD][PIN][VALUE][CRC]

// Data of a received, validated command
// A 'struct' (structure) groups several variables, possibly of
// different types, into a single logical package.
struct HC12Command {
    uint8_t cmd;     // The command code (e.g. 0x01 is CMD_SET_DIGITAL)
    uint8_t pin;     // The affected pin number on the microcontroller
    uint8_t value;   // The value to set (e.g. 1/0 for digital, or 0-255 for PWM)
    bool broadcast;  // 'bool' is a logical type (true/false). True if it arrived addressed to BROADCAST_ID - must NOT be answered, to avoid response collisions.
};

// A 'class' is the foundation of object-oriented programming: it
// bundles data together with the functions that operate on it.
class HC12Link {
private: // Variables and functions here are hidden, only accessible from within the class.
    Stream* hc12; // Pointer (the '*' marks it) to the serial-port object - any Stream descendant (SoftwareSerial, SerialPIO, HardwareSerial) works.
    uint8_t deviceId;     // This device's own identifier.
    uint8_t broadcastId;  // The "addressed to everyone" address.
    uint8_t buffer[HC12_FRAME_SIZE]; // A 6-element array (buffer) where incoming bytes are collected.

    // Computes a checksum (CRC) to filter out possible radio transmission errors.
    uint8_t calculateChecksum(uint8_t* data, uint8_t length) {
        uint8_t crc = 0;
        for (uint8_t i = 0; i < length; i++) {
            // '^=' is a bitwise XOR (exclusive or) operation.
            // It XORs all the bytes together; the result is the checksum.
            crc ^= data[i];
        }
        return crc;
    }

public: // Functions here can also be called from the main program.

    // This function prepares (initializes) the system for operation with the given parameters.
    void init(Stream* radio, uint8_t deviceIdIn, uint8_t broadcastIdIn) {
        hc12 = radio;
        deviceId = deviceIdIn;
        broadcastId = broadcastIdIn;
        memset(buffer, 0, HC12_FRAME_SIZE); // memset() fills the buffer with zeros, so we start with a clean slate.
    }

    // Sliding-window reception (tuned for 6 bytes).
    // Returns true if a valid command addressed to this device (or a
    // broadcast) has arrived, and fills in the 'out' struct.
    bool checkForCommand(HC12Command* out) {
        while (hc12->available()) { // While there is unread data waiting on the radio serial port...
            uint8_t b = hc12->read(); // ...read a single byte.

            // "Sliding window" mechanism: shift every byte so far one position to the left (forward) in the array...
            for (uint8_t i = 0; i < HC12_FRAME_SIZE - 1; i++) {
                buffer[i] = buffer[i + 1];
            }
            // ...and place the newly read byte at the very end of the array.
            buffer[HC12_FRAME_SIZE - 1] = b;

            // If the first byte is the start byte (0xAA), and the second byte is either our own address or the broadcast address:
            if (buffer[0] == HC12_START_BYTE &&
                (buffer[1] == deviceId || buffer[1] == broadcastId)) {

                // Check data integrity: if the checksum computed from the content matches the last byte sent:
                if (calculateChecksum(buffer, HC12_FRAME_SIZE - 1) == buffer[HC12_FRAME_SIZE - 1]) {
                    // Then the command is valid! Save the data into the 'out' struct passed to the function.
                    out->cmd = buffer[2];
                    out->pin = buffer[3];
                    out->value = buffer[4];
                    out->broadcast = (buffer[1] == broadcastId); // Logical evaluation: true if it matches.

                    memset(buffer, 0, HC12_FRAME_SIZE); // Clear the buffer for the next message.
                    return true; // Signal to the main program that a valid command arrived.
                }
            }
        }
        return false; // If there is no complete, processable command, return false.
    }

    // Sends a response to a READ_* command. cmdEcho is the triggering command's code (0x03/0x04),
    // data is the 16-bit raw value (split into DATA_HI/DATA_LO).
    void sendResponse(uint8_t cmdEcho, uint16_t data) {
        uint8_t outBuffer[HC12_FRAME_SIZE]; // Build the array of outgoing data.
        outBuffer[0] = HC12_START_BYTE;
        outBuffer[1] = deviceId; // Send our own identifier, so the controller knows who it's from.
        outBuffer[2] = cmdEcho;  // Echo back the original command code.

        // Since 'data' is a 16-bit number (uint16_t), but we can only send 8-bit bytes, it needs to be split in two:
        outBuffer[3] = (data >> 8) & 0xFF; // The '>> 8' bit shift brings down the upper 8 bits, the '& 0xFF' guarantees truncation.
        outBuffer[4] = data & 0xFF;        // And this extracts the number's lower 8 bits via masking.

        outBuffer[5] = calculateChecksum(outBuffer, HC12_FRAME_SIZE - 1); // Compute the checksum of our message too.

        // Send the assembled 6 bytes out over the radio port.
        for (uint8_t i = 0; i < HC12_FRAME_SIZE; i++) {
            hc12->write(outBuffer[i]);
        }
        hc12->flush(); // Wait until the data has guaranteed, physically left the microcontroller.
    }
};
#endif
