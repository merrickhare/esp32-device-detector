#include "detection.h"
#include "signatures.h"
#include "pcap.h"
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

DetectionEngine detector;

using DT = DetectionType;
using CF = Confidence;

static const char* typeNames[] = {
    "Unknown", "AirTag", "Find My", "Samsung Tag", "Tile", "Google Tag",
    "Flipper Zero", "Flock Camera", "Axon BWC", "Drone", "ALPR", "Camera",
    "Card Skimmer", "Ring", "Smart Speaker", "Deauth Flood", "Evil Twin",
    "Pwnagotchi", "Hacker Tool", "Meta Glasses", "iBeacon", "BLE Spam",
    "Custom Alert", "LoRa Jamming"
};
static_assert(sizeof(typeNames) / sizeof(typeNames[0]) == (size_t)DT::COUNT,
              "typeNames out of sync with DetectionType");

const char* detectionTypeName(DetectionType t) {
    uint8_t i = (uint8_t)t;
    return i < (uint8_t)DT::COUNT ? typeNames[i] : "Unknown";
}

const char* confidenceName(Confidence c) {
    switch (c) {
        case CF::CONF_LOW:  return "LOW";
        case CF::CONF_MED:  return "MED";
        case CF::CONF_HIGH: return "HIGH";
    }
    return "?";
}

const char* sourceName(Source s) {
    switch (s) {
        case Source::BLE:         return "BLE";
        case Source::AP:     return "WiFi AP";
        case Source::CLIENT: return "WiFi client";
        case Source::SYSTEM:      return "System";
    }
    return "?";
}

bool isTrackerType(DetectionType t) {
    return t == DT::AIRTAG || t == DT::SAMSUNG_TAG || t == DT::TILE_TRACKER || t == DT::GOOGLE_TAG;
}

const char* deviceOSName(DeviceOS os) {
    switch (os) {
        case DeviceOS::OS_IOS:     return "iOS";
        case DeviceOS::OS_ANDROID: return "Android";
        case DeviceOS::OS_WINDOWS: return "Windows";
        case DeviceOS::OS_SAMSUNG: return "Samsung";
        case DeviceOS::OS_LINUX:   return "Linux";
        default:                   return nullptr;
    }
}

// --- Radio → main loop mailbox ---
// Radio callbacks run in the NimBLE / WiFi tasks; they classify and post here,
// and only loop() touches the detection table.

enum EvKind : uint8_t { EV_BLE, EV_AP, EV_PROBE };
enum EvFlag : uint8_t { F_SPAM = 1, F_ENC = 2, F_HIDDEN = 4 };

struct RadioEvent {
    uint8_t kind;
    uint8_t flags;
    uint8_t mac[6];
    int8_t  rssi;
    uint8_t channel;
    DT      type;
    CF      conf;
    DeviceOS os;
    const char* vendor;
    char    name[33];
};

static QueueHandle_t s_queue = nullptr;
static volatile uint32_t s_frames = 0;
static volatile uint32_t s_adverts = 0;
static volatile uint32_t s_deauthCount = 0;
static volatile int8_t   s_deauthRssi = 0;
static uint8_t s_deauthMac[6];

struct Verdict {
    DT type = DT::UNKNOWN;
    CF conf = CF::CONF_LOW;
    const char* vendor = nullptr;

    void offer(DT t, CF c, const char* v) {
        if (t == DT::UNKNOWN) {
            if (!vendor) vendor = v;
            return;
        }
        if (type == DT::UNKNOWN || (uint8_t)c > (uint8_t)conf) {
            type = t;
            conf = c;
            vendor = v;
        }
    }
};

static void post(const RadioEvent& ev) {
    if (s_queue) xQueueSend(s_queue, &ev, 0);
}

static bool isRandomMac(const uint8_t* mac) {
    return mac[0] & 0x02;
}

// --- WiFi sniffer ---

