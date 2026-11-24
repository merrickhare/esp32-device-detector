// LoRa32 companion for the device detector. Receive-only: the SX1276 is never
// told to transmit. Rests in Meshtastic LongFast receive, and every couple of
// seconds (when no packet is arriving) sweeps the band for a spectrum/jamming
// picture and runs a quick LoRa channel-activity check on any hot spot.
// Results go to the detector as a LoraReport over ESP-NOW.
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <RadioLib.h>
#include <U8g2lib.h>
#include "lora_proto.h"

// --- ttgov21new (LoRa32 V2.1 / v1.6) pin map, confirmed by radio.begin() ---
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

// --- Band (this board shipped with US915 Paxcounter) ---
#ifndef LORA_BAND
#define LORA_BAND BAND_915
#endif
struct BandPlan { float startMHz, endMHz, meshMHz; const char* name; };
static const BandPlan BANDS[] = {
    {433.05f, 434.79f, 433.875f, "433"},   // EU_433
    {863.0f,  870.0f,  869.525f, "868"},   // EU_868
    {902.0f,  928.0f,  906.875f, "915"},   // US
};
static const BandPlan& band = BANDS[LORA_BAND];

// Meshtastic LongFast (firmware RadioLibInterface / RadioInterface + docs)
#define MESH_BW        250.0f
#define MESH_SF        11
#define MESH_CR        5
#define MESH_SYNC      0x2B
#define MESH_PREAMBLE  16
#define MESH_HDR_LEN   16

// --- Timing / thresholds ---
#define SWEEP_INTERVAL_MS  2000
#define REPORT_INTERVAL_MS 1000
#define OLED_INTERVAL_MS    500
#define RSSI_SETTLE_US     1500
#define HOTSPOT_DB           15   // bin this far above the floor gets a CAD check
#define JAM_DB               15   // whole-band median this far above baseline = jamming
#define JAM_CLEAR_DB          8
#define JAM_SWEEPS            3
#define NODE_WINDOW_MS   600000   // 10 min
#define MAX_NODES            32
#define MAX_BURSTS           16
#define WATERFALL_ROWS       40

static SX1276 radio = new Module(LORA_CS, LORA_DIO0, LORA_RST, LORA_DIO1);
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);
static bool s_radioOk = false;

// ---------------- ESP-NOW sender ----------------

static const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static volatile bool s_sendDone = true;
static portMUX_TYPE s_reportMux = portMUX_INITIALIZER_UNLOCKED;
static LoraReport s_report = {};

static void onSent(const uint8_t*, esp_now_send_status_t) { s_sendDone = true; }

static void broadcastAllChannels(const LoraReport& r) {
    // The detector hops channels, so send one copy on each; it keeps whichever it hears
    for (uint8_t ch = 1; ch <= 13; ch++) {
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        s_sendDone = false;
        if (esp_now_send(BROADCAST, (const uint8_t*)&r, sizeof(r)) != ESP_OK) continue;
        uint32_t t0 = millis();
        while (!s_sendDone && millis() - t0 < 20) delay(1);
    }
}

// Re-sends the current report ~6x per second. The detector's radio is shared with
// BLE and hops WiFi channels, so any single copy has only a fair chance of landing.
static void senderTask(void*) {
    LoraReport r;
    for (;;) {
        portENTER_CRITICAL(&s_reportMux);
        memcpy(&r, &s_report, sizeof(r));
        portEXIT_CRITICAL(&s_reportMux);
        if (r.magic == LORA_PROTO_MAGIC) broadcastAllChannels(r);
        vTaskDelay(pdMS_TO_TICKS(140 + (esp_random() % 40)));
    }
}

static void publish(const LoraReport& r) {
    portENTER_CRITICAL(&s_reportMux);
    memcpy(&s_report, &r, sizeof(r));
    portEXIT_CRITICAL(&s_reportMux);
}

static void startEspNow() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    if (esp_now_init() != ESP_OK) {
        Serial.println("[LORA] esp_now_init FAILED");
        return;
    }
    esp_now_register_send_cb(onSent);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROADCAST, 6);
    peer.ifidx = WIFI_IF_STA;
    esp_now_add_peer(&peer);
    xTaskCreatePinnedToCore(senderTask, "espnow_tx", 4096, nullptr, 1, nullptr, 0);
    Serial.printf("[LORA] ESP-NOW up, MAC %s\n", WiFi.macAddress().c_str());
}

// ---------------- Tracking state ----------------

struct Node  { uint32_t id; int8_t rssi; int8_t snr; uint32_t lastMs; };
struct Burst { uint32_t freqKhz; int8_t rssi; uint8_t sf; uint32_t lastMs; };

