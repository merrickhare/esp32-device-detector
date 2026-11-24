#pragma once
// Wire format shared by lora-companion (sender) and device-detector (receiver).
// Keep both copies identical; bump LORA_PROTO_VERSION on any layout change.
#include <stdint.h>

#define LORA_PROTO_MAGIC    0x314C4444u   // "DDL1"
#define LORA_PROTO_VERSION  1
#define LORA_SPECTRUM_BINS  64
#define LORA_MAX_HITS       8

enum LoraBand : uint8_t { BAND_433 = 0, BAND_868 = 1, BAND_915 = 2 };
enum LoraHitKind : uint8_t { HIT_LORA = 1, HIT_MESHTASTIC = 2 };

struct __attribute__((packed)) LoraHit {
    uint32_t freqKhz;
    uint32_t nodeId;     // Meshtastic sender node number, 0 for plain LoRa
    int8_t   rssi;
    int8_t   snr;
    uint8_t  sf;
    uint8_t  kind;       // LoraHitKind
    uint16_t ageS;       // seconds since last heard
};

struct __attribute__((packed)) LoraReport {
    uint32_t magic;
    uint8_t  version;
    uint8_t  band;        // LoraBand
    uint16_t seq;
    uint32_t uptimeS;
    uint32_t startKhz;    // spectrum bin 0 centre
    uint32_t stepKhz;
    int8_t   spectrum[LORA_SPECTRUM_BINS];  // peak dBm per bin over the last sweeps
    int8_t   noiseFloor;  // median of the latest sweep, dBm
    int8_t   baseline;    // long-run quiet noise floor, dBm
    uint8_t  jamming;     // 1 while the whole band is raised above baseline
    uint8_t  radioOk;
    uint16_t loraPackets;
    uint16_t meshPackets;
    uint8_t  meshNodes;   // distinct Meshtastic nodes heard in the last 10 min
    uint8_t  hitCount;    // valid entries in hits[]
    LoraHit  hits[LORA_MAX_HITS];
};

static_assert(sizeof(LoraReport) <= 250, "ESP-NOW payload limit");
