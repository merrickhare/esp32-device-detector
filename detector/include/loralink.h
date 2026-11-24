#pragma once
#include "config.h"
#include "lora_proto.h"

#define LORA_WATER_ROWS 40

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
};

extern LoraLink loraLink;
