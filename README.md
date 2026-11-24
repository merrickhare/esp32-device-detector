# ESP32 Device Detector

Passive surveillance-device detector for the ESP32, with an animated boot
splash and a Desktop mode (clock + local weather). Listen-only: it scans BLE adverts
and WiFi management frames and flags known tracker / camera / skimmer signatures.
It does not transmit attacks.

## Hardware
| Part | Notes |
|------|-------|
| ESP32-WROOM dev board | detector (`detector/`) |
| Adafruit 2.8" ILI9341 TFT, resistive touch + microSD | display, alerts, SD log |
| LilyGO LoRa32 V2.1 (915 MHz) — optional | receive-only companion (`lora-companion/`), linked over ESP-NOW |

## Features
- BLE + WiFi passive detection with signature database (`detector/src/signatures.cpp`)
- Touch UI: Home, lists, Settings, LoRa status, SD logging
- Boot splash: drawn pilot helmet, targeting reticle, name typewriter, mech launch scene
- Desktop mode: big clock, date, weather (Open-Meteo), animated original mech
  sprite kit (`detector/tools/mech_sprites.py`). All scanning pauses while Desktop is open.
- WiFi setup portal on the device (AP `DETECTOR-SETUP` by default, random password shown on screen).
  Credentials are stored only in the board's NVS — never in source.

## Build
PlatformIO (espressif32 6.5.0, Arduino core 2.0.14).

```bash
cd detector && pio run -t upload
```

```bash
cd lora-companion && pio run -t upload
```

To pair the companion, create `detector/include/local_config.h` (git-ignored):

```c
#pragma once
#define LORA_COMPANION_MAC {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC}
```

Without it the detector falls back to ESP-NOW broadcast.

## Your own branding
The on-screen name, labels and setup-portal SSID come from `BRAND_*` defaults in
`detector/include/config.h`. Copy `detector/include/local_config.example.h` to
`detector/include/local_config.h` and set your own; that file is git-ignored, so
personal branding and your companion MAC never end up in the repo.

## Contributing
Issues and pull requests welcome. Keep it passive (receive-only) and keep secrets
and personal settings out of tracked files.

## Serial debug keys (115200)
`d` Desktop · `h` Home · `w` demo weather · `p` setup portal · `c` cancel portal

## Credits
Weather: [Open-Meteo](https://open-meteo.com). Location fallback: ip-api.com.
The mech sprites and helmet art are original designs drawn in code.