static void wifiCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    s_frames++;

    const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
    const uint8_t* f = pkt->payload;
    int len = (int)pkt->rx_ctrl.sig_len - 4;  // strip FCS
    if (len < 24) return;

    if (pcapLog.running()) pcapLog.writePacket(f, len);

    uint8_t sub = (f[0] >> 4) & 0x0F;
    const uint8_t* addr2 = f + 10;
    const uint8_t* addr3 = f + 16;

    // Deauth (12) / disassoc (10)
    if (sub == 12 || sub == 10) {
        s_deauthCount++;
        s_deauthRssi = pkt->rx_ctrl.rssi;
        memcpy(s_deauthMac, addr2, 6);
        return;
    }

    RadioEvent ev = {};
    ev.rssi = pkt->rx_ctrl.rssi;
    ev.channel = pkt->rx_ctrl.channel;
    Verdict v;

    if (sub == 4) {
        // Probe request: a client device looking for networks
        ev.kind = EV_PROBE;
        memcpy(ev.mac, addr2, 6);
        int pos = 24;
        if (pos + 2 <= len && f[pos] == 0) {
            uint8_t tl = f[pos + 1];
            if (tl > 0 && tl <= 32 && pos + 2 + tl <= len) {
                memcpy(ev.name, f + pos + 2, tl);
                ev.name[tl] = 0;
            }
        }
    } else if (sub == 8 || sub == 5) {
        // Beacon / probe response: an access point
        if (len < 36) return;
        ev.kind = EV_AP;
        memcpy(ev.mac, addr3, 6);
        uint16_t cap = f[34] | (f[35] << 8);
        if (cap & 0x0010) ev.flags |= F_ENC;

        bool gotSsid = false;
        int pos = 36;
        while (pos + 2 <= len) {
            uint8_t tag = f[pos];
            uint8_t tl = f[pos + 1];
            const uint8_t* d = f + pos + 2;
            if (pos + 2 + tl > len) break;
            if (tag == 0 && !gotSsid) {
                gotSsid = true;
                if (tl == 0 || d[0] == 0) {
                    ev.flags |= F_HIDDEN;
                } else if (tl <= 32) {
                    memcpy(ev.name, d, tl);
                    ev.name[tl] = 0;
                }
            } else if (tag == 3 && tl == 1) {
                ev.channel = d[0];
            } else if (tag == 221 && tl >= 4 &&
                       d[0] == 0xFA && d[1] == 0x0B && d[2] == 0xBC && d[3] == 0x0D) {
                v.offer(DT::DRONE_REMOTEID, CF::CONF_HIGH, "WiFi RemoteID");
            }
            pos += 2 + tl;
        }

        static const uint8_t pwn[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD};
        if (memcmp(addr2, pwn, 6) == 0) {
            v.offer(DT::PWNAGOTCHI, CF::CONF_HIGH, "Pwnagotchi");
        }
        if (ev.name[0]) {
            const char* vv = nullptr; CF cc = CF::CONF_LOW;
            v.offer(Signatures::lookupSsid(ev.name, &vv, &cc), cc, vv);
        }
    } else {
        return;
    }

    if (!isRandomMac(ev.mac)) {
        const char* vv = nullptr; CF cc = CF::CONF_LOW;
        DT t = Signatures::lookupOui(ev.mac, &vv, &cc);
        v.offer(t, cc, vv);
    }

    ev.type = v.type;
    ev.conf = v.conf;
    ev.vendor = v.vendor;
    post(ev);
}

// --- BLE scanner ---

class BleCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        s_adverts++;

        RadioEvent ev = {};
        ev.kind = EV_BLE;
        // NimBLE stores the address little-endian
        const uint8_t* a = dev->getAddress().getBase()->val;
        for (int i = 0; i < 6; i++) ev.mac[i] = a[5 - i];
        ev.rssi = dev->getRSSI();
        bool isPublic = dev->getAddressType() == BLE_ADDR_PUBLIC;

        Verdict v;
        const std::vector<uint8_t>& p = dev->getPayload();
        size_t n = p.size();
        size_t i = 0;
        while (i + 1 < n) {
            uint8_t len = p[i];
            if (len == 0 || i + 1 + len > n) break;
            uint8_t t = p[i + 1];
            const uint8_t* d = &p[i + 2];
            uint8_t dl = len - 1;

            if ((t == 0x08 || t == 0x09) && dl > 0) {
                uint8_t c = dl < 32 ? dl : 32;
                memcpy(ev.name, d, c);
                ev.name[c] = 0;
            } else if (t == 0x02 || t == 0x03) {
                for (uint8_t k = 0; k + 1 < dl; k += 2) {
                    const char* vv = nullptr; CF cc = CF::CONF_LOW;
                    DT tt = Signatures::lookupUuid16(d[k] | (d[k + 1] << 8), &vv, &cc);
                    v.offer(tt, cc, vv);
                }
            } else if (t == 0x16 && dl >= 2) {
                uint16_t uuid = d[0] | (d[1] << 8);
                if (uuid == 0xFEAA) {
                    // Eddystone frame 0x40/0x41 = Google Find Hub tracker
                    if (dl >= 3 && (d[2] == 0x40 || d[2] == 0x41)) {
                        v.offer(DT::GOOGLE_TAG, CF::CONF_HIGH, "Google Find Hub");
                    } else {
                        v.offer(DT::IBEACON, CF::CONF_LOW, "Eddystone");
                    }
                } else {
                    const char* vv = nullptr; CF cc = CF::CONF_LOW;
                    DT tt = Signatures::lookupUuid16(uuid, &vv, &cc);
                    v.offer(tt, cc, vv);
                }
            } else if (t == 0xFF && dl >= 2) {
                uint16_t cid = d[0] | (d[1] << 8);
                const uint8_t* m = d + 2;
                uint8_t ml = dl - 2;
                if (cid == 0x004C && ml >= 2) {
                    uint8_t at = m[0], alen = m[1];
                    if (at == 0x12 && alen >= 0x19) {
                        v.offer(DT::AIRTAG, CF::CONF_HIGH, "Find My (separated)");
                    } else if (at == 0x12) {
                        v.offer(DT::FINDMY, CF::CONF_LOW, "Find My (near owner)");
                    } else if (at == 0x02 && alen == 0x15) {
                        v.offer(DT::IBEACON, CF::CONF_LOW, "iBeacon");
                    } else if ((at == 0x07 || at == 0x0F) && !isPublic) {
                        ev.flags |= F_SPAM;
                    }
                    if (ev.os == DeviceOS::OS_UNKNOWN) ev.os = DeviceOS::OS_IOS;
                } else if (cid == 0x0006) {
                    if (ml >= 1 && m[0] == 0x03 && !isPublic) ev.flags |= F_SPAM;
                    if (ev.os == DeviceOS::OS_UNKNOWN) ev.os = DeviceOS::OS_WINDOWS;
                } else if (cid == 0x0075) {
                    if (ev.os == DeviceOS::OS_UNKNOWN) ev.os = DeviceOS::OS_SAMSUNG;
                } else if (cid == 0x00E0) {
                    if (ev.os == DeviceOS::OS_UNKNOWN) ev.os = DeviceOS::OS_ANDROID;
                }
                const char* vv = nullptr; CF cc = CF::CONF_LOW;
                DT tt = Signatures::lookupMfgId(cid, &vv, &cc);
                v.offer(tt, cc, vv);
            }
            i += 1 + len;
        }

        if (ev.name[0]) {
            const char* vv = nullptr; CF cc = CF::CONF_LOW;
            DT tt = Signatures::lookupBtName(ev.name, &vv, &cc);
            v.offer(tt, cc, vv);
        }
        if (isPublic) {
            const char* vv = nullptr; CF cc = CF::CONF_LOW;
            DT tt = Signatures::lookupOui(ev.mac, &vv, &cc);
            v.offer(tt, cc, vv);
        }

        ev.type = v.type;
        ev.conf = v.conf;
        ev.vendor = v.vendor;
        post(ev);
    }
};

