# LaCrosse MQTT Gateway — ESP32 edition

[English](README.md) | [Deutsch](README.de.md)

ESP32 firmware for receiving 868 MHz LaCrosse-compatible temperature/humidity sensors with an RFM69CW, publishing readings to MQTT, and serving a local dashboard. This is an initial hardware-testing implementation; validate the RF profile against your exact Technoline model.

## Hardware and board targets

Use an ESP32 board, an **868 MHz RFM69CW**, a suitable antenna, and 3.3 V logic. Never connect 5 V logic to the radio. Join ESP32 and radio grounds.

| PlatformIO environment | Board ID | Example RFM69 wiring (SCK / MISO / MOSI / NSS / RESET / DIO0) |
|---|---|---|
| `esp32dev` | `esp32dev` | `18 / 19 / 23 / 5 / 27 / 26` |
| `esp32-c3-super-mini` | `esp32-c3-devkitm-1` | `4 / 5 / 6 / 7 / 2 / 3` |
| `esp32-s2-mini` | `lolin_s2_mini` | `12 / 13 / 11 / 10 / 9 / 8` |

These are default GPIO mappings; check the exact board revision and select supported, distinct pins in **Network / MQTT → RFM69 GPIO pins**. SCK, MOSI, NSS, and RESET must be output-capable pins. The selections are saved and applied after reboot. The `RADIO_PIN_*` flags in `platformio.ini` remain the first-boot defaults. DIO0 is mapped, although this firmware currently polls the radio over SPI.

### Build, flash, tests

Install PlatformIO and run:

```sh
pio run -e esp32dev
pio run -e esp32-c3-super-mini
pio run -e esp32-s2-mini
pio test -e esp32dev
pio run -e esp32dev -t upload
pio device monitor
```

The web UI is embedded in the firmware; no filesystem upload is required.
For a first install, use the [online USB flasher](https://wunderbaum.github.io/lacrosse-mqtt-gateway-esp32-rfm69-mqtt-dashboard/) or upload with PlatformIO. The online flasher requires a Web Serial-compatible browser, a USB data cable, and an HTTPS connection.

## Releases and online flasher

See [GitHub Releases](https://github.com/wunderbaum/lacrosse-mqtt-gateway-esp32-rfm69-mqtt-dashboard/releases) for compiled firmware. Each release includes an OTA application binary (`*-ota.bin`) for the gateway's **Firmware update** page and the bootloader, partition table, and `boot_app0` images used for USB flashing.

To publish a release, push a version tag, for example:

```sh
git tag v0.2.0
git push origin v0.2.0
```

GitHub Actions builds all three board targets, creates the release, and deploys the latest firmware to the online flasher. Tags beginning with `v0.` are marked as prereleases. The online flasher prompts whether to erase existing flash data; back up any settings you need first.

## Setup

1. On first boot, the gateway creates Wi-Fi AP `LaCrosse-Gateway`, password `lacrosse868`.
2. Join it and open `http://192.168.4.1/`.
3. Set Wi-Fi and MQTT broker details and, if needed, adjust the RFM69 GPIO pins or receive bandwidth in **Network / MQTT**, then save and reboot.
4. Open the gateway's LAN IP and use **Sensor setup** to name discovered IDs and enable or disable MQTT publishing.

The dashboard reports whether an RFM69CW was detected at startup. Detection checks that its SPI version register returns `0x24`; if not, the firmware skips radio polling. Check the module's power and SPI wiring if it is not detected.

Change the setup AP password in `src/main.cpp` before deployment. Configure an OTA update password (at least 8 characters) in **Network / MQTT**, then open **Firmware update** and upload the matching `.pio/build/<environment>/firmware.bin` file or the release's `*-ota.bin` asset. Firmware uploads require this password and reboot the gateway after a successful update.

The rest of the local web interface has no authentication. OTA password authentication uses HTTP Basic over plain HTTP, so it is not encrypted in transit. Use the interface only on a trusted network and do not expose it directly to the internet.

## MQTT and Home Assistant

State is retained at `<base-topic>/sensor/<name-slug>/state`, default base topic `lacrosse`. JSON state contains `id`, `temperature`, `humidity`, `battery_low`, `new_battery`, `rssi`, and uptime `updated`.

When MQTT connects or reconnects, the gateway publishes retained Home Assistant MQTT discovery for temperature, humidity, and low-battery entities. Discovery topics use stable sensor-ID based unique identifiers under `homeassistant/`; each sensor's three entities are grouped as one device. The gateway also publishes retained online/offline availability at `<base-topic>/status`. Disabling a sensor clears its retained discovery config topics so Home Assistant removes the entities. A rename updates the retained discovery config while retaining stable unique IDs.

Home Assistant's MQTT integration must connect to the same broker. Discovery prefix defaults to `homeassistant`.

## RF profile and limitations

The initial radio profile is FSK at 868.300 MHz, 17.241 kbit/s, 90 kHz deviation, sync word `2D D4`, fixed five-byte receive payload, and a LaCrosse-style CRC-8 polynomial `0x31`. Temperature-only frames with the LaCrosse no-humidity marker (`0x6A`) are supported; no humidity entity or value is published for those sensors. The receive bandwidth can be set to 125, 200, or 250 kHz under **Network / MQTT → RFM69 receive bandwidth**; it defaults to 250 kHz. A wider filter can help with weak or slightly off-frequency signals, but can also admit more interference. Changes apply after reboot. Decoder unit tests cover valid frames, temperature-only frames, temperature/humidity/battery decoding, and invalid CRC/header/BCD/humidity data.

Technoline is a brand covering different RF protocols. Firmware compilation and tests do not establish that a specific sensor model uses this exact radio format. Confirm reception with your hardware and share the model number or captures if the sensor is not detected. Do not treat this initial RF profile as validated for all Technoline sensors.
