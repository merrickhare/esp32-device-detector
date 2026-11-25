#pragma once
// Wire format shared by lora-companion (sender) and device-detector (receiver).
// Keep both copies identical; bump LORA_PROTO_VERSION on any layout change.
#include <stdint.h>

#define LORA_PROTO_MAGIC    0x314C4444u   // "DDL1"
#define LORA_PROTO_VERSION  1
#define LORA_SPECTRUM_BINS  64
#define LORA_MAX_HITS       8

// Second frame type for decoded Meshtastic data
#define LORA_DECODED_MAGIC  0x324C4444u   // "DDL2"
#define LORA_DECODED_VERSION 1

enum LoraBand : uint8_t { BAND_433 = 0, BAND_868 = 1, BAND_915 = 2 };
enum LoraHitKind : uint8_t { HIT_LORA = 1, HIT_MESHTASTIC = 2 };

// Decoded Meshtastic payload types
enum MeshPortnum : uint8_t {
    MESH_PORT_TEXT      = 1,
    MESH_PORT_POSITION  = 3,
    MESH_PORT_NODEINFO  = 4,
    MESH_PORT_TELEMETRY = 67,
};

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

// Decoded Meshtastic message — sent as a separate ESP-NOW frame
struct __attribute__((packed)) MeshDecoded {
    uint32_t magic;       // LORA_DECODED_MAGIC
    uint8_t  version;     // LORA_DECODED_VERSION
    uint8_t  portnum;     // MeshPortnum
    uint16_t seq;
    uint32_t from;        // source node ID
    uint32_t to;          // destination node ID
    int8_t   rssi;
    int8_t   snr;
    uint8_t  hopLimit;
    uint8_t  channel;     // channel index
    union __attribute__((packed)) {
        struct __attribute__((packed)) {
            int32_t  latI;    // lat * 1e7
            int32_t  lonI;    // lon * 1e7
            int32_t  alt;     // meters
            uint32_t time;    // unix epoch
            uint32_t satsInView;
        } pos;
        struct __attribute__((packed)) {
            char shortName[5];
            char longName[40];
            uint8_t hwModel;
        } nodeinfo;
        struct __attribute__((packed)) {
            char text[100];
        } text;
        struct __attribute__((packed)) {
            uint32_t time;
            float    battLevel;   // 0-100
            float    voltage;
            float    chUtil;      // channel utilization %
            float    airUtilTx;   // air utilization tx %
        } telemetry;
    };
};

static_assert(sizeof(MeshDecoded) <= 250, "ESP-NOW payload limit");
