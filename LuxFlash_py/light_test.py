import pymysql  # MariaDB/MySQL client - the Python counterpart of light_measures.php's mysqli.

import config
from wflink_link import WFLink

LIGHT_ADC_PIN = 27  # ADC1 (GPIO27) - same pin as in ESP8285_WebServer_Test


def log_to_database(adc_raw, v_adc, cpu_temp_c):
    # Table schema (light_measures): id (auto), timestamp (auto,
    # defaulting to current_timestamp()), adc, voltage, cpu_voltage
    # (nullable), cpu_temp_c (nullable). We don't provide id or
    # timestamp - the database fills them in on its own.
    #
    # cpu_temp_c is now actually filled in (see below, CMD_READ_TEMP).
    # cpu_voltage STILL stays NULL - CMD_READ_SUPPLY (VSYS/GPIO29) is
    # DELIBERATELY not implemented on the Pico side in LuxFlash: this
    # measurement is disabled in RFLink_test.ino, because the author
    # measured with a multimeter that on this board family, the
    # VSYS/GPIO29 pin carries roughly 5V directly (there is no real
    # voltage divider), which exceeds the RP2040 ADC's ~3.3V safe limit
    # and could damage the chip - see the comment before readTempRaw()
    # in main.cpp.
    conn = pymysql.connect(**config.DB_CONFIG)
    try:
        with conn.cursor() as cursor:
            cursor.execute(
                "INSERT INTO light_measures (adc, voltage, cpu_temp_c) VALUES (%s, %s, %s)",
                (adc_raw, v_adc, cpu_temp_c),
            )
        conn.commit()
    finally:
        conn.close()


def main():
    link = WFLink(config.WFLINK_HOST, config.WFLINK_PORT, config.RESPONSE_TIMEOUT_S)

    print(f"READ_ADC <- (LIGHT, pin={LIGHT_ADC_PIN}) @ {config.WFLINK_HOST}:{config.WFLINK_PORT}")
    response = link.read_adc(config.DEVICE_ID, LIGHT_ADC_PIN)

    if response is None:
        print("  no response (timeout or failed to connect)")
        return

    adc_raw = response["data"]
    v_adc = (adc_raw / config.ADC_MAX_VAL) * config.PICO_ADC_REF_V

    print(f"  raw = {adc_raw} / {config.ADC_MAX_VAL}")
    print(f"  V_adc (on GPIO{LIGHT_ADC_PIN}) = {v_adc:.3f} V")

    print("READ_TEMP <- (Pico CPU temperature)")
    temp_response = link.read_temp(config.DEVICE_ID)

    cpu_temp_c = None
    if temp_response is None:
        # We don't abort the save because of this - the light reading is
        # valuable on its own, cpu_temp_c simply stays NULL in the
        # database in this case.
        print("  no response (timeout) - cpu_temp_c will stay NULL")
    else:
        # The Pico already sends it as Celsius*100 (centi-degrees), see
        # main.cpp's readTempRaw() - no need for separate calibration/
        # offset like the old AVR-based RFLink_test.
        cpu_temp_c = temp_response["data"] / 100.0
        print(f"  CPU temperature = {cpu_temp_c:.1f} C")

    log_to_database(adc_raw, v_adc, cpu_temp_c)
    print("  saved to the database (light_measures)")


if __name__ == "__main__":
    main()
