#include "loralink.h"
#include <esp_now.h>
#include <esp_wifi.h>

LoraLink loraLink;

static const uint8_t COMPANION_MAC[6] = LORA_COMPANION_MAC;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static LoraReport s_box;
static volatile bool s_full = false;
static volatile uint32_t s_rejected = 0;

// Runs in the WiFi task: validate, then hand over a copy
static void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if (len != (int)sizeof(LoraReport) || memcmp(mac, COMPANION_MAC, 6) != 0) {
        s_rejected++;
        return;
    }
    uint32_t magic;
    memcpy(&magic, data, sizeof(magic));
    if (magic != LORA_PROTO_MAGIC || data[4] != LORA_PROTO_VERSION) {
        s_rejected++;
        return;
    }
    portENTER_CRITICAL(&s_mux);
    memcpy(&s_box, data, sizeof(s_box));
    s_full = true;
    portEXIT_CRITICAL(&s_mux);
}

void LoraLink::begin() {
    esp_err_t e = esp_now_init();
    if (e == ESP_OK) e = esp_now_register_recv_cb(onRecv);
    Serial.printf("[LORA] ESP-NOW receiver %s (err=0x%x)\n", e == ESP_OK ? "up" : "FAILED", e);
}

uint32_t LoraLink::rejected() const { return s_rejected; }

void LoraLink::loop() {
    if (!s_full) return;
    LoraReport r;
    portENTER_CRITICAL(&s_mux);
    memcpy(&r, &s_box, sizeof(r));
    s_full = false;
    portEXIT_CRITICAL(&s_mux);

    // The companion sends each report on all 13 channels, so duplicates are normal
    if (_have && r.seq == _lastSeq) return;
    if (_have) {
        uint16_t gap = (uint16_t)(r.seq - _lastSeq);
        if (gap > 1 && gap < 1000) _missed += gap - 1;
    }
    if (r.hitCount > LORA_MAX_HITS) r.hitCount = LORA_MAX_HITS;
    if (r.band > BAND_915) r.band = BAND_915;

    _latest = r;
    memcpy(_water[_rowHead], r.spectrum, LORA_SPECTRUM_BINS);
    _rowHead = (_rowHead + 1) % LORA_WATER_ROWS;
    if (_rowCount < LORA_WATER_ROWS) _rowCount++;
    _lastSeq = r.seq;
    _lastRxMs = millis();
    _accepted++;
    _have = true;
    _new = true;
}