static BleCallbacks bleCallbacks;
static bool s_bleInit = false;

// --- Engine ---

void DetectionEngine::init() {
    memset(_det, 0, sizeof(_det));
    if (!s_queue) s_queue = xQueueCreate(EVENT_QUEUE_LEN, sizeof(RadioEvent));
    _alertHead = _alertTail = 0;
}

void DetectionEngine::startBLE() {
    if (_bleRunning) return;
    if (!s_bleInit) {
        NimBLEDevice::init("");
        s_bleInit = true;
    }
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&bleCallbacks, true);
    scan->setMaxResults(0);
    scan->setActiveScan(true);
    // ~50% duty cycle leaves the shared antenna free for the WiFi sniffer
    scan->setInterval(100);
    scan->setWindow(50);
    _bleRunning = scan->start(0, false, true);
    Serial.printf("[DET] BLE scan %s\n", _bleRunning ? "started" : "FAILED");
}

void DetectionEngine::stopBLE() {
    if (!_bleRunning) return;
    NimBLEDevice::getScan()->stop();
    _bleRunning = false;
    Serial.println("[DET] BLE scan stopped");
}

void DetectionEngine::startWiFi() {
    if (_wifiRunning) return;
    if (!_wifiInit) {
        WiFi.persistent(false);
        WiFi.mode(WIFI_STA);
        esp_wifi_disconnect();
        _wifiInit = true;
    }
    wifi_promiscuous_filter_t flt = {};
    flt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&flt);
    esp_wifi_set_promiscuous_rx_cb(&wifiCallback);
    esp_err_t err = esp_wifi_set_promiscuous(true);
    if (err == ESP_OK) {
        esp_wifi_set_channel(_wifiChannel, WIFI_SECOND_CHAN_NONE);
        _wifiRunning = true;
        _lastHopMs = millis();
    }
    Serial.printf("[DET] WiFi sniffer %s (err=0x%x)\n", _wifiRunning ? "started" : "FAILED", err);
}

void DetectionEngine::stopWiFi() {
    if (!_wifiRunning) return;
    esp_wifi_set_promiscuous(false);
    _wifiRunning = false;
    Serial.println("[DET] WiFi sniffer stopped");
}

void DetectionEngine::lockChannel(uint8_t ch) {
    _lockedChannel = ch;
    if (ch && _wifiRunning) {
        _wifiChannel = ch;
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    }
}

void DetectionEngine::hop() {
    _wifiChannel = _wifiChannel >= WIFI_MAX_CHANNELS ? 1 : _wifiChannel + 1;
    esp_wifi_set_channel(_wifiChannel, WIFI_SECOND_CHAN_NONE);
}

