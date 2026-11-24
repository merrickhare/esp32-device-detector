#pragma once
#include <Arduino.h>

// WiFi client features used only by the desktop screen: time (SNTP) and weather (Open-Meteo).
// The radio is joined to the home network only while the desktop is open; the detector's
// sniffers are stopped by the caller first and restored after stopDesktop().
struct Weather {
    bool valid;
    float temp, hi, lo, wind;
    int humidity;
    int code;        // WMO weather code
    bool day;
    uint32_t stamp;  // millis() of the fetch, changes on every new reading
};

enum class NetState : uint8_t { IDLE, NO_SETUP, CONNECTING, SYNCING, LIVE, FAILED };

class NetSync {
public:
    void begin();
    bool configured() const { return _ssid[0] != 0; }

    void startDesktop();
    void stopDesktop();   // blocks until the link is down (a few seconds at most)

    NetState state() const { return _state; }
    const char* failReason() const { return _fail; }
    bool weather(Weather& out) const;
    const char* place() const { return _place; }
    bool fahrenheit() const { return _unitF; }
    bool timeValid() const;
    void demoNext();

    // Setup portal (blocking helpers driven from main.cpp)
    void portalBegin(char* apPass, size_t n);
    void portalLoop();
    bool portalSaved() const { return _portalSaved; }

    // Internal (task + web handlers)
    void taskBody();
    void handleRoot();
    void handleSave();

private:
    bool httpGet(const char* url, String& body);
    bool locate();
    bool fetchWeather();

    char _ssid[33] = "";
    char _pass[64] = "";
    char _city[41] = "";
    bool _unitF = true;
    float _lat = 0, _lon = 0;
    bool _haveLoc = false;
    char _place[32] = "";
    int32_t _tzOffset = 0;
    bool _sntpStarted = false;

    volatile bool _active = false;
    volatile bool _busy = false;
    volatile NetState _state = NetState::IDLE;
    const char* _fail = "";
    Weather _wx = {};
    String _scanList;
    volatile bool _portalSaved = false;
};

extern NetSync netSync;
