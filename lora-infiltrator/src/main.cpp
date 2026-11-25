// Mesh infiltrator: joins the Meshtastic mesh as a legitimate node.
// Passive: decrypts all default-channel traffic and forwards to detector.
// Active: announces NodeInfo, requests positions from discovered nodes.
// Always listening — no spectrum sweep, zero missed Meshtastic packets.
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <RadioLib.h>
#include <U8g2lib.h>
#include <mbedtls/aes.h>
#include <mbedtls/sha256.h>
#include "lora_proto.h"

// --- LoRa32 V2.1 pin map ---
#define LORA_SCK   5
#define LORA_MISO 19
#define LORA_MOSI 27
#define LORA_CS   18
#define LORA_RST  23
#define LORA_DIO0 26
#define LORA_DIO1 33
#define OLED_SDA  21
#define OLED_SCL  22
#define LED_PIN   25

// --- Band ---
#ifndef LORA_BAND
#define LORA_BAND BAND_915
#endif
struct BandPlan { float startMHz, endMHz, meshMHz; const char* name; };
static const BandPlan BANDS[] = {
    {433.05f, 434.79f, 433.875f, "433"},
    {863.0f,  870.0f,  869.525f, "868"},
    {902.0f,  928.0f,  906.875f, "915"},
};
static const BandPlan& band = BANDS[LORA_BAND];

// Meshtastic LongFast radio params
#define MESH_BW        250.0f
#define MESH_SF        11
#define MESH_CR        5
#define MESH_SYNC      0x2B
#define MESH_PREAMBLE  16
#define MESH_HDR_LEN   16

// TX power (dBm, SX1276 PA_BOOST max = 17)
#define MESH_TX_POWER  17

// --- Infiltrator identity ---
// Looks like a normal Meshtastic node. Override in a local header if desired.
#ifndef INFIL_LONG_NAME
#define INFIL_LONG_NAME  "Meshtastic"
#endif
#ifndef INFIL_HW_MODEL
#define INFIL_HW_MODEL   4   // TBEAM (common, blends in)
#endif

// --- Timing ---
#define ANNOUNCE_INTERVAL_MS  900000  // 15 min (Meshtastic default NodeInfo rate)
#define PROBE_INTERVAL_MS      45000  // 45s between active probes
#define BOOT_QUIET_MS          10000  // listen 10s before first TX
#define REPORT_INTERVAL_MS      1000
#define OLED_INTERVAL_MS         500
#define NODE_WINDOW_MS        600000  // 10 min

// --- Node tracking ---
#define MAX_NODES  32

static SX1276 radio = new Module(LORA_CS, LORA_DIO0, LORA_RST, LORA_DIO1);
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);
static bool s_radioOk = false;

static uint32_t s_ourNodeId = 0;
static char s_shortName[5] = {};

// ================================================================
// ESP-NOW sender (same pattern as companion)
// ================================================================

static const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static volatile bool s_sendDone = true;
static portMUX_TYPE s_decodedMux = portMUX_INITIALIZER_UNLOCKED;
static MeshDecoded s_decoded = {};
static volatile bool s_decodedReady = false;

static void onSent(const uint8_t*, esp_now_send_status_t) { s_sendDone = true; }

static void broadcastDecoded(const MeshDecoded& d) {
    for (uint8_t ch = 1; ch <= 13; ch++) {
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        s_sendDone = false;
        if (esp_now_send(BROADCAST, (const uint8_t*)&d, sizeof(d)) != ESP_OK) continue;
        uint32_t t0 = millis();
        while (!s_sendDone && millis() - t0 < 20) delay(1);
    }
}

static void senderTask(void*) {
    MeshDecoded d;
    for (;;) {
        bool have = false;
        portENTER_CRITICAL(&s_decodedMux);
        if (s_decodedReady) {
            memcpy(&d, &s_decoded, sizeof(d));
            s_decodedReady = false;
            have = true;
        }
        portEXIT_CRITICAL(&s_decodedMux);
        if (have) broadcastDecoded(d);
        vTaskDelay(pdMS_TO_TICKS(140 + (esp_random() % 40)));
    }
}

