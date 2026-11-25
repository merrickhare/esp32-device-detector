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

#ifndef PIN_SD_CS
#define PIN_SD_CS      5   // override in local_config.h if your board differs
#endif

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

// --- Hunt mode ---
#define HUNT_BLE_INTERVAL    100
#define HUNT_BLE_WINDOW       90   // ~90% duty cycle during hunt
#define NORMAL_BLE_INTERVAL  100
#define NORMAL_BLE_WINDOW     50   // ~50% duty cycle normally
#define HUNT_STALE_MS       4000   // target silent this long → look for rotated MAC
#define HUNT_RSSI_TOLERANCE   15   // dB window for MAC rotation match
#define RSSI_EMA_ALPHA        0.3f // smoothing factor (0–1, lower = smoother)
#define HUNT_PAUSE_WIFI     true  // pause WiFi sniffer during hunt for 100% BLE antenna

// --- Hunt display ---
#define HUNT_HISTORY_LEN      24   // timestamped RSSI samples for trend + history
#define BLE_RSSI_MIN       -100   // outer edge (BLE/WiFi)
#define BLE_RSSI_MAX        -35   // center     (BLE/WiFi)
#define LORA_RSSI_MIN      -130   // outer edge (LoRa)
#define LORA_RSSI_MAX       -60   // center     (LoRa)
#define HUNT_TREND_SAMPLES     4   // compare N recent vs N older for trend
#define LORA_HUNT_STALE_MS 30000   // LoRa samples come slowly

// --- False positive filter ---
#define AIRTAG_RATE_MIN     0.15f  // min hits/sec for real AirTag
#define AIRTAG_RATE_MAX     1.5f   // max hits/sec (above = phone/AirPods)
#define AIRTAG_RATE_SAMPLES   10   // need this many hits before filtering

// --- Battery monitoring ---
#ifndef PIN_BATTERY
#define PIN_BATTERY          35   // ADC1 input-only pin (override in local_config.h)
#endif
#define BATT_DIVIDER       2.0f   // Vbat / Vadc ratio (100k/100k = 2.0)
#define BATT_FULL_MV       4200
#define BATT_EMPTY_MV      3000
#define BATT_READ_MS      10000   // read interval
#define BATT_SAMPLES          8   // rolling average window
#define BATT_MIN_VALID_MV  2500   // below this = no battery circuit

// --- Stealth mode ---
#define STEALTH_BRIGHTNESS    5

// --- PCAP capture ---
#define PCAP_SNAPLEN        128   // max bytes per packet saved
#define PCAP_DLT_80211      105   // IEEE 802.11

// --- UI ---
#define POPUP_MS           4000
#define HOME_REFRESH_MS    1000
#define LIST_REFRESH_MS    2000
#define HUNT_REFRESH_MS     200   // faster for radar display

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
