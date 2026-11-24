#pragma once
#include "config.h"

enum class DetectionType : uint8_t {
    UNKNOWN = 0,
    AIRTAG,
    FINDMY,
    SAMSUNG_TAG,
    TILE_TRACKER,
    GOOGLE_TAG,
    FLIPPER_ZERO,
    FLOCK_CAMERA,
    AXON_BODYCAM,
    DRONE_REMOTEID,
    ALPR,
    CAMERA,
    CARD_SKIMMER,
    RING_DEVICE,
    SMART_SPEAKER,
    DEAUTH_FLOOD,
    EVIL_TWIN,
    PWNAGOTCHI,
    HACKER_TOOL,
    META_GLASSES,
    IBEACON,
    BLE_SPAM,
    CUSTOM_ALERT,
    LORA_JAMMING,
    COUNT
};

enum class Confidence : uint8_t {
    CONF_LOW = 0,
    CONF_MED,
    CONF_HIGH
};

enum class Source : uint8_t {
    BLE = 0,
    AP,
    CLIENT,
    SYSTEM
};

struct Detection {
    uint8_t  mac[6];
    int8_t   rssi;
    uint8_t  channel;
    Source   src;
    DetectionType type;
    Confidence conf;
    bool     encrypted;
    bool     following;
    bool     ignored;
    bool     used;
    const char* vendor;
    char     name[33];
    uint32_t firstSeen;
    uint32_t lastSeen;
    uint32_t trackStart;
    uint32_t lastAlertMs;
    uint16_t hits;
    uint8_t  alerts;
    int8_t   quietBar;
    int8_t   spark[SIGNAL_TRAIL_LEN];
    uint8_t  sparkHead;
    uint8_t  sparkCount;
};

const char* detectionTypeName(DetectionType t);
const char* confidenceName(Confidence c);
const char* sourceName(Source s);
bool isTrackerType(DetectionType t);

class DetectionEngine {
public:
    void init();
    void loop();

    void startBLE();
    void stopBLE();
    void startWiFi();
    void stopWiFi();
    bool bleRunning() const { return _bleRunning; }
    bool wifiRunning() const { return _wifiRunning; }
    uint8_t wifiChannel() const { return _wifiChannel; }

    // Hold the sniffer on one channel (hunt mode); 0 resumes hopping
    void lockChannel(uint8_t ch);

    Detection* at(int i) { return &_det[i]; }
    int capacity() const { return MAX_DETECTIONS; }
    int find(const uint8_t* mac, Source src) const;
    void clearAll();

    int countActive(Source src) const;
    int countAlerts() const;
    int topAlert() const;

    // Index of a detection that just crossed the alert bar, or -1
    int popAlert();

    // Reported by the LoRa companion while the whole band is raised
    void raiseLoraJamming(int8_t floorDbm, int8_t baselineDbm);

    uint32_t framesSeen() const { return _frames; }
    uint32_t advertsSeen() const { return _adverts; }

private:
    Detection _det[MAX_DETECTIONS];
    bool _bleRunning = false;
    bool _wifiRunning = false;
    bool _wifiInit = false;
    uint8_t _wifiChannel = 1;
    uint8_t _lockedChannel = 0;
    uint32_t _lastHopMs = 0;
    uint32_t _lastSparkMs = 0;
    uint32_t _deauthWindowMs = 0;
    uint32_t _spamWindowMs = 0;
    uint16_t _spamNew = 0;
    uint32_t _spamActiveUntil = 0;
    uint32_t _frames = 0;
    uint32_t _adverts = 0;

    int _alertQueue[8];
    uint8_t _alertHead = 0, _alertTail = 0;

    int allocate(const uint8_t* mac, Source src);
    void drainEvents();
    void checkDeauth(uint32_t now);
    void checkEvilTwin(int idx);
    void raiseSystem(const uint8_t* mac, DetectionType t, const char* vendor, const char* name, int8_t rssi);
    void maybeAlert(int idx, bool force);
    void pushAlert(int idx);
    void hop();
};

extern DetectionEngine detector;