static void publishDecoded(const MeshDecoded& d) {
    portENTER_CRITICAL(&s_decodedMux);
    memcpy(&s_decoded, &d, sizeof(d));
    s_decodedReady = true;
    portEXIT_CRITICAL(&s_decodedMux);
}

static void startEspNow() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    if (esp_now_init() != ESP_OK) { Serial.println("[INFIL] ESP-NOW init FAILED"); return; }
    esp_now_register_send_cb(onSent);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROADCAST, 6);
    peer.ifidx = WIFI_IF_STA;
    esp_now_add_peer(&peer);
    xTaskCreatePinnedToCore(senderTask, "espnow_tx", 4096, nullptr, 1, nullptr, 0);
    Serial.printf("[INFIL] ESP-NOW up, MAC %s\n", WiFi.macAddress().c_str());
}

// ================================================================
// Node tracking
// ================================================================

struct TrackedNode {
    uint32_t id;
    int8_t   rssi, snr;
    uint32_t lastMs;
    uint32_t posMs;    // when we last got their position
    uint32_t infoMs;
    bool     probed;   // true if we've requested their position
};

static TrackedNode s_nodes[MAX_NODES];
static int s_nodeCount = 0;
static uint16_t s_meshPackets = 0;
static uint16_t s_txCount = 0;
static uint16_t s_probeCount = 0;
static uint16_t s_decodeCount = 0;
static char s_lastEvent[32] = "listening...";

static TrackedNode* findOrCreateNode(uint32_t id) {
    for (int i = 0; i < s_nodeCount; i++)
        if (s_nodes[i].id == id) return &s_nodes[i];
    if (s_nodeCount < MAX_NODES) {
        TrackedNode& n = s_nodes[s_nodeCount++];
        memset(&n, 0, sizeof(n));
        n.id = id;
        return &n;
    }
    int oldest = 0;
    for (int i = 1; i < s_nodeCount; i++)
        if (s_nodes[i].lastMs < s_nodes[oldest].lastMs) oldest = i;
    TrackedNode& n = s_nodes[oldest];
    memset(&n, 0, sizeof(n));
    n.id = id;
    return &n;
}

// ================================================================
// Meshtastic AES-128-CTR (symmetric: same function encrypts and decrypts)
// ================================================================

static uint8_t s_meshKey[16];
static bool s_meshKeyReady = false;

static void initMeshKey() {
    const uint8_t psk[] = {0x01};
    uint8_t hash[32];
    mbedtls_sha256(psk, 1, hash, 0);
    memcpy(s_meshKey, hash, 16);
    s_meshKeyReady = true;
}

static bool meshCrypt(uint8_t* payload, size_t len, uint32_t packetId, uint32_t fromNode) {
    if (!s_meshKeyReady || len == 0) return false;
    uint8_t nonce[16] = {};
    memcpy(nonce, &packetId, 4);
    memcpy(nonce + 4, &fromNode, 4);
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, s_meshKey, 128);
    uint8_t stream[16] = {};
    size_t nc_off = 0;
    int r = mbedtls_aes_crypt_ctr(&aes, len, &nc_off, nonce, stream, payload, payload);
    mbedtls_aes_free(&aes);
    return r == 0;
}

// ================================================================
// Protobuf decoder (same as companion)
// ================================================================

static bool pbVarint(const uint8_t*& p, const uint8_t* end, uint64_t& val) {
    val = 0; int shift = 0;
    while (p < end) {
        uint8_t b = *p++;
        val |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) return true;
        shift += 7;
        if (shift > 63) return false;
    }
    return false;
}

static bool pbSkip(const uint8_t*& p, const uint8_t* end, uint8_t wt) {
    uint64_t v;
    switch (wt) {
        case 0: return pbVarint(p, end, v);
        case 1: p += 8; return p <= end;
        case 2: if (!pbVarint(p, end, v)) return false; p += v; return p <= end;
        case 5: p += 4; return p <= end;
        default: return false;
    }
}

struct DataEnvelope { uint8_t portnum; const uint8_t* payload; size_t payloadLen; };

