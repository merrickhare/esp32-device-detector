#pragma once
#include "config.h"
#include "lora_proto.h"

#define LORA_WATER_ROWS 40
#define MESH_MSG_MAX    16
#define MESH_NODE_MAX   32

// Decoded Meshtastic node info cached on the detector
struct MeshNodeCache {
    uint32_t id;
    int32_t  latI, lonI, alt;
    float    battLevel, voltage, chUtil;
    char     shortName[5];
    char     longName[40];
    uint8_t  hwModel;
    uint32_t posMs;       // millis() when position was last updated
    uint32_t infoMs;      // millis() when nodeinfo was last updated
    uint32_t telemMs;     // millis() when telemetry was last updated
};

// Recent text message
struct MeshMsg {
    uint32_t from;
    uint32_t to;
    uint32_t ms;          // millis() when received
    int8_t   rssi;
    char     text[100];
};

// Receives LoraReports from the LoRa32 companion over ESP-NOW.
class LoraLink {
public:
    void begin();   // call after WiFi is started
    void loop();    // moves the latest report out of the radio-task mailbox

    bool online() const { return _have && millis() - _lastRxMs < LORA_OFFLINE_MS; }
    bool everSeen() const { return _have; }
    bool fresh() { bool f = _new; _new = false; return f; }
    const LoraReport& latest() const { return _latest; }

    // Spectrum history for the waterfall; 0 = newest
    int rows() const { return _rowCount; }
    const int8_t* row(int age) const {
        return _water[(_rowHead + LORA_WATER_ROWS - 1 - age) % LORA_WATER_ROWS];
    }

    // Decoded Meshtastic data
    const MeshNodeCache* findNode(uint32_t id) const;
    int nodeCount() const { return _nodeCount; }
    const MeshNodeCache& node(int i) const { return _nodes[i]; }
    int msgCount() const { return _msgCount; }
    const MeshMsg& msg(int i) const {
        return _msgs[(_msgHead + MESH_MSG_MAX - _msgCount + i) % MESH_MSG_MAX];
    }
    bool newDecoded() { bool f = _newDec; _newDec = false; return f; }

    uint32_t accepted() const { return _accepted; }
    uint32_t missed() const { return _missed; }
    uint32_t rejected() const;
    uint32_t lastRxMs() const { return _lastRxMs; }

private:
    LoraReport _latest = {};
    bool _have = false;
    bool _new = false;
    uint16_t _lastSeq = 0;
    uint32_t _lastRxMs = 0;
    uint32_t _accepted = 0;
    uint32_t _missed = 0;
    int8_t _water[LORA_WATER_ROWS][LORA_SPECTRUM_BINS];
    uint8_t _rowHead = 0;
    uint8_t _rowCount = 0;

    // Decoded Meshtastic state
    MeshNodeCache _nodes[MESH_NODE_MAX];
    int _nodeCount = 0;
    MeshMsg _msgs[MESH_MSG_MAX];
    uint8_t _msgHead = 0;
    uint8_t _msgCount = 0;
    bool _newDec = false;
    uint16_t _lastDecSeq = 0;

    MeshNodeCache* getOrCreateNode(uint32_t id);
    void handleDecoded(const MeshDecoded& d);
};

extern LoraLink loraLink;