static Node  s_nodes[MAX_NODES];
static Burst s_bursts[MAX_BURSTS];
static uint16_t s_meshPackets = 0;
static uint16_t s_loraPackets = 0;
static uint32_t s_lastHitMs = 0;
static char s_lastHit[24] = "listening...";

static int8_t  s_sweep[LORA_SPECTRUM_BINS];   // latest sweep
static int8_t  s_peak[LORA_SPECTRUM_BINS];    // max since last report
static int8_t  s_water[WATERFALL_ROWS][LORA_SPECTRUM_BINS];
static uint8_t s_waterHead = 0, s_waterCount = 0;
static int8_t  s_noise = -127;
static float   s_baseline = 0;
static bool    s_haveBaseline = false;
static bool    s_jamming = false;
static uint8_t s_jamCount = 0, s_clearCount = 0;
static uint32_t s_sweeps = 0;

static volatile bool s_rxFlag = false;
static void IRAM_ATTR onRxDone() { s_rxFlag = true; }

static uint32_t binKhz(int i) {
    uint32_t span = (uint32_t)((band.endMHz - band.startMHz) * 1000.0f);
    uint32_t step = span / LORA_SPECTRUM_BINS;
    return (uint32_t)(band.startMHz * 1000.0f) + step / 2 + i * step;
}

static void noteNode(uint32_t id, int8_t rssi, int8_t snr) {
    uint32_t now = millis();
    int slot = -1, oldest = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        if (s_nodes[i].id == id) { slot = i; break; }
        if (s_nodes[i].lastMs < s_nodes[oldest].lastMs) oldest = i;
    }
    if (slot < 0) slot = oldest;
    s_nodes[slot] = {id, rssi, snr, now};
}

static void noteBurst(uint32_t freqKhz, int8_t rssi, uint8_t sf) {
    uint32_t now = millis();
    int slot = -1, oldest = 0;
    for (int i = 0; i < MAX_BURSTS; i++) {
        Burst& b = s_bursts[i];
        if (b.lastMs && b.sf == sf && (b.freqKhz > freqKhz ? b.freqKhz - freqKhz : freqKhz - b.freqKhz) < 300) {
            slot = i; break;
        }
        if (s_bursts[i].lastMs < s_bursts[oldest].lastMs) oldest = i;
    }
    if (slot < 0) slot = oldest;
    s_bursts[slot] = {freqKhz, rssi, sf, now};
    if (s_loraPackets < 65535) s_loraPackets++;
}

// ---------------- Radio modes ----------------

static void enterMeshRx() {
    radio.setFrequency(band.meshMHz);
    radio.setBandwidth(MESH_BW);
    radio.setSpreadingFactor(MESH_SF);
    radio.setCodingRate(MESH_CR);
    radio.setPacketReceivedAction(onRxDone);
    s_rxFlag = false;
    radio.startReceive();
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
        // Header is cleartext: to(0..3) from(4..7) id(8..11) flags channel next_hop relay
        uint32_t from = buf[4] | (buf[5] << 8) | (buf[6] << 16) | ((uint32_t)buf[7] << 24);
        if (from != 0) {
            noteNode(from, rssi, snr);
            if (s_meshPackets < 65535) s_meshPackets++;
            snprintf(s_lastHit, sizeof(s_lastHit), "MESH !%08lx %d", (unsigned long)from, rssi);
            Serial.printf("[MESH] from !%08lx len %u rssi %d snr %d\n", (unsigned long)from, (unsigned)len, rssi, snr);
        }
    } else if (st == RADIOLIB_ERR_CRC_MISMATCH) {
        // Something LoRa-shaped on the mesh channel we couldn't decode
        noteBurst((uint32_t)(band.meshMHz * 1000.0f), rssi, MESH_SF);
        snprintf(s_lastHit, sizeof(s_lastHit), "LoRa crc %.1f %d", band.meshMHz, rssi);
    }
    s_lastHitMs = millis();
    digitalWrite(LED_PIN, HIGH);
    radio.startReceive();
}

static int8_t median(const int8_t* v, int n) {
    int8_t tmp[LORA_SPECTRUM_BINS];
    memcpy(tmp, v, n);
    for (int i = 1; i < n; i++) {
        int8_t x = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j] > x) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = x;
    }
    return tmp[n / 2];
}