static bool parseDataEnvelope(const uint8_t* data, size_t len, DataEnvelope& env) {
    env = {};
    const uint8_t* p = data, *end = data + len;
    while (p < end) {
        uint64_t tag; if (!pbVarint(p, end, tag)) return false;
        uint8_t field = tag >> 3, wt = tag & 7;
        if (field == 1 && wt == 0) { uint64_t v; if (!pbVarint(p, end, v)) return false; env.portnum = (uint8_t)v; }
        else if (field == 2 && wt == 2) { uint64_t v; if (!pbVarint(p, end, v)) return false; env.payload = p; env.payloadLen = (size_t)v; p += v; }
        else { if (!pbSkip(p, end, wt)) return false; }
    }
    return env.portnum != 0 && env.payload != nullptr;
}

static void parsePosition(const uint8_t* data, size_t len, MeshDecoded& d) {
    const uint8_t* p = data, *end = data + len;
    while (p < end) {
        uint64_t tag; if (!pbVarint(p, end, tag)) return;
        uint8_t field = tag >> 3, wt = tag & 7;
        if (field == 1 && wt == 5 && p + 4 <= end) { memcpy(&d.pos.latI, p, 4); p += 4; }
        else if (field == 2 && wt == 5 && p + 4 <= end) { memcpy(&d.pos.lonI, p, 4); p += 4; }
        else if (field == 3 && wt == 0) { uint64_t v; if (!pbVarint(p, end, v)) return; d.pos.alt = (int32_t)v; }
        else if (field == 4 && wt == 5 && p + 4 <= end) { memcpy(&d.pos.time, p, 4); p += 4; }
        else if (field == 9 && wt == 0) { uint64_t v; if (!pbVarint(p, end, v)) return; d.pos.satsInView = (uint32_t)v; }
        else { if (!pbSkip(p, end, wt)) return; }
    }
}

static void parseNodeInfo(const uint8_t* data, size_t len, MeshDecoded& d) {
    const uint8_t* p = data, *end = data + len;
    while (p < end) {
        uint64_t tag; if (!pbVarint(p, end, tag)) return;
        uint8_t field = tag >> 3, wt = tag & 7;
        if (field == 2 && wt == 2) {
            uint64_t slen; if (!pbVarint(p, end, slen)) return;
            size_t n = slen < sizeof(d.nodeinfo.longName) - 1 ? (size_t)slen : sizeof(d.nodeinfo.longName) - 1;
            memcpy(d.nodeinfo.longName, p, n); d.nodeinfo.longName[n] = 0; p += slen;
        } else if (field == 3 && wt == 2) {
            uint64_t slen; if (!pbVarint(p, end, slen)) return;
            size_t n = slen < sizeof(d.nodeinfo.shortName) - 1 ? (size_t)slen : sizeof(d.nodeinfo.shortName) - 1;
            memcpy(d.nodeinfo.shortName, p, n); d.nodeinfo.shortName[n] = 0; p += slen;
        } else if (field == 4 && wt == 0) {
            uint64_t v; if (!pbVarint(p, end, v)) return; d.nodeinfo.hwModel = (uint8_t)v;
        } else { if (!pbSkip(p, end, wt)) return; }
    }
}

static void parseTelemetry(const uint8_t* data, size_t len, MeshDecoded& d) {
    const uint8_t* p = data, *end = data + len;
    while (p < end) {
        uint64_t tag; if (!pbVarint(p, end, tag)) return;
        uint8_t field = tag >> 3, wt = tag & 7;
        if (field == 1 && wt == 5 && p + 4 <= end) { memcpy(&d.telemetry.time, p, 4); p += 4; }
        else if (field == 2 && wt == 2) {
            uint64_t slen; if (!pbVarint(p, end, slen)) return;
            const uint8_t* sub = p, *subEnd = p + slen; p += slen;
            while (sub < subEnd) {
                uint64_t stag; if (!pbVarint(sub, subEnd, stag)) return;
                uint8_t sf2 = stag >> 3, sw = stag & 7;
                if (sf2 == 1 && sw == 0) { uint64_t v; if (!pbVarint(sub, subEnd, v)) return; d.telemetry.battLevel = (float)v; }
                else if (sf2 == 2 && sw == 5 && sub + 4 <= subEnd) { memcpy(&d.telemetry.voltage, sub, 4); sub += 4; }
                else if (sf2 == 3 && sw == 5 && sub + 4 <= subEnd) { memcpy(&d.telemetry.chUtil, sub, 4); sub += 4; }
                else if (sf2 == 4 && sw == 5 && sub + 4 <= subEnd) { memcpy(&d.telemetry.airUtilTx, sub, 4); sub += 4; }
                else { if (!pbSkip(sub, subEnd, sw)) return; }
            }
        } else { if (!pbSkip(p, end, wt)) return; }
    }
}

