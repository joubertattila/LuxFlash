# LuxFlash

A real-world example of combining [WiFlash](https://github.com/joubertattila/WiFlash)
(a WiFi-based, self-healing OTA firmware updater for RP2040 + ESP8285
boards) with an actual sensor application: a light-intensity sensor
(BPW34 photodiode + MCP6002 transimpedance amplifier) and the Pico's
internal CPU temperature, both queried over WiFi and logged to a MariaDB
database with a small Chart.js dashboard.

LuxFlash is a **separate, standalone project** - it is not a fork of
WiFlash and does not replace it. It reuses WiFlash's OTA modules
(vendored - copied in, with clear provenance comments) on top of its own
application logic, the same way any real WiFlash-based product would.

**Status: hardware-tested, working.** The board has been validated
end-to-end, including receiving a brand-new feature (the CPU
temperature reading) purely over-the-air, with no USB cable involved at
any point after the initial flash.

## What it demonstrates

- **WiFlash's OTA update, unmodified**, running alongside real
  application logic - exactly the way WiFlash's own demo `main.cpp`
  anticipates ("this file can be freely replaced with real application
  logic").
- **A light-sensor query server** (`Esp8285Server`), built on the same
  low-level AT-command driver as WiFlash's own OTA client, avoiding a
  real linker conflict that was hit trying to add a second WiFi library
  (WiFiEspAT) alongside WiFlash's client-only driver. See
  [doc/LuxFlash_terv.md](doc/LuxFlash_terv.md) (Hungarian - kept as the
  original design-decision log, the way WiFlash's own `doc/history/`
  folder is also intentionally left untranslated) for the full story,
  including the AT-modem quirks and retries that real hardware testing
  uncovered.
- **A tiny binary protocol** (6-byte frames, XOR checksum) reused
  unchanged from an earlier HC-12-radio-based project, now carried over
  WiFi/TCP instead - showing that a transport-independent protocol layer
  (built on `Stream*`) really does port cleanly to a new transport.
- **A Python client with retries** and a **PHP/Chart.js dashboard**
  reading the same database.
- **An LED status indicator** (lit during the OTA check, and again while
  a sensor query is being served) - a small, practical way to see what a
  headless board is doing without a Serial Monitor.

## Hardware

- RP2040 "Pico W-2023" clone board (ESP8285 AT WiFi modem, **not** the
  official CYW43-based Pico W) - same board WiFlash targets.
- A BPW34 photodiode feeding an MCP6002 dual op-amp: stage 1 is a
  transimpedance amplifier (photodiode held at a virtual ground for good
  linearity, feedback resistor value tuned to the sensor's light range -
  see the schematic for the exact value), stage 2 is a unity-gain buffer
  into the RP2040's ADC (GP27).

Schematic: [doc/LuxFlash_Schematic.png](doc/LuxFlash_Schematic.png) -
full KiCad project in [doc/LuxFlash_KiCAD.zip](doc/LuxFlash_KiCAD.zip).

## Directory layout

```
LuxFlash/
├── README.md              <- this file
├── LICENSE                 <- GPL-3.0 (see "License" below)
├── doc/
│   ├── LuxFlash_terv.md      <- design-decision log (Hungarian, historical)
│   ├── LuxFlash_Schematic.png <- light-sensor circuit schematic
│   └── LuxFlash_KiCAD.zip     <- full KiCad project for the schematic
├── app/                    <- PlatformIO firmware project
│   ├── platformio.ini
│   ├── wifi_secrets.ini.example
│   └── src/
│       ├── main.cpp             <- application logic (start here)
│       ├── Config.h              <- new, combined config
│       ├── HC12Link.h            <- vendored from an earlier project (not WiFlash), unchanged
│       ├── Esp8285Server.h/.cpp   <- NEW: the light-sensor query TCP server
│       ├── Esp8285WiFi.*          <- vendored from WiFlash, unchanged
│       ├── Esp8285Client.*        <- vendored from WiFlash, unchanged
│       ├── WiFlashOta.*           <- vendored from WiFlash, unchanged
│       ├── WiFlashApp.*           <- vendored from WiFlash (HTTP_UPDATE_HOST/PORT/URI filled in)
│       ├── WiFlashVersion.h       <- vendored from WiFlash, unchanged
│       └── WiFlashSigningKey.h    <- vendored from WiFlash, unchanged (public key)
├── LuxFlash_py/             <- Python query client
│   ├── config.py.example
│   ├── wflink_link.py         <- protocol layer (TCP transport)
│   └── light_test.py          <- queries the board, writes to MariaDB
└── php/                     <- read-only dashboard
    ├── db_config.php.example
    └── light_measures.php
```

## Quick start

1. **Firmware**: in `app/`, copy `wifi_secrets.ini.example` to
   `wifi_secrets.ini` and fill in your WiFi SSID/password. Review
   `WiFlashApp.cpp`'s `HTTP_UPDATE_HOST`/`PORT`/`URI` (your own OTA
   server - see WiFlash's own docs for setting one up). Build and
   upload over USB with PlatformIO (`pio run -e pico -t upload`).
2. **Find the IP**: the Serial Monitor (115200 baud) prints the board's
   IP address at boot.
3. **Python client**: in `LuxFlash_py/`, copy `config.py.example` to
   `config.py`, fill in the board's IP and your database credentials,
   then run `python light_test.py`.
4. **Dashboard** (optional): in `php/`, copy `db_config.php.example` to
   `db_config.php`, fill in your database credentials, and serve
   `light_measures.php` from any PHP-capable web server.
5. **OTA updates, from then on**: publish a new firmware build with
   WiFlash's `tools/publish_firmware.py` and simply power-cycle the
   board - no USB cable needed again.

## Relationship to WiFlash

LuxFlash vendors (copies) WiFlash's OTA source files rather than
depending on it as a proper library, because WiFlash's own `app/src`
isn't currently structured as an installable library (its demo
`main.cpp` lives in the same folder as the reusable modules). Each
vendored file carries a comment naming its origin and WiFlash version,
should you want to check for upstream fixes.

## License

GNU General Public License v3.0 - see [LICENSE](LICENSE). This project
includes source code from [WiFlash](https://github.com/joubertattila/WiFlash),
also GPL-3.0 licensed. Anyone is free to use, modify and redistribute
this project, provided that any distributed modified/derivative version
also makes its source code available, under the same license.
