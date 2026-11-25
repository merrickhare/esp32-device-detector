#include "loralink.h"
#include <esp_now.h>
#include <esp_wifi.h>

LoraLink loraLink;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static LoraReport s_box;
static volatile bool s_full = false;
static volatile uint32_t s_rejected = 0;

static portMUX_TYPE s_decMux = portMUX_INITIALIZER_UNLOCKED;
static MeshDecoded s_decBox;
static volatile bool s_decFull = false;

static void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
    // Accept frames from any source with valid protocol magic (companion or infiltrator)
    if (len < 5) { s_rejected++; return; }

    uint32_t magic;
    memcpy(&magic, data, sizeof(magic));

    if (magic == LORA_PROTO_MAGIC && len == (int)sizeof(LoraReport) && data[4] == LORA_PROTO_VERSION) {
        portENTER_CRITICAL(&s_mux);
        memcpy(&s_box, data, sizeof(s_box));
        s_full = true;
        portEXIT_CRITICAL(&s_mux);
    } else if (magic == LORA_DECODED_MAGIC && len == (int)sizeof(MeshDecoded) && data[4] == LORA_DECODED_VERSION) {
        portENTER_CRITICAL(&s_decMux);
        memcpy(&s_decBox, data, sizeof(s_decBox));
        s_decFull = true;
        portEXIT_CRITICAL(&s_decMux);
    } else {
        s_rejected++;
    }
}

void LoraLink::begin() {
    esp_err_t e = esp_now_init();
    if (e == ESP_OK) e = esp_now_register_recv_cb(onRecv);
    Serial.printf("[LORA] ESP-NOW receiver %s (err=0x%x)\n", e == ESP_OK ? "up" : "FAILED", e);
}

uint32_t LoraLink::rejected() const { return s_rejected; }

MeshNodeCache* LoraLink::getOrCreateNode(uint32_t id) {
    for (int i = 0; i < _nodeCount; i++)
        if (_nodes[i].id == id) return &_nodes[i];
    if (_nodeCount < MESH_NODE_MAX) {
        MeshNodeCache& n = _nodes[_nodeCount++];
        memset(&n, 0, sizeof(n));
        n.id = id;
        return &n;
    }
    // Evict oldest (least recently updated)
    int oldest = 0;
    uint32_t oldestMs = UINT32_MAX;
    for (int i = 0; i < _nodeCount; i++) {
        uint32_t latest = _nodes[i].posMs;
        if (_nodes[i].infoMs > latest) latest = _nodes[i].infoMs;
        if (_nodes[i].telemMs > latest) latest = _nodes[i].telemMs;
        if (latest < oldestMs) { oldestMs = latest; oldest = i; }
    }
    MeshNodeCache& n = _nodes[oldest];
    memset(&n, 0, sizeof(n));
    n.id = id;
    return &n;
}

const MeshNodeCache* LoraLink::findNode(uint32_t id) const {
    for (int i = 0; i < _nodeCount; i++)
        if (_nodes[i].id == id) return &_nodes[i];
    return nullptr;
}

void LoraLink::handleDecoded(const MeshDecoded& d) {
    uint32_t now = millis();
    MeshNodeCache* n = getOrCreateNode(d.from);

    switch (d.portnum) {
        case MESH_PORT_POSITION:
            n->latI = d.pos.latI;
            n->lonI = d.pos.lonI;
            n->alt = d.pos.alt;
            n->posMs = now;
            break;
        case MESH_PORT_NODEINFO:
            memcpy(n->shortName, d.nodeinfo.shortName, sizeof(n->shortName));
            memcpy(n->longName, d.nodeinfo.longName, sizeof(n->longName));
            n->hwModel = d.nodeinfo.hwModel;
            n->infoMs = now;
            break;
        case MESH_PORT_TEXT: {
            MeshMsg& m = _msgs[_msgHead];
            m.from = d.from;
            m.to = d.to;
            m.ms = now;
            m.rssi = d.rssi;
            memcpy(m.text, d.text.text, sizeof(m.text));
            _msgHead = (_msgHead + 1) % MESH_MSG_MAX;
            if (_msgCount < MESH_MSG_MAX) _msgCount++;
            break;
        }
        case MESH_PORT_TELEMETRY:
            n->battLevel = d.telemetry.battLevel;
            n->voltage = d.telemetry.voltage;
            n->chUtil = d.telemetry.chUtil;
            n->telemMs = now;
            break;
    }
    _newDec = true;
}

void LoraLink::loop() {
    // Handle decoded messages
    if (s_decFull) {
        MeshDecoded d;
        portENTER_CRITICAL(&s_decMux);
        memcpy(&d, &s_decBox, sizeof(d));
        s_decFull = false;
        portEXIT_CRITICAL(&s_decMux);
        if (d.seq != _lastDecSeq) {
            _lastDecSeq = d.seq;
            handleDecoded(d);
            _lastRxMs = millis();
        }
    }

    if (!s_full) return;
    LoraReport r;
    portENTER_CRITICAL(&s_mux);
    memcpy(&r, &s_box, sizeof(r));
    s_full = false;
    portEXIT_CRITICAL(&s_mux);

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