static void checkHotspot() {
    int best = -1;
    for (int i = 0; i < LORA_SPECTRUM_BINS; i++) {
        if (s_sweep[i] >= s_noise + HOTSPOT_DB && (best < 0 || s_sweep[i] > s_sweep[best])) best = i;
    }
    if (best < 0) return;

    // LoRaWAN-style uplinks: 125 kHz, SF7-SF10. Detach the RX ISR, CAD reuses DIO0.
    float f = binKhz(best) / 1000.0f;
    radio.clearPacketReceivedAction();
    radio.setFrequency(f);
    radio.setBandwidth(125.0f);
    for (uint8_t sf = 7; sf <= 10; sf++) {
        radio.setSpreadingFactor(sf);
        if (radio.scanChannel() == RADIOLIB_PREAMBLE_DETECTED) {
            noteBurst(binKhz(best), s_sweep[best], sf);
            snprintf(s_lastHit, sizeof(s_lastHit), "LoRa %.1f SF%u", f, sf);
            s_lastHitMs = millis();
            Serial.printf("[LORA] CAD hit %.3f MHz SF%u peak %d dBm\n", f, sf, s_sweep[best]);
            break;
        }
    }
}

static void sweep() {
    for (int i = 0; i < LORA_SPECTRUM_BINS; i++) {
        radio.setFrequency(binKhz(i) / 1000.0f);
        radio.startReceive();
        delayMicroseconds(RSSI_SETTLE_US);
        float r = radio.getRSSI(false);
        s_sweep[i] = (int8_t)constrain((int)r, -127, 0);
        if (s_sweep[i] > s_peak[i]) s_peak[i] = s_sweep[i];
    }
    s_sweeps++;

    memcpy(s_water[s_waterHead], s_sweep, LORA_SPECTRUM_BINS);
    s_waterHead = (s_waterHead + 1) % WATERFALL_ROWS;
    if (s_waterCount < WATERFALL_ROWS) s_waterCount++;

    s_noise = median(s_sweep, LORA_SPECTRUM_BINS);
    if (!s_haveBaseline) {
        s_baseline = s_noise;
        s_haveBaseline = true;
    }

    // A jammer lifts the whole band, so judge on the median, not single bins
    if (!s_jamming) {
        if (s_noise >= s_baseline + JAM_DB) {
            if (++s_jamCount >= JAM_SWEEPS) {
                s_jamming = true;
                s_clearCount = 0;
                Serial.printf("[LORA] JAMMING: floor %d dBm vs baseline %.0f\n", s_noise, s_baseline);
            }
        } else {
            s_jamCount = 0;
            if (s_noise < s_baseline + 6) s_baseline += (s_noise - s_baseline) * 0.05f;
        }
    } else if (s_noise < s_baseline + JAM_CLEAR_DB) {
        if (++s_clearCount >= JAM_SWEEPS) {
            s_jamming = false;
            s_jamCount = 0;
            Serial.println("[LORA] jamming cleared");
        }
    } else {
        s_clearCount = 0;
    }

    checkHotspot();
}

// ---------------- Report + display ----------------

static void buildReport(LoraReport& r) {
    static uint16_t seq = 0;
    uint32_t now = millis();
    memset(&r, 0, sizeof(r));
    r.magic = LORA_PROTO_MAGIC;
    r.version = LORA_PROTO_VERSION;
    r.band = LORA_BAND;
    r.seq = ++seq;
    r.uptimeS = now / 1000;
    r.startKhz = binKhz(0);
    r.stepKhz = binKhz(1) - binKhz(0);
    memcpy(r.spectrum, s_sweeps ? s_peak : s_sweep, LORA_SPECTRUM_BINS);
    memcpy(s_peak, s_sweep, LORA_SPECTRUM_BINS);
    r.noiseFloor = s_noise;
    r.baseline = (int8_t)s_baseline;
    r.jamming = s_jamming;
    r.radioOk = s_radioOk;
    r.loraPackets = s_loraPackets;
    r.meshPackets = s_meshPackets;

    // Most recent first: mesh nodes and LoRa bursts heard in the last 10 minutes
    struct Cand { uint32_t lastMs; LoraHit hit; } cands[MAX_NODES + MAX_BURSTS];
    int n = 0;
    for (auto& nd : s_nodes) {
        if (!nd.id || now - nd.lastMs > NODE_WINDOW_MS) continue;
        r.meshNodes++;
        cands[n++] = {nd.lastMs, {(uint32_t)(band.meshMHz * 1000.0f), nd.id, nd.rssi, nd.snr,
                                  MESH_SF, HIT_MESHTASTIC, (uint16_t)((now - nd.lastMs) / 1000)}};
    }
    for (auto& b : s_bursts) {
        if (!b.lastMs || now - b.lastMs > NODE_WINDOW_MS) continue;
        cands[n++] = {b.lastMs, {b.freqKhz, 0, b.rssi, 0, b.sf, HIT_LORA,
                                 (uint16_t)((now - b.lastMs) / 1000)}};
    }
    for (int i = 1; i < n; i++) {
        Cand c = cands[i];
        int j = i - 1;
        while (j >= 0 && cands[j].lastMs < c.lastMs) { cands[j + 1] = cands[j]; j--; }
        cands[j + 1] = c;
    }
    r.hitCount = n < LORA_MAX_HITS ? n : LORA_MAX_HITS;
    for (int i = 0; i < r.hitCount; i++) r.hits[i] = cands[i].hit;
}

