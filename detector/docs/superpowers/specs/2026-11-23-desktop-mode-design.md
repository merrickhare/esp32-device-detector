# Desktop mode — design (approved 2026-11-23)

## Intent
Give the detector a configurable brand (BRAND_* in config.h) and an
always-on desktop: local time, local weather, original mech header art, and animations,
with detection paused while it is shown.

## Goals
- Animated boot splash: original pilot helmet drawn in code, cyan reticle drawing in
  over the visor, BRAND_NAME / BRAND_BOOT_TAG typewriter, loading bar.
- DESKTOP screen: Header art drawn in code (starfield, grid, mech at 2x) with HUD brackets and BRAND_NAME,
  big clock + date + seconds sweep, visor-HUD radar animation,
  animated weather icon + temp, condition, hi/lo, place name.
- Entry: DESKTOP button on Home only. Tap exits to Home.
- User change (2026-11-23): in DESKTOP everything else is disabled — BLE scan, WiFi sniffer,
  LoRa link handling, alerts/popups, SD logging. Exiting restores the previous scan state.
- Radar is a decorative visor-HUD animation (no detection data, since scanning is off).

## Non-goals / anti-goals
- Never touches WiFi as a client outside DESKTOP, so carrying the detector never broadcasts the home SSID.
- WiFi password never in chat, source, or serial output.
- No TLS (heap); HTTP only.

## Approach (picked)
All art drawn in code; small sprites (radar, weather icon)
allocated only while DESKTOP is shown. Network sync is one owner state machine
(OFF / DESKTOP / PORTAL) in a new `netsync` module:
- Entering DESKTOP: stop scanners, `WiFi.begin`, fetch Open-Meteo (`timezone=auto` gives UTC offset),
  SNTP. Stay connected; re-fetch every 30 min (retry 2 min on failure). Leaving: disconnect (STA mode
  stays up for ESP-NOW), restore scanners. Clock keeps running from the RTC after the first sync.
- Location: city from portal → Open-Meteo geocoding; else ip-api.com. Cached in NVS with offset.
- Setup portal (Settings → WIFI SETUP): AP BRAND_AP_SSID (default "DETECTOR-SETUP") with random 8-digit password shown on
  screen, form at 192.168.4.1 (SSID list from scan, password, optional city, F/C, sync on/off),
  saved to NVS, then restart.

Alternatives rejected: full-screen photo background (no RAM for a frame buffer, redraw cost);
blind periodic WiFi.begin (privacy leak); hardcoded credentials (secrets in source/chat).

## Builder's calls
Exact animation timings, colors (white/red/navy/yellow/cyan from the art), serial debug keys
(`d` desktop, `h` home) for testing without touch.