// ================================================================
// Protobuf encoder (new — for building TX packets)
// ================================================================

static int encVarint(uint8_t* buf, uint64_t val) {
    int n = 0;
    do { buf[n] = val & 0x7F; val >>= 7; if (val) buf[n] |= 0x80; n++; } while (val);
    return n;
}

static int encVarField(uint8_t* buf, uint8_t field, uint64_t val) {
    int n = encVarint(buf, (uint64_t)(field << 3));
    n += encVarint(buf + n, val);
    return n;
}

static int encBytes(uint8_t* buf, uint8_t field, const uint8_t* data, size_t len) {
    int n = encVarint(buf, (uint64_t)(field << 3 | 2));
    n += encVarint(buf + n, len);
    memcpy(buf + n, data, len);
    return n + (int)len;
}

static int encString(uint8_t* buf, uint8_t field, const char* str) {
    return encBytes(buf, field, (const uint8_t*)str, strlen(str));
}

// ================================================================
// Mesh TX (new — encrypt + header + transmit)
// ================================================================

static volatile bool s_rxFlag = false;
static void IRAM_ATTR onRxDone() { s_rxFlag = true; }

static void enterMeshRx() {
    radio.setFrequency(band.meshMHz);
    radio.setBandwidth(MESH_BW);
    radio.setSpreadingFactor(MESH_SF);
    radio.setCodingRate(MESH_CR);
    radio.setPacketReceivedAction(onRxDone);
    s_rxFlag = false;
    radio.startReceive();
}

static bool meshTx(uint32_t to, uint8_t portnum, const uint8_t* payload, size_t payloadLen, bool wantResponse) {
    // Build Data envelope
    uint8_t data[200];
    int dLen = 0;
    dLen += encVarField(data + dLen, 1, portnum);
    if (payloadLen > 0)
        dLen += encBytes(data + dLen, 2, payload, payloadLen);
    if (wantResponse)
        dLen += encVarField(data + dLen, 6, 1);

    uint32_t packetId = esp_random();

    // Encrypt in place (CTR is symmetric)
    if (!meshCrypt(data, dLen, packetId, s_ourNodeId)) return false;

    // Build 16-byte header + encrypted payload
    uint8_t pkt[256];
    memcpy(pkt, &to, 4);
    memcpy(pkt + 4, &s_ourNodeId, 4);
    memcpy(pkt + 8, &packetId, 4);
    pkt[12] = 3 | (3 << 5);   // hop_limit=3, hop_start=3
    pkt[13] = 0;               // channel 0 (primary)
    pkt[14] = 0;               // next_hop
    pkt[15] = 0;               // relay_node
    memcpy(pkt + MESH_HDR_LEN, data, dLen);

    // Listen-before-talk: abort if channel is busy
    radio.clearPacketReceivedAction();
    bool busy = radio.scanChannel() == RADIOLIB_PREAMBLE_DETECTED;
    if (busy) { enterMeshRx(); return false; }

    int st = radio.transmit(pkt, MESH_HDR_LEN + dLen);
    enterMeshRx();
    return st == RADIOLIB_ERR_NONE;
}

// ================================================================
// Mesh RX handler (decrypt + decode + forward)
// ================================================================

static uint16_t s_decSeq = 0;

