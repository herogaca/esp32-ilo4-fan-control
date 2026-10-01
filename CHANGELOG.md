# Changelog

## v1.0.0 — Initial release

- Web UI for WiFi/iLO4 configuration and fan profile management (up to 20
  profiles, 6 fans per profile, 0-100%)
- iLO4 SSH connection test with legacy KEX/cipher compatibility
- 1.3" 128x64 I2C OLED + EC11 rotary control panel
  - One-item-at-a-time profile browser and Apply/Back menu, with
    auto-sizing text that fills available space for any profile name length
  - Apply sends `fan p <n> max <raw 0-255>` per fan (converted from the
    0-100% stored in the profile)
  - Verifies applied speed against `show /system1/fan<n>` for up to 10s,
    ignoring fans reported as not installed
  - Dimming screensaver after 10s idle, wakes on any input
