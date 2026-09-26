# ClimaCarta

ESP-IDF firmware for a low-power environmental sensor based on the Waveshare
ESP32-C6-ePaper-1.54 board.

ClimaCarta measures temperature and humidity, stores historical data in flash,
shows readings on an e-paper display, and publishes data over MQTT with
automatic Home Assistant integration.

## Features

- SHTC3 environmental sensor;
- temperature, relative humidity, and dew point;
- configurable sampling through 't_sample' (900 seconds by default);
- hourly, daily, weekly, monthly, and yearly aggregates;
- daily minimum and maximum temperature;
- CRC-protected circular flash history;
- PCF85063 RTC synchronized through NTP;
- e-paper display with status bar, environmental data, historical deltas, and charts;
- history page selection with a short BOOT/GP9 press;
- Wi-Fi and MQTT provisioning through SoftAP, QR code, and captive portal;
- USB Serial/JTAG command console;
- MQTT transmission every 'update_freq * t_sample';
- MQTT command reception;
- Home Assistant MQTT auto-discovery;
- deep sleep with timer and Power-button wake-up;
- temporary active window when connected to a USB host;
- battery percentage calculated from the measured constant-load discharge table,
  interpolated between 2810 mV (0%) and 4160 mV (100%);
- green LED on EXIO4 turned off before deep sleep.

## Supported hardware

- [Waveshare ESP32-C6-ePaper-1.54](https://www.waveshare.com/esp32-c6-epaper-1.54.htm)
- [Waveshare documentation](https://docs.waveshare.com/ESP32-C6-ePaper-1.54/Resources-And-Documents)
- [Official ESP-IDF examples](https://docs.waveshare.com/ESP32-C6-ePaper-1.54/ESP-IDF)
- [Official Waveshare repository](https://github.com/waveshareteam/ESP32-C6-ePaper-1.54)

The board includes a 1.54-inch e-paper display, SHTC3, PCF85063 RTC,
ETA6098 charger, TCA9554 I/O expander, and USB-C connector.

### Pin and device map

| Function | Connection |
| --- | --- |
| I2C SDA / SCL | GPIO18 / GPIO8 |
| SHTC3 | I2C 0x70 |
| PCF85063 | I2C 0x51 |
| TCA9554 | I2C 0x20 |
| e-paper MOSI / SCLK / CS | GPIO5 / GPIO6 / GPIO7 |
| e-paper DC / BUSY / RESET | GPIO15 / GPIO10 / GPIO11 |
| Battery ADC | ADC1 CH0, GPIO0, 1:2 divider |
| Power button / BOOT | GPIO2 / GPIO9 |
| Green LED | TCA9554 EXIO4, active low |

The board schematic is available in the [doc](doc/) directory. The charger
STAT signal is handled by the hardware. USB VBUS is not connected to a
dedicated microcontroller input; host presence is detected through USB
Serial/JTAG.

## Configuration

ESP-IDF 5.5 or later is required. The project is verified with ESP-IDF 6.1
for the ESP32-C6 target.

~~~powershell
idf.py set-target esp32c6
idf.py menuconfig
~~~

The **RHT sensor configuration** menu provides Wi-Fi credentials, MQTT URI and
credentials, base topic, Home Assistant device ID, NTP server, POSIX timezone,
't_sample', 'update_freq', SHTC3 temperature correction, and NTP resynchronization
interval.

The default broker is 'mqtt://192.168.2.99:1883'. Values saved through
provisioning or the console are stored in NVS and take precedence over values
compiled into 'sdkconfig'.

## Smartphone provisioning

1. Hold BOOT/GP9 for approximately five seconds.
2. Connect to the 'RHT-xxxxxx' SoftAP network.
3. Scan the QR code shown on the e-paper display or open the captive portal.
4. Enter the Wi-Fi SSID and password, MQTT broker, and MQTT credentials.
5. The device verifies the Wi-Fi connection and stores the configuration in NVS.

The broker field is pre-filled with 'mqtt://192.168.2.99:1883', but it can be
changed through the portal.

## USB console

When a USB host is connected, open the Serial/JTAG port at 115200 baud with
'idf.py monitor'. The console supports:

~~~text
help
show
wifi set "Wi-Fi name" "Wi-Fi password"
wifi clear
mqtt set mqtt://192.168.2.99:1883 username password
mqtt clear
reboot
~~~

Passwords are never displayed. The 'clear' commands remove values stored in
NVS and allow compiled defaults to be used again.

## Measurement cycle

1. The device wakes from the timer or the Power button.
2. It reads the RTC, SHTC3, and battery voltage.
3. It updates the current hourly accumulator.
4. When the hour changes, it stores the completed hourly record in the 'stats' partition.
5. It updates longer-period aggregates.
6. If required by 'update_freq', it connects to Wi-Fi and publishes MQTT data.
7. It refreshes the display.
8. It enters deep sleep, except for the console window available while a USB host is connected.

'update_freq=0' and 'update_freq=1' both transmit every sample. With
'update_freq=3', for example, transmission occurs every three samples.

A short Power-button press during the active window forces deep sleep. A short
GP9 press changes the history page; holding GP9 for approximately five seconds
starts provisioning.

## MQTT and Home Assistant

With the default configuration:

~~~text
state:     homeassistant/sensor/rht_epaper/state
discovery: homeassistant/sensor/rht_epaper/<entity>/config
commands:  homeassistant/sensor/rht_epaper/command
~~~

The retained QoS 1 state JSON contains temperature, humidity, dew point, daily
minimum and maximum, battery percentage and voltage, RSSI, plus values and
deltas for the previous hour, the same hour on the previous day, and the
previous week, month, and year.

Discovery messages are published after the first successful MQTT connection,
after new provisioning, and at least every 24 hours thereafter. Historical
periods that are not available yet are published as 'null'.

## Build and flash

~~~powershell
idf.py build
idf.py -p COMx flash monitor
~~~

If the board contains firmware with an incompatible partition table, it may be
necessary to run the following command once:

~~~powershell
idf.py -p COMx erase-flash
~~~

This also erases NVS credentials and the stored history.

## FOTA through GitHub Releases

The firmware supports HTTPS FOTA from public GitHub Releases using two OTA
application slots and rollback protection. The default manifest URL is
`https://github.com/zer0g/climacarta/releases/latest/download/manifest.json`.

Each release must contain `rht_sensor.bin` and a `manifest.json` asset (see
`ota/manifest.json.example`):

~~~json
{
  "version": "1.0.0",
  "url": "https://github.com/zer0g/climacarta/releases/download/v1.0.0/rht_sensor.bin",
  "sha256": "64_lowercase_or_uppercase_hex_characters"
}
~~~

Build the firmware and calculate its digest with
`Get-FileHash build/rht_sensor.bin -Algorithm SHA256`. Upload the binary to the
release, fill in the digest, then upload the manifest as `manifest.json`. The
device verifies the manifest and image over HTTPS, checks SHA-256, switches
slots, and confirms the new image after reboot.

Publish `ota_install` (or `{"command":"ota_install"}`) to the configured MQTT
command topic to request an update. `ota_check` is accepted for forward
compatibility and does not install an image.

The first firmware using the dual-slot layout requires a full erase before
flashing because the partition table changes. This erases measurements and
provisioning data; configure Wi-Fi and MQTT again afterwards.

~~~text
Home Assistant --MQTT--> ESP32-C6 --HTTPS--> GitHub Release
~~~

## License

Copyright (C) 2026 Zer0G

This project is licensed under the GNU General Public License, version 3 or
later (GPL-3.0-or-later). See [LICENSE](LICENSE) for the full text.