static void tryDecode(const uint8_t* raw, size_t rawLen, int8_t rssi, int8_t snr) {
    if (rawLen < MESH_HDR_LEN + 1) return;

    uint32_t to2   = raw[0] | (raw[1] << 8) | (raw[2] << 16) | ((uint32_t)raw[3] << 24);
    uint32_t from2 = raw[4] | (raw[5] << 8) | (raw[6] << 16) | ((uint32_t)raw[7] << 24);
    uint32_t id    = raw[8] | (raw[9] << 8) | (raw[10] << 16) | ((uint32_t)raw[11] << 24);
    uint8_t chIdx  = raw[13];

    // Ignore our own packets (relayed back through the mesh)
    if (from2 == s_ourNodeId) return;

    size_t payLen = rawLen - MESH_HDR_LEN;
    if (payLen > 240) return;
    uint8_t dec[240];
    memcpy(dec, raw + MESH_HDR_LEN, payLen);

    if (!meshCrypt(dec, payLen, id, from2)) return;

    DataEnvelope env;
    if (!parseDataEnvelope(dec, payLen, env)) return;

    MeshDecoded d = {};
    d.magic = LORA_DECODED_MAGIC;
    d.version = LORA_DECODED_VERSION;
    d.portnum = env.portnum;
    d.from = from2;
    d.to = to2;
    d.rssi = rssi;
    d.snr = snr;
    d.hopLimit = raw[12] & 0x07;
    d.channel = chIdx;

    TrackedNode* nd = findOrCreateNode(from2);
    nd->rssi = rssi;
    nd->snr = snr;
    nd->lastMs = millis();

    switch (env.portnum) {
        case MESH_PORT_TEXT: {
            size_t n = env.payloadLen < sizeof(d.text.text) - 1 ? env.payloadLen : sizeof(d.text.text) - 1;
            memcpy(d.text.text, env.payload, n); d.text.text[n] = 0;
            Serial.printf("[MESH] TEXT !%08lx: %s\n", (unsigned long)from2, d.text.text);
            snprintf(s_lastEvent, sizeof(s_lastEvent), "TXT !%04lx", (unsigned long)(from2 & 0xFFFF));
            break;
        }
        case MESH_PORT_POSITION:
            parsePosition(env.payload, env.payloadLen, d);
            nd->posMs = millis();
            Serial.printf("[MESH] POS !%08lx: %.6f,%.6f\n", (unsigned long)from2, d.pos.latI / 1e7, d.pos.lonI / 1e7);
            snprintf(s_lastEvent, sizeof(s_lastEvent), "POS !%04lx", (unsigned long)(from2 & 0xFFFF));
            break;
        case MESH_PORT_NODEINFO:
            parseNodeInfo(env.payload, env.payloadLen, d);
            nd->infoMs = millis();
            Serial.printf("[MESH] NODE !%08lx: \"%s\"\n", (unsigned long)from2, d.nodeinfo.longName);
            snprintf(s_lastEvent, sizeof(s_lastEvent), "NODE %s", d.nodeinfo.shortName);
            break;
        case MESH_PORT_TELEMETRY:
            parseTelemetry(env.payload, env.payloadLen, d);
            Serial.printf("[MESH] TELEM !%08lx: batt%.0f%%\n", (unsigned long)from2, d.telemetry.battLevel);
            snprintf(s_lastEvent, sizeof(s_lastEvent), "TELEM !%04lx", (unsigned long)(from2 & 0xFFFF));
            break;
        default:
            return;
    }

    d.seq = ++s_decSeq;
    s_decodeCount++;
    publishDecoded(d);
}

static void handlePacket() {
    s_rxFlag = false;
    uint8_t buf[256];
    size_t len = radio.getPacketLength();
    if (len > sizeof(buf)) len = sizeof(buf);
    int st = radio.readData(buf, len);
    int8_t rssi = (int8_t)radio.getRSSI(true);
    int8_t snr = (int8_t)radio.getSNR();

    if (st == RADIOLIB_ERR_NONE && len >= MESH_HDR_LEN) {
        uint32_t from = buf[4] | (buf[5] << 8) | (buf[6] << 16) | ((uint32_t)buf[7] << 24);
        if (from != 0 && from != s_ourNodeId) {
            if (s_meshPackets < 65535) s_meshPackets++;
            tryDecode(buf, len, rssi, snr);
        }
    }
    radio.startReceive();
}

