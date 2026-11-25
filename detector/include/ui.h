#pragma once
#include <TFT_eSPI.h>
#include "config.h"
#include "detection.h"

// RGB565 palette
#define C_BG       0x0000
#define C_ROW_ALT  0x0841
#define C_PANEL    0x10A2
#define C_BTN      0x2945
#define C_FG       0xFFFF
#define C_MUTED    0x8410
#define C_DIM      0x4208
#define C_ACCENT   0x07FF
#define C_OK       0x07E0
#define C_LOW      0xFFE0
#define C_MED      0xFD20
#define C_HIGH     0xF800
#define C_DANGER   0xF800
// Palette (white / red / navy / gold / cyan)
#define C_NAVY     0x10C8
#define C_ZRED     0xD8A3
#define C_ZGOLD    0xF5E5
#define C_ZCYAN    0x07FF
#define C_ZCYDIM   0x032C

enum class Screen : uint8_t { BOOT, HOME, LIST, DETAIL, HUNT, SETTINGS, CALIBRATE, LORA, DESKTOP, PORTAL };
enum class ListMode : uint8_t { ALERTS, BLE, WIFI };

struct Button {
    int16_t x, y, w, h;
    const char* label;
    uint16_t color;
};

// Shared layout: main.cpp hit-tests the same rects ui.cpp draws
namespace Layout {
    static const Button HOME_PANEL   = {6, 44, 228, 80, nullptr, 0};
    static const Button HOME_TILE_BLE  = {6, 130, 72, 44, nullptr, 0};
    static const Button HOME_TILE_WIFI = {84, 130, 72, 44, nullptr, 0};
    static const Button HOME_TILE_THR  = {162, 130, 72, 44, nullptr, 0};
    static const Button HOME_ALERTS  = {6, 196, 228, 40, "THREATS", C_BTN};
    static const Button HOME_BLE     = {6, 242, 111, 36, "BLE LIST", C_BTN};
    static const Button HOME_WIFI    = {123, 242, 111, 36, "WIFI LIST", C_BTN};
    static const Button HOME_SET     = {6, 284, 74, 32, "SETTINGS", C_BTN};
    static const Button HOME_LORA    = {83, 284, 74, 32, "LORA", C_BTN};
    static const Button HOME_DESK    = {160, 284, 74, 32, "DESKTOP", C_NAVY};

    static const int LIST_Y0 = 42;
    static const int LIST_ROW_H = 32;
    static const int LIST_ROWS = 7;
    static const Button LIST_BACK = {0, 272, 78, 44, "< BACK", C_BTN};
    static const Button LIST_UP   = {81, 272, 78, 44, nullptr, C_BTN};
    static const Button LIST_DOWN = {162, 272, 78, 44, nullptr, C_BTN};

    static const Button DET_BACK   = {0, 272, 78, 44, "< BACK", C_BTN};
    static const Button DET_HUNT   = {81, 272, 78, 44, "HUNT", C_BTN};
    static const Button DET_IGNORE = {162, 272, 78, 44, nullptr, C_BTN};

    static const Button HUNT_BACK  = {0, 280, 240, 36, "< BACK", C_BTN};

    static const Button SET_BLE    = {6, 44, 111, 32, nullptr, C_BTN};
    static const Button SET_WIFI   = {123, 44, 111, 32, nullptr, C_BTN};
    static const Button SET_DIM    = {6, 80, 111, 32, "BRIGHT -", C_BTN};
    static const Button SET_BRIGHT = {123, 80, 111, 32, "BRIGHT +", C_BTN};
    static const Button SET_POPUP  = {6, 116, 111, 32, nullptr, C_BTN};
    static const Button SET_CAL    = {123, 116, 111, 32, "CALIBRATE", C_BTN};
    static const Button SET_UNIGN  = {6, 152, 111, 30, "UN-IGNORE", C_BTN};
    static const Button SET_CLEAR  = {123, 152, 111, 30, "CLEAR ALL", C_BTN};
    static const Button SET_STEALTH = {6, 186, 111, 30, nullptr, C_BTN};
    static const Button SET_PCAP   = {123, 186, 111, 30, nullptr, C_BTN};
    static const Button SET_NET    = {6, 220, 228, 24, "WIFI SETUP (DESKTOP)", C_NAVY};
    static const Button PORTAL_CANCEL = {0, 280, 240, 36, "CANCEL", C_BTN};

    static const Button LORA_BACK  = {0, 280, 240, 36, "< BACK", C_BTN};
    static const Button SET_BACK   = {0, 280, 240, 36, "< BACK", C_BTN};

    static const Button POPUP = {10, 86, 220, 150, nullptr, 0};
}

class Display {
public:
    void init();
    void showBoot();

    void setBrightness(uint8_t b);
    uint8_t brightness() const { return _brightness; }
    void dimCheck();
    bool wake();          // true if the screen was dimmed (swallow that tap)
    void flash();         // full brightness for an alert

