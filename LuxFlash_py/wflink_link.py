import socket  # Built-in module needed for network (TCP) communication - the WiFi counterpart of the serial module.
import time    # For timing and timeout handling.

# This file is the WiFi counterpart of RFLink_test_py/hc12_link.py: it
# speaks the EXACT SAME 6-byte [0xAA][DEVICE_ID][CMD][PIN][VALUE][CRC]
# protocol as HC12Link.h/hc12_link.py - just over a TCP connection
# instead of a serial port. The Pico-side WFLink_test.ino/LuxFlash
# accepts EXACTLY ONE command per incoming connection, sends back the
# response (for READ_* commands), then closes the connection - so here
# too, every call opens a new TCP connection and closes it afterwards,
# instead of keeping one permanent connection open (the way the serial
# port stayed permanently open for the HC-12).
#
# CMD_READ_ADC (light sensor) and CMD_READ_TEMP (Pico CPU temperature)
# are implemented - the latter follows the same pattern as
# RFLink_test_py/hc12_link.py's read_temp() (a shared
# _send_and_receive() helper, the same way there's a shared
# _send_command() there too). The other command codes (SET_DIGITAL,
# BME280, RTC, etc.) can be found in HC12Link.h's header, if you want to
# extend the WiFi device further later.

START_BYTE = 0xAA
FRAME_SIZE = 6  # [0xAA][DEVICE_ID][CMD][PIN][VALUE][CRC]

CMD_READ_ADC = 0x03
CMD_READ_TEMP = 0x06  # the Pico sends this back as Celsius*100 (centi-degrees), not a raw ADC value - see main.cpp's readTempRaw()

# During hardware testing, the AT modem (ESP8285) repeatedly showed
# occasional, transient stubbornness (see LuxFlash/doc/LuxFlash_terv.md's
# debugging sections - e.g. the CIPSERVER startup also needed a retry
# added on the Pico side). The same kind of phenomenon can show up here,
# on the client side, as a random timeout/refusal on an individual
# query, even when the board is otherwise completely fine - so we handle
# it with a simple retry before giving up.
RETRY_COUNT = 3
RETRY_DELAY_S = 0.5


def _checksum(data):
    # Same bitwise XOR sum as in hc12_link.py and HC12Link.h - all three
    # implementations MUST always match.
    crc = 0
    for b in data:
        crc ^= b
    return crc


class WFLink:
    def __init__(self, host, port, response_timeout_s, debug=False):
        self.host = host
        self.port = port
        self.response_timeout_s = response_timeout_s
        self.debug = debug

    def _read_response(self, sock, expected_cmd, deadline):
        # Sliding-window reception, same as in hc12_link.py - here we
        # read from a socket with recv() instead of the serial port's
        # read().
        buffer = bytearray()

        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            sock.settimeout(remaining)
            try:
                chunk = sock.recv(1)
            except socket.timeout:
                break
            if not chunk:
                break  # the Pico closed the connection

            buffer += chunk
            if len(buffer) > FRAME_SIZE:
                buffer = buffer[-FRAME_SIZE:]  # keep only the last 6 bytes (sliding window)

            if self.debug:
                print("  [debug] bytes received so far:", buffer.hex())

            if len(buffer) == FRAME_SIZE and buffer[0] == START_BYTE:
                if _checksum(buffer[:FRAME_SIZE - 1]) == buffer[FRAME_SIZE - 1]:
                    device_id = buffer[1]
                    cmd_echo = buffer[2]
                    data = (buffer[3] << 8) | buffer[4]
                    if cmd_echo == expected_cmd:
                        return {"device_id": device_id, "cmd_echo": cmd_echo, "data": data}

        return None

    def _send_and_receive_once(self, device_id, cmd, pin=0x00, value=0x00):
        # A single attempt (open connection, send frame, receive
        # response, close connection) - see the central retry logic in
        # _send_and_receive() below.
        frame = bytearray([START_BYTE, device_id, cmd, pin, value])
        frame.append(_checksum(frame))

        deadline = time.monotonic() + self.response_timeout_s

        # Every call opens a new TCP connection - the Pico expects this
        # too (server.accept() -> one command -> client.stop()).
        sock = socket.create_connection((self.host, self.port), timeout=self.response_timeout_s)
        try:
            sock.sendall(frame)
            if self.debug:
                print("  [debug] frame sent:", frame.hex())

            return self._read_response(sock, cmd, deadline)
        finally:
            sock.close()

    def _send_and_receive(self, device_id, cmd, pin=0x00, value=0x00):
        # Retry wrapper around _send_and_receive_once() above - see the
        # explanation next to RETRY_COUNT/RETRY_DELAY_S at the top of
        # this file. We catch OSError here (e.g. ConnectionRefusedError,
        # if the board happens to be restarting/applying an OTA update)
        # and treat it as a retry-worthy failure too, instead of letting
        # it stop the calling code (e.g. light_test.py) with a raw
        # Python traceback.
        for attempt in range(RETRY_COUNT):
            try:
                result = self._send_and_receive_once(device_id, cmd, pin=pin, value=value)
            except OSError as e:
                result = None
                if self.debug:
                    print(f"  [debug] network error (attempt {attempt + 1}): {e}")

            if result is not None:
                return result

            if attempt < RETRY_COUNT - 1:
                if self.debug:
                    print(f"  [debug] no response (attempt {attempt + 1}), retrying...")
                time.sleep(RETRY_DELAY_S)

        return None

    def read_adc(self, device_id, pin):
        return self._send_and_receive(device_id, CMD_READ_ADC, pin=pin)

    def read_temp(self, device_id):
        return self._send_and_receive(device_id, CMD_READ_TEMP)