// ================================================================
// Mesh management (announce + probe cycle)
// ================================================================

static uint32_t s_lastAnnounce = 0;
static uint32_t s_lastProbe = 0;
static int s_probeIdx = 0;

static void announceNodeInfo() {
    // Build User protobuf (our identity)
    char idStr[12];
    snprintf(idStr, sizeof(idStr), "!%08lx", (unsigned long)s_ourNodeId);
    uint8_t user[80];
    int n = 0;
    n += encString(user + n, 1, idStr);
    n += encString(user + n, 2, INFIL_LONG_NAME);
    n += encString(user + n, 3, s_shortName);
    n += encVarField(user + n, 4, INFIL_HW_MODEL);

    if (meshTx(0xFFFFFFFF, MESH_PORT_NODEINFO, user, n, false)) {
        s_txCount++;
        Serial.printf("[INFIL] Announced as %s (%s)\n", s_shortName, INFIL_LONG_NAME);
        snprintf(s_lastEvent, sizeof(s_lastEvent), "TX announce");
    }
}

static void probeNextNode() {
    uint32_t now = millis();
    // Find a node we haven't gotten a position from yet
    for (int i = 0; i < s_nodeCount; i++) {
        int idx = (s_probeIdx + i) % s_nodeCount;
        TrackedNode& nd = s_nodes[idx];
        if (nd.id == 0 || now - nd.lastMs > NODE_WINDOW_MS) continue;
        if (nd.posMs == 0 && !nd.probed) {
            // Request position from this node
            if (meshTx(nd.id, MESH_PORT_POSITION, nullptr, 0, true)) {
                nd.probed = true;
                s_txCount++;
                s_probeCount++;
                s_probeIdx = (idx + 1) % MAX_NODES;
                Serial.printf("[INFIL] Requested position from !%08lx\n", (unsigned long)nd.id);
                snprintf(s_lastEvent, sizeof(s_lastEvent), "PROBE !%04lx", (unsigned long)(nd.id & 0xFFFF));
            }
            return;
        }
    }
    // All known nodes have been probed; re-probe the oldest position
    uint32_t oldestPos = UINT32_MAX;
    int oldestIdx = -1;
    for (int i = 0; i < s_nodeCount; i++) {
        if (s_nodes[i].id && s_nodes[i].posMs && s_nodes[i].posMs < oldestPos) {
            oldestPos = s_nodes[i].posMs;
            oldestIdx = i;
        }
    }
    if (oldestIdx >= 0 && now - oldestPos > 300000) {
        TrackedNode& nd = s_nodes[oldestIdx];
        if (meshTx(nd.id, MESH_PORT_POSITION, nullptr, 0, true)) {
            s_txCount++;
            s_probeCount++;
            Serial.printf("[INFIL] Re-probed !%08lx (stale pos)\n", (unsigned long)nd.id);
            snprintf(s_lastEvent, sizeof(s_lastEvent), "REPROBE !%04lx", (unsigned long)(nd.id & 0xFFFF));
        }
    }
}

static void meshManage() {
    uint32_t now = millis();
    if (now < BOOT_QUIET_MS) return;

    if (s_lastAnnounce == 0 || now - s_lastAnnounce > ANNOUNCE_INTERVAL_MS) {
        announceNodeInfo();
        s_lastAnnounce = now;
    }

    if (s_nodeCount > 0 && (s_lastProbe == 0 || now - s_lastProbe > PROBE_INTERVAL_MS)) {
        probeNextNode();
        s_lastProbe = now;
    }
}

// ================================================================
// OLED display
// ================================================================

