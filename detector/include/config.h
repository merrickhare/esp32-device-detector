#pragma once
#include <Arduino.h>

// --- Pin assignments ---
#define PIN_SPI_CLK   18
#define PIN_SPI_MOSI  23
#define PIN_SPI_MISO  19

#define PIN_TFT_CS    15
#define PIN_TFT_DC     2
#define PIN_TFT_RST    4
#define PIN_TFT_LITE  27

// Touch (4-wire resistive, ADC1 pins for WiFi coexistence)
#define PIN_TOUCH_XP  32
#define PIN_TOUCH_YP  33
#define PIN_TOUCH_XM  25
#define PIN_TOUCH_YM  26

#define PIN_SD_CS      5

// --- Display ---
#define SCREEN_W  240
#define SCREEN_H  320

// --- Touch ---
#define TOUCH_PRESSURE_THRESHOLD 200
#define TOUCH_SAMPLES   6
#define TOUCH_DEBOUNCE_MS 150
#define TOUCH_RELEASE_MISSES 3

// --- Detection ---
#define MAX_DETECTIONS      128
#define EVENT_QUEUE_LEN      64
#define WIFI_CHANNEL_HOP_MS 250
#define WIFI_MAX_CHANNELS    13
#define SIGNAL_TRAIL_LEN     12
#define SPARK_INTERVAL_MS  4000
#define ACTIVE_WINDOW_MS  60000
#define QUIET_MARGIN_DB       7
#define QUIET_DECAY_MS  1800000   // 30 min
#define DEAUTH_WINDOW_MS   3000
#define DEAUTH_THRESHOLD     12
#define SPAM_WINDOW_MS     5000
#define SPAM_THRESHOLD       20   // new spoofed-looking BLE MACs per window
#define FOLLOW_MS        600000   // tracker present 10 min => following
#define FOLLOW_GAP_MS    300000   // gone 5 min => restart follow timer

// --- UI ---
#define POPUP_MS           4000
#define HOME_REFRESH_MS    1000
#define LIST_REFRESH_MS    2000
#define HUNT_REFRESH_MS     400

// --- Backlight ---
#define BACKLIGHT_CHANNEL  0
#define BACKLIGHT_FREQ  5000
#define BACKLIGHT_RES      8
#define BACKLIGHT_DEFAULT 200
#define BACKLIGHT_DIM      20
#define DIM_TIMEOUT_MS  60000

// --- LoRa companion (LilyGO LoRa32, ESP-NOW) ---
// Put your companion's MAC in include/local_config.h (git-ignored), e.g.
//   #define LORA_COMPANION_MAC {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC}
#if __has_include("local_config.h")
#include "local_config.h"
#endif
#ifndef LORA_COMPANION_MAC
#define LORA_COMPANION_MAC {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}   // broadcast fallback
#endif
#define LORA_OFFLINE_MS    5000

// --- Branding (override any of these in include/local_config.h) ---
#ifndef BRAND_NAME
#define BRAND_NAME      "DETECTOR"                          // big name on boot / desktop / portal
#endif
#ifndef BRAND_TITLE
#define BRAND_TITLE     "DEVICE // DETECTOR"                // Home screen title bar
#endif
#ifndef BRAND_LABEL
#define BRAND_LABEL     "OPERATOR // UNIT"                  // small label above the desktop name
#endif
#ifndef BRAND_BOOT_TAG
#define BRAND_BOOT_TAG  "SENSORS ARMED // SYSTEMS ONLINE"   // boot typewriter line
#endif
#ifndef BRAND_UNIT
#define BRAND_UNIT      "MK-1"                              // label on the desktop mech panel
#endif
#ifndef BRAND_AP_SSID
#define BRAND_AP_SSID   "DETECTOR-SETUP"                    // setup portal access point
#endif

// --- Desktop mode ---
#define DESKTOP_ANIM_MS      40       // ~25 fps
#define NET_REFRESH_MS  1800000UL     // weather every 30 min
#define NET_RETRY_MS     120000UL     // retry after a failure

// --- SD Logging ---
#define LOG_FLUSH_INTERVAL_MS 10000
