# ESP32 iLO4 Fan Controller

A standalone ESP32 web app + physical control panel for managing custom HP
iLO4 fan speed profiles over SSH — create and edit profiles from a browser,
then apply them with a rotary-encoder control panel that applies the profile
over SSH **and verifies the fans actually reached the requested speed**.

## Features

- Web UI served directly from the ESP32 (connects to your WiFi, or runs its
  own configuration access point if no WiFi is set up yet)
- Persistent WiFi and iLO4 credentials (DHCP or static IP)
- iLO4 SSH connection test, with compatibility options for iLO4's older
  SSH key exchange / cipher suites
- Full profile manager: create, rename, delete, and edit up to 20 profiles,
  each with its own six fan max-speed percentages (0-100%)
- 1.3" 128x64 I2C OLED + EC11 rotary encoder physical control panel:
  - Browse profiles one at a time (rotate to move, click to select)
  - Apply / Back submenu per profile
  - Applying a profile sends the correct `fan p <n> max <value>` SSH
    command per fan (converting the UI's 0-100% to iLO4's raw 0-255 scale)
  - **Verification**: after applying, it polls each fan's actual reported
    speed for up to 10 seconds and confirms it matches the profile's target
    (within a small tolerance) before reporting Success/Failed
  - Fans reported as not installed (`HealthState=Not Installed`) are
    automatically ignored rather than causing a false failure
  - Auto-dimming screensaver after 10s of inactivity, wakes instantly on
    any rotary/button input

## Hardware

| Component | Pin |
|---|---|
| OLED SDA | GPIO21 |
| OLED SCL | GPIO22 |
| EC11 CLK | GPIO32 |
| EC11 DT  | GPIO33 |
| EC11 SW  | GPIO25 |

Tested on a plain ESP32 WROOM-32D with a 1.3" SH1106-based 128x64 I2C OLED
and a bare EC11 rotary encoder (5-pin: A/C/B for the rotary contacts, plus
the separate push-button switch).

> If your OLED shows a vertical line artifact / wraps a column of pixels to
> the wrong edge, your panel is probably the other controller family
> (SSD1306 vs SH1106) — swap the `U8g2` constructor in the sketch to match.

## Required libraries

Install these via the Arduino IDE Library Manager:

- `WiFi` (bundled with the ESP32 board package)
- `WebServer` (bundled with the ESP32 board package)
- `Preferences` (bundled with the ESP32 board package)
- [`libssh_esp32`](https://github.com/ewpa/libssh-esp32)
- `U8g2` (by olikraus)

Board package: **esp32 by Espressif Systems** (tested on 3.3.11).

## Setup

1. Flash `esp32_ilo4_fan_controller/esp32_ilo4_fan_controller.ino` to your
   ESP32.
2. On first boot (no WiFi configured yet), connect to the **ESP32-iLO4**
   access point and open `http://192.168.4.1` to set your WiFi credentials.
3. Once on your network, open the ESP32's IP address in a browser to
   configure your iLO4 host/username/password and build fan profiles.
4. Use the OLED + rotary panel on the device itself to browse and apply
   profiles without needing the web UI.

## Notes on iLO4 command behavior

- Apply command: `fan p <fan 1-6> max <raw 0-255>` — the web UI stores and
  displays 0-100%, converted to iLO4's raw scale before sending.
- Verify/read command: `show /system1/fan<n>` — the speed iLO4 reports back
  here is already a 0-100% value (no conversion needed on this side).
- These were confirmed against a real iLO4 unit during development, but
  firmware output can vary — if verification behaves oddly on your unit,
  enable Serial Monitor at 115200 baud; every apply/verify step logs the
  exact command sent and the raw response received.

## License

MIT — see [LICENSE](LICENSE).