static void drawOled() {
    char buf[32];
    oled.clearBuffer();
    oled.setFont(u8g2_font_5x8_tr);

    if (!s_radioOk) {
        oled.drawStr(0, 8, "RADIO NOT FOUND");
        oled.sendBuffer();
        return;
    }

    snprintf(buf, sizeof(buf), "INFIL [%s] !%04lx", s_shortName, (unsigned long)(s_ourNodeId & 0xFFFF));
    oled.drawStr(0, 7, buf);

    snprintf(buf, sizeof(buf), "Nodes:%d Pkts:%u Dec:%u", s_nodeCount, s_meshPackets, s_decodeCount);
    oled.drawStr(0, 18, buf);

    snprintf(buf, sizeof(buf), "TX:%u Probed:%u", s_txCount, s_probeCount);
    oled.drawStr(0, 29, buf);

    // Active node list (up to 4 rows)
    uint32_t now = millis();
    int y = 40;
    for (int i = 0; i < s_nodeCount && y <= 56; i++) {
        TrackedNode& nd = s_nodes[i];
        if (!nd.id || now - nd.lastMs > NODE_WINDOW_MS) continue;
        uint16_t ago = (now - nd.lastMs) / 1000;
        snprintf(buf, sizeof(buf), "!%04lx %ddBm %s %us",
                 (unsigned long)(nd.id & 0xFFFF), nd.rssi,
                 nd.posMs ? "P" : (nd.probed ? "?" : "."), ago);
        oled.drawStr(0, y, buf);
        y += 9;
    }

    oled.drawHLine(0, 59, 128);
    oled.drawStr(0, 63, s_lastEvent);
    oled.sendBuffer();
}

// ================================================================
// Arduino setup + loop
// ================================================================

void setup() {
    Serial.begin(115200);
    delay(200);

    // Derive node ID from MAC (like Meshtastic does)
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    s_ourNodeId = mac[2] << 24 | mac[3] << 16 | mac[4] << 8 | mac[5];
    // Short name from last 2 bytes of node ID (Meshtastic default)
    snprintf(s_shortName, sizeof(s_shortName), "%04lX", (unsigned long)(s_ourNodeId & 0xFFFF));

    Serial.printf("\n[INFIL] Mesh infiltrator v1, node !%08lx [%s]\n", (unsigned long)s_ourNodeId, s_shortName);
    Serial.printf("[INFIL] Band %s, freq %.3f MHz, TX %d dBm\n", band.name, band.meshMHz, MESH_TX_POWER);
    pinMode(LED_PIN, OUTPUT);

    Wire.begin(OLED_SDA, OLED_SCL);
    oled.begin();
    oled.setFont(u8g2_font_5x8_tr);
    oled.clearBuffer();
    oled.drawStr(0, 10, "MESH INFILTRATOR");
    char buf[32];
    snprintf(buf, sizeof(buf), "!%08lx [%s]", (unsigned long)s_ourNodeId, s_shortName);
    oled.drawStr(0, 22, buf);
    oled.drawStr(0, 34, "joining mesh...");
    oled.sendBuffer();

    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
    int st = radio.begin(band.meshMHz, MESH_BW, MESH_SF, MESH_CR, MESH_SYNC, MESH_TX_POWER, MESH_PREAMBLE, 0);
    if (st == RADIOLIB_ERR_NONE) st = radio.setCRC(true);
    s_radioOk = st == RADIOLIB_ERR_NONE;
    Serial.printf("[INFIL] radio: %d (%s)\n", st, s_radioOk ? "OK" : "FAILED");
    if (s_radioOk) enterMeshRx();

    initMeshKey();
    startEspNow();
}

void loop() {
    uint32_t now = millis();
    static uint32_t lastOled = 0, lastLog = 0;

    if (s_radioOk) {
        if (s_rxFlag) {
            handlePacket();
            digitalWrite(LED_PIN, HIGH);
        }
        if (now - millis() > 80 || !digitalRead(LED_PIN)) {} // LED off handled below
        meshManage();
    }

    if (now - lastOled >= OLED_INTERVAL_MS) {
        lastOled = now;
        drawOled();
    }

    // LED off after flash
    static uint32_t ledOn = 0;
    if (digitalRead(LED_PIN)) {
        if (ledOn == 0) ledOn = now;
        else if (now - ledOn > 80) { digitalWrite(LED_PIN, LOW); ledOn = 0; }
    }

    if (now - lastLog >= 10000) {
        lastLog = now;
        Serial.printf("[INFIL] nodes %d pkts %u decoded %u tx %u probed %u\n",
                      s_nodeCount, s_meshPackets, s_decodeCount, s_txCount, s_probeCount);
    }
    delay(2);
}