int DetectionEngine::find(const uint8_t* mac, Source src) const {
    for (int i = 0; i < MAX_DETECTIONS; i++) {
        if (_det[i].used && _det[i].src == src && memcmp(_det[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

int DetectionEngine::allocate(const uint8_t* mac, Source src) {
    int slot = -1;
    for (int i = 0; i < MAX_DETECTIONS; i++) {
        if (!_det[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        // Evict the stalest plain device first; threats only when nothing else is left
        uint32_t oldest = UINT32_MAX;
        for (int pass = 0; pass < 2 && slot < 0; pass++) {
            for (int i = 0; i < MAX_DETECTIONS; i++) {
                const Detection& d = _det[i];
                if (pass == 0 && (d.type != DT::UNKNOWN || d.src == Source::SYSTEM)) continue;
                if (d.lastSeen < oldest) { oldest = d.lastSeen; slot = i; }
            }
        }
    }
    Detection& d = _det[slot];
    memset(&d, 0, sizeof(d));
    memcpy(d.mac, mac, 6);
    d.src = src;
    d.used = true;
    d.firstSeen = millis();
    d.trackStart = d.firstSeen;
    d.quietBar = -127;
    d.rssiSmooth = -127;
    return slot;
}

void DetectionEngine::pushAlert(int idx) {
    uint8_t next = (_alertHead + 1) % 8;
    if (next == _alertTail) return;
    _alertQueue[_alertHead] = idx;
    _alertHead = next;
}

int DetectionEngine::popAlert() {
    if (_alertTail == _alertHead) return -1;
    int idx = _alertQueue[_alertTail];
    _alertTail = (_alertTail + 1) % 8;
    return idx;
}

void DetectionEngine::maybeAlert(int idx, bool force) {
    Detection& d = _det[idx];
    if (d.ignored || d.type == DT::UNKNOWN) return;
    if (d.conf == CF::CONF_LOW && !d.following && !force) return;

    uint32_t now = millis();
    bool fire = force || d.alerts == 0 ||
                d.rssi >= d.quietBar + QUIET_MARGIN_DB ||
                now - d.lastAlertMs > QUIET_DECAY_MS;
    if (!fire) return;

    if (d.alerts < 255) d.alerts++;
    d.lastAlertMs = now;
    d.quietBar = d.rssi;
    pushAlert(idx);
}

void DetectionEngine::checkEvilTwin(int idx) {
    Detection& n = _det[idx];
    if (!n.name[0] || strcmp(n.name, "<hidden>") == 0) return;
    for (int i = 0; i < MAX_DETECTIONS; i++) {
        const Detection& o = _det[i];
        if (i == idx || !o.used || o.src != Source::AP) continue;
        if (o.encrypted != n.encrypted && strcmp(o.name, n.name) == 0) {
            n.type = DT::EVIL_TWIN;
            n.conf = n.encrypted ? CF::CONF_MED : CF::CONF_HIGH;
            n.vendor = n.encrypted ? "Same SSID, diff security" : "Open clone of secured SSID";
            return;
        }
    }
}

void DetectionEngine::raiseSystem(const uint8_t* mac, DT t, const char* vendor,
                                  const char* name, int8_t rssi) {
    int idx = find(mac, Source::SYSTEM);
    if (idx < 0) idx = allocate(mac, Source::SYSTEM);
    Detection& d = _det[idx];
    uint32_t now = millis();
    d.type = t;
    d.conf = CF::CONF_HIGH;
    d.vendor = vendor;
    strlcpy(d.name, name, sizeof(d.name));
    d.rssi = rssi;
    d.channel = _wifiChannel;
    d.lastSeen = now;
    if (d.hits < 65535) d.hits++;
    if (d.alerts == 0 || now - d.lastAlertMs > 60000) maybeAlert(idx, true);
}

void DetectionEngine::raiseLoraJamming(int8_t floorDbm, int8_t baselineDbm) {
    static const uint8_t jamMac[6] = {0x02, 0x4C, 0x4A, 0x00, 0x00, 0x01};
    char name[33];
    snprintf(name, sizeof(name), "floor %d dBm vs %d", floorDbm, baselineDbm);
    raiseSystem(jamMac, DT::LORA_JAMMING, "LoRa band jamming", name, floorDbm);
}

void DetectionEngine::checkDeauth(uint32_t now) {
    if (now - _deauthWindowMs < DEAUTH_WINDOW_MS) return;
    uint32_t count = s_deauthCount;
    s_deauthCount = 0;
    _deauthWindowMs = now;
    if (count < DEAUTH_THRESHOLD) return;

    uint8_t mac[6];
    memcpy(mac, s_deauthMac, 6);
    char name[33];
    snprintf(name, sizeof(name), "%lu frames/3s", (unsigned long)count);
    raiseSystem(mac, DT::DEAUTH_FLOOD, "Deauth attack", name, s_deauthRssi);
    Serial.printf("[DET] Deauth flood: %lu frames\n", (unsigned long)count);
}

void DetectionEngine::drainEvents() {
    RadioEvent ev;
    uint32_t now = millis();
    int budget = EVENT_QUEUE_LEN;

    while (budget-- > 0 && xQueueReceive(s_queue, &ev, 0) == pdTRUE) {
        Source src = ev.kind == EV_BLE ? Source::BLE
                   : ev.kind == EV_AP  ? Source::AP
                                       : Source::CLIENT;
        int idx = find(ev.mac, src);
        if (idx < 0) {
            if (ev.flags & F_SPAM) {
                _spamNew++;
                // Don't let a spam burst flush the table
                if (now < _spamActiveUntil) continue;
            }
            idx = allocate(ev.mac, src);
        }

        Detection& d = _det[idx];
        bool isNew = d.hits == 0;
        if (!isNew && now - d.lastSeen > FOLLOW_GAP_MS) {
            d.trackStart = now;
            d.following = false;
        }
        d.rssi = ev.rssi;
        if (d.rssiSmooth == -127)
            d.rssiSmooth = ev.rssi;
        else
            d.rssiSmooth = (int8_t)(RSSI_EMA_ALPHA * ev.rssi +
                                     (1.0f - RSSI_EMA_ALPHA) * d.rssiSmooth);
        d.lastSeen = now;
        if (d.hits < 65535) d.hits++;
        if (ev.channel) d.channel = ev.channel;

        if (ev.name[0]) {
            if (src == Source::CLIENT)
                strlcpy(d.probeSSID, ev.name, sizeof(d.probeSSID));
            else
                strlcpy(d.name, ev.name, sizeof(d.name));
        } else if ((ev.flags & F_HIDDEN) && !d.name[0]) {
            strcpy(d.name, "<hidden>");
        }
        if (src == Source::AP) d.encrypted = ev.flags & F_ENC;
        if (ev.os != DeviceOS::OS_UNKNOWN) d.os = ev.os;

        if (ev.type != DT::UNKNOWN &&
            d.type != DT::EVIL_TWIN &&
            (d.type == DT::UNKNOWN || (uint8_t)ev.conf > (uint8_t)d.conf)) {
            d.type = ev.type;
            d.conf = d.following ? CF::CONF_HIGH : ev.conf;
            d.vendor = ev.vendor;
        } else if (!d.vendor && ev.vendor) {
            d.vendor = ev.vendor;
        }

        if (isNew && src == Source::AP) checkEvilTwin(idx);
        if (d.type != DT::UNKNOWN) maybeAlert(idx, false);
    }
}

void DetectionEngine::loop() {
    uint32_t now = millis();
    _frames = s_frames;
    _adverts = s_adverts;

    if (_wifiRunning && !_lockedChannel && now - _lastHopMs > WIFI_CHANNEL_HOP_MS) {
        hop();
        _lastHopMs = now;
    }

    drainEvents();
    checkDeauth(now);

    if (now - _spamWindowMs > SPAM_WINDOW_MS) {
        if (_spamNew >= SPAM_THRESHOLD) {
            static const uint8_t spamMac[6] = {0x02, 0x00, 0x00, 0x00, 0x5B, 0x1E};
            char name[33];
            snprintf(name, sizeof(name), "%u spoofed devs/5s", _spamNew);
            raiseSystem(spamMac, DT::BLE_SPAM, "BLE spam attack", name, 0);
            _spamActiveUntil = now + 30000;
            Serial.printf("[DET] BLE spam: %u new spoofed MACs\n", _spamNew);
        }
        _spamNew = 0;
        _spamWindowMs = now;
    }

    bool spark = now - _lastSparkMs > SPARK_INTERVAL_MS;
    if (spark) _lastSparkMs = now;

    for (int i = 0; i < MAX_DETECTIONS; i++) {
        Detection& d = _det[i];
        if (!d.used) continue;
        bool active = now - d.lastSeen < ACTIVE_WINDOW_MS;

        if (spark && active) {
            d.spark[d.sparkHead] = d.rssi;
            d.sparkHead = (d.sparkHead + 1) % SIGNAL_TRAIL_LEN;
            if (d.sparkCount < SIGNAL_TRAIL_LEN) d.sparkCount++;
        }

        // Rate-based false positive filter for AirTag-like detections:
        // real AirTags advertise at ~0.5 Hz; phones/AirPods are much faster
        if (d.type == DT::AIRTAG && d.hits >= AIRTAG_RATE_SAMPLES && active) {
            uint32_t elapsed = now - d.firstSeen;
            if (elapsed > 2000) {
                float rate = d.hits * 1000.0f / elapsed;
                if (rate > AIRTAG_RATE_MAX) {
                    d.type = DT::FINDMY;
                    d.conf = CF::CONF_LOW;
                    d.vendor = "Find My (likely phone)";
                }
            }
        }

        // A tracker that has stayed with you for FOLLOW_MS is following you
        if (isTrackerType(d.type) && !d.following && active &&
            now - d.trackStart > FOLLOW_MS) {
            d.following = true;
            d.conf = CF::CONF_HIGH;
            Serial.printf("[DET] %s following for %lus\n", detectionTypeName(d.type),
                          (unsigned long)((now - d.trackStart) / 1000));
            maybeAlert(i, true);
        }
    }
}

int DetectionEngine::countActive(Source src) const {
    uint32_t now = millis();
    int n = 0;
    for (int i = 0; i < MAX_DETECTIONS; i++) {
        const Detection& d = _det[i];
        if (d.used && d.src == src && now - d.lastSeen < ACTIVE_WINDOW_MS) n++;
    }
    return n;
}

int DetectionEngine::countAlerts() const {
    uint32_t now = millis();
    int n = 0;
    for (int i = 0; i < MAX_DETECTIONS; i++) {
        const Detection& d = _det[i];
        if (!d.used || d.type == DT::UNKNOWN || d.ignored) continue;
        if (d.conf == CF::CONF_LOW && !d.following) continue;
        if (now - d.lastSeen < ACTIVE_WINDOW_MS) n++;
    }
    return n;
}

int DetectionEngine::topAlert() const {
    uint32_t now = millis();
    int best = -1;
    int bestScore = -1;
    for (int i = 0; i < MAX_DETECTIONS; i++) {
        const Detection& d = _det[i];
        if (!d.used || d.type == DT::UNKNOWN || d.ignored) continue;
        if (now - d.lastSeen >= ACTIVE_WINDOW_MS) continue;
        int score = (d.following ? 1000 : 0) + (int)d.conf * 200 + (d.rssi + 128);
        if (score > bestScore) { bestScore = score; best = i; }
    }
    return best;
}

void DetectionEngine::clearAll() {
    memset(_det, 0, sizeof(_det));
    _alertHead = _alertTail = 0;
    if (s_queue) xQueueReset(s_queue);
}

// --- Hunt mode with MAC rotation tracking ---

void DetectionEngine::setBLEDuty(uint16_t interval, uint16_t window) {
    if (!_bleRunning) return;
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->stop();
    scan->setInterval(interval);
    scan->setWindow(window);
    _bleRunning = scan->start(0, false, true);
}

void DetectionEngine::startHunt(const uint8_t* mac, Source src) {
    memcpy(_huntMac, mac, 6);
    _huntSrc = src;
    _huntActive = true;
    _huntRotations = 0;

    int idx = find(mac, src);
    if (idx >= 0) {
        _huntType = _det[idx].type;
        _huntLastRssi = _det[idx].rssiSmooth != -127 ? _det[idx].rssiSmooth : _det[idx].rssi;
    } else {
        _huntType = DT::UNKNOWN;
        _huntLastRssi = -127;
    }

    if (!_bleBoosted) {
        setBLEDuty(HUNT_BLE_INTERVAL, HUNT_BLE_WINDOW);
        _bleBoosted = true;
        Serial.println("[DET] BLE boosted for hunt");
    }

    if (HUNT_PAUSE_WIFI && _wifiRunning) {
        stopWiFi();
        _wifiPausedForHunt = true;
        Serial.println("[DET] WiFi paused for hunt (100% BLE antenna)");
    }
}

void DetectionEngine::stopHunt() {
    _huntActive = false;
    if (_bleBoosted) {
        setBLEDuty(NORMAL_BLE_INTERVAL, NORMAL_BLE_WINDOW);
        _bleBoosted = false;
        Serial.println("[DET] BLE duty restored");
    }
    if (_wifiPausedForHunt) {
        startWiFi();
        _wifiPausedForHunt = false;
        Serial.println("[DET] WiFi sniffer resumed");
    }
}

int DetectionEngine::correlateRotation() {
    uint32_t now = millis();
    int best = -1;
    int8_t bestRssi = -127;

    for (int i = 0; i < MAX_DETECTIONS; i++) {
        const Detection& d = _det[i];
        if (!d.used || d.src != _huntSrc) continue;
        if (memcmp(d.mac, _huntMac, 6) == 0) continue;
        if (d.type != _huntType) continue;
        if (now - d.lastSeen > HUNT_STALE_MS) continue;

        int8_t r = d.rssiSmooth != -127 ? d.rssiSmooth : d.rssi;
        int diff = abs((int)r - (int)_huntLastRssi);
        if (diff > HUNT_RSSI_TOLERANCE) continue;

        if (r > bestRssi) {
            bestRssi = r;
            best = i;
        }
    }
    return best;
}

int DetectionEngine::findHuntTarget() {
    if (!_huntActive) return -1;

    int idx = find(_huntMac, _huntSrc);
    uint32_t now = millis();

    if (idx >= 0) {
        Detection& d = _det[idx];
        if (now - d.lastSeen < HUNT_STALE_MS) {
            _huntLastRssi = d.rssiSmooth != -127 ? d.rssiSmooth : d.rssi;
            return idx;
        }
    }

    // Original MAC is stale — look for a rotated replacement
    if (!isTrackerType(_huntType)) return idx >= 0 ? idx : -1;

    int candidate = correlateRotation();
    if (candidate >= 0) {
        Detection& c = _det[candidate];
        memcpy(_huntMac, c.mac, 6);
        _huntLastRssi = c.rssiSmooth != -127 ? c.rssiSmooth : c.rssi;
        _huntRotations++;
        Serial.printf("[DET] Hunt target rotated MAC → %02X:%02X:%02X:%02X:%02X:%02X (#%lu)\n",
                      c.mac[0], c.mac[1], c.mac[2], c.mac[3], c.mac[4], c.mac[5],
                      (unsigned long)_huntRotations);
        return candidate;
    }

    return idx >= 0 ? idx : -1;
}

// --- Stealth mode ---

void DetectionEngine::setStealthMode(bool on) {
    if (on == _stealthActive) return;
    _stealthActive = on;

    if (on) {
        _preStealthBle = _bleRunning;
        _preStealthWifi = _wifiRunning;
        // Switch BLE to passive scan (no SCAN_REQ emissions)
        if (_bleRunning) {
            NimBLEScan* scan = NimBLEDevice::getScan();
            scan->stop();
            scan->setActiveScan(false);
            scan->setInterval(NORMAL_BLE_INTERVAL);
            scan->setWindow(NORMAL_BLE_WINDOW);
            _bleRunning = scan->start(0, false, true);
        }
        // WiFi promiscuous is already passive; LoRa TX stopped by caller
        Serial.println("[DET] Stealth mode ON: passive BLE, no TX");
    } else {
        // Restore active scan
        if (_bleRunning) {
            NimBLEScan* scan = NimBLEDevice::getScan();
            scan->stop();
            scan->setActiveScan(true);
            scan->setInterval(NORMAL_BLE_INTERVAL);
            scan->setWindow(NORMAL_BLE_WINDOW);
            _bleRunning = scan->start(0, false, true);
        }
        if (_preStealthWifi && !_wifiRunning) startWiFi();
        Serial.println("[DET] Stealth mode OFF: active scan restored");
    }
}