static void drawOled(uint8_t meshNodes) {
    char buf[32];
    oled.clearBuffer();
    oled.setFont(u8g2_font_5x8_tr);
    if (!s_radioOk) {
        oled.drawStr(0, 8, "LoRa radio NOT found");
        oled.sendBuffer();
        return;
    }
    snprintf(buf, sizeof(buf), "%s nf%d M%u", band.name, s_noise, meshNodes);
    oled.drawStr(0, 7, buf);
    if (s_jamming) {
        oled.drawBox(98, 0, 30, 8);
        oled.setDrawColor(0);
        oled.drawStr(100, 7, "JAM!");
        oled.setDrawColor(1);
    } else {
        snprintf(buf, sizeof(buf), "#%lu", (unsigned long)s_sweeps % 1000);
        oled.drawStr(104, 7, buf);
    }

    // Waterfall, newest row on top; 2 px per bin, brighter = more above the floor
    for (int r = 0; r < s_waterCount; r++) {
        const int8_t* row = s_water[(s_waterHead + WATERFALL_ROWS - 1 - r) % WATERFALL_ROWS];
        int y = 10 + r;
        for (int b = 0; b < LORA_SPECTRUM_BINS; b++) {
            int v = row[b] - s_noise;
            if (v > 5)  oled.drawPixel(b * 2, y);
            if (v > 12) oled.drawPixel(b * 2 + 1, y);
        }
    }
    oled.drawHLine(0, 51, 128);
    oled.drawStr(0, 63, s_lastHit);
    oled.sendBuffer();
}

// ---------------- Arduino ----------------

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.printf("\n[LORA] companion v1, band %s, receive-only\n", band.name);
    pinMode(LED_PIN, OUTPUT);

    Wire.begin(OLED_SDA, OLED_SCL);
    oled.begin();
    oled.setFont(u8g2_font_5x8_tr);
    oled.clearBuffer();
    oled.drawStr(0, 10, "LoRa companion");
    oled.drawStr(0, 22, "starting...");
    oled.sendBuffer();

    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
    int st = radio.begin(band.meshMHz, MESH_BW, MESH_SF, MESH_CR, MESH_SYNC, 2, MESH_PREAMBLE, 0);
    if (st == RADIOLIB_ERR_NONE) st = radio.setCRC(true);
    s_radioOk = st == RADIOLIB_ERR_NONE;
    Serial.printf("[LORA] radio.begin: %d (%s)\n", st, s_radioOk ? "SX127x found" : "FAILED");
    memset(s_sweep, -127, sizeof(s_sweep));
    memset(s_peak, -127, sizeof(s_peak));
    if (s_radioOk) enterMeshRx();

    startEspNow();
}

void loop() {
    uint32_t now = millis();
    static uint32_t lastSweep = 0, lastReport = 0, lastOled = 0, lastLog = 0;
    static uint8_t meshNodes = 0;

    if (s_radioOk) {
        if (s_rxFlag) handlePacket();

        // Only sweep when nothing is mid-air, or we'd cut a Meshtastic packet off
        bool busy = radio.getModemStatus() & 0x0B;  // signal detected / synced / header valid
        if (now - lastSweep > SWEEP_INTERVAL_MS && !busy && !s_rxFlag) {
            sweep();
            enterMeshRx();
            lastSweep = millis();
        }
    }
    if (now - s_lastHitMs > 80) digitalWrite(LED_PIN, LOW);

    if (now - lastReport >= REPORT_INTERVAL_MS) {
        lastReport = now;
        LoraReport r;
        buildReport(r);
        meshNodes = r.meshNodes;
        publish(r);
    }
    if (now - lastOled >= OLED_INTERVAL_MS) {
        lastOled = now;
        drawOled(meshNodes);
    }
    if (now - lastLog >= 10000) {
        lastLog = now;
        Serial.printf("[LORA] sweeps %lu nf %d base %.0f jam %d mesh %u pkts / %u nodes, lora %u\n",
                      (unsigned long)s_sweeps, s_noise, s_baseline, s_jamming,
                      s_meshPackets, meshNodes, s_loraPackets);
    }
    delay(2);
}