    Screen screen() const { return _screen; }
    void go(Screen s);    // switch screen and full redraw
    void redraw();        // full redraw of the current screen
    void refresh();       // partial redraw of the current screen

    void drawPopup(const Detection& d);
    void showCalTarget(int16_t x, int16_t y, int step, int total);
    void showMessage(const char* line1, const char* line2, uint16_t color);
    void showPortal(const char* apPass);

    // LoRa hunt: track a Meshtastic node by ID
    void startLoraHunt(uint32_t nodeId);
    bool loraHuntActive() const { return _loraHuntActive; }

    static bool hit(const Button& b, int16_t x, int16_t y);

    // List state
    ListMode listMode = ListMode::ALERTS;
    int listScroll = 0;
    int listCount = 0;
    int16_t listRows[MAX_DETECTIONS];
    void buildList();
    void scrollList(int dir);
    int rowAt(int16_t y) const;  // detection index under a tap, or -1

    // Selected device (by identity, since table slots get reused)
    void select(int idx);
    int selected() const;

    bool popupsOn = true;
    const char* resetInfo = "";

private:
    // Change caches so periodic refreshes repaint only what changed
    struct RowCache { int16_t idx; int8_t rssi; uint8_t flags; };
    RowCache _rowCache[Layout::LIST_ROWS];
    int _listHdrKey = -1;
    bool _sbDirty = true;
    uint32_t _loraDrawnAcc = 0;
    int _homeKey = -2;
    int _tileVals[3] = {-1, -1, -1};
    char _detCache[8][40];
    int _detIdx = -2;
    uint8_t _detSpark = 255;
    int16_t _huntRssiShown = 999;
    int8_t _huntTrendShown = -2;

    TFT_eSPI _tft = TFT_eSPI();
    Screen _screen = Screen::BOOT;
    uint8_t _brightness = BACKLIGHT_DEFAULT;
    uint32_t _lastTouchMs = 0;
    bool _dimmed = false;

    uint8_t _selMac[6] = {0};
    Source _selSrc = Source::BLE;
    bool _selValid = false;

    // Hunt history (timestamped samples for trend + history bars)
    struct HuntSample { int8_t rssi; uint32_t ms; };
    HuntSample _huntSamples[HUNT_HISTORY_LEN];
    uint8_t _huntSampleHead = 0, _huntSampleCount = 0;
    uint32_t _huntLastSeen = 0;
    int8_t _peakRssi = -127;

    // LoRa hunt (track a Meshtastic node by ID)
    uint32_t _loraHuntNode = 0;
    bool _loraHuntActive = false;

    // Battery
    uint32_t _battReadMs = 0;
    uint16_t _battSamples[BATT_SAMPLES] = {};
    uint8_t  _battIdx = 0, _battCount = 0;
    int8_t   _battPercent = -1;  // -1 = no battery detected

    // Rate display
    uint32_t _rateMs = 0, _rateFrames = 0, _rateAdverts = 0;
    uint16_t _fps = 0, _aps = 0;

    void drawStatusBar();
    void drawButton(const Button& b, const char* label = nullptr, uint16_t color = 0);
    void drawArrow(const Button& b, bool up);
    void drawBars(int16_t x, int16_t y, int8_t rssi);
    void drawSpark(int16_t x, int16_t y, int16_t w, int16_t h, const Detection& d);

    void drawHome(bool full);
    void drawList(bool full);
    void drawDetail(bool full);
    void drawHunt(bool full);
    void drawSettings(bool full);
    void drawLora(bool full);
    void drawDesktop(bool full);
    void freeDesktop();
    void drawRadar(uint32_t t);
    void drawWxIcon(uint32_t t, int code, bool day);
    void drawWxText(bool full);
    TFT_eSprite* _radar = nullptr;
    TFT_eSprite* _wxIcon = nullptr;
    int _clockSec = -1;
    uint32_t _secBase = 0;
    uint32_t _wxStamp = 1;
    uint8_t _netShown = 255;

    // Mech animation (desktop panel)
    struct MechStep { uint8_t frame; uint16_t ms; };
    MechStep _mechSeq[40];
    uint8_t _mechLen = 0, _mechPos = 0;
    uint32_t _mechStepAt = 0, _mechNextAction = 0;
    bool _beamOn = false;
    void mechStartIdle(uint32_t t);
    void mechStartAction(uint32_t t);
    void drawMechPanel(uint32_t t);
    void mechBootScene();
    void mechStrike(uint16_t bg);

    uint16_t colorFor(const Detection& d) const;
    void labelFor(const Detection& d, char* out, size_t n) const;
};

extern Display ui;

void fmtMac(const uint8_t* mac, char* out);
void fmtAgo(uint32_t ms, char* out, size_t n);
