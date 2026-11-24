#include "ui.h"
#include "sdlog.h"
#include "loralink.h"

Display ui;

static const uint16_t C_COLD = 0x041F;

void fmtMac(const uint8_t* m, char* out) {
    sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

void fmtAgo(uint32_t ms, char* out, size_t n) {
    uint32_t s = ms / 1000;
    if (s < 60)        snprintf(out, n, "%lus", (unsigned long)s);
    else if (s < 3600) snprintf(out, n, "%lum", (unsigned long)(s / 60));
    else               snprintf(out, n, "%luh%02lum", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60));
}

static void fmtUptime(char* out, size_t n) {
    uint32_t s = millis() / 1000;
    snprintf(out, n, "%02lu:%02lu:%02lu", (unsigned long)(s / 3600),
             (unsigned long)((s / 60) % 60), (unsigned long)(s % 60));
}

static uint16_t proximityColor(int8_t rssi) {
    if (rssi > -50) return C_HIGH;
    if (rssi > -60) return C_MED;
    if (rssi > -70) return C_LOW;
    if (rssi > -80) return C_OK;
    return C_COLD;
}

static const char* proximityLabel(int8_t rssi) {
    if (rssi > -45) return "ON TOP OF IT";
    if (rssi > -55) return "VERY CLOSE";
    if (rssi > -65) return "CLOSE";
    if (rssi > -75) return "NEARBY";
    if (rssi > -85) return "FAR";
    return "VERY FAR";
}

// --- Basics ---

void Display::init() {
    _tft.init();
    _tft.setRotation(0);
    _tft.fillScreen(C_BG);
    _tft.setTextDatum(TL_DATUM);

    ledcSetup(BACKLIGHT_CHANNEL, BACKLIGHT_FREQ, BACKLIGHT_RES);
    ledcAttachPin(PIN_TFT_LITE, BACKLIGHT_CHANNEL);
    setBrightness(_brightness);
    _lastTouchMs = millis();
}

void Display::setBrightness(uint8_t b) {
    _brightness = b < 20 ? 20 : b;
    if (!_dimmed) ledcWrite(BACKLIGHT_CHANNEL, _brightness);
}

void Display::dimCheck() {
    if (!_dimmed && millis() - _lastTouchMs > DIM_TIMEOUT_MS) {
        ledcWrite(BACKLIGHT_CHANNEL, BACKLIGHT_DIM);
        _dimmed = true;
    }
}

bool Display::wake() {
    _lastTouchMs = millis();
    if (!_dimmed) return false;
    _dimmed = false;
    ledcWrite(BACKLIGHT_CHANNEL, _brightness);
    return true;
}

void Display::flash() {
    _lastTouchMs = millis();
    _dimmed = false;
    ledcWrite(BACKLIGHT_CHANNEL, _brightness);
}

bool Display::hit(const Button& b, int16_t x, int16_t y) {
    return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

void Display::select(int idx) {
    Detection* d = detector.at(idx);
    memcpy(_selMac, d->mac, 6);
    _selSrc = d->src;
    _selValid = true;
}

int Display::selected() const {
    return _selValid ? detector.find(_selMac, _selSrc) : -1;
}

void Display::go(Screen s) {
    if (_screen == Screen::HUNT && s != Screen::HUNT) detector.lockChannel(0);
    if (_screen == Screen::DESKTOP && s != Screen::DESKTOP) freeDesktop();
    _screen = s;

    if (s == Screen::LIST) buildList();
    if (s == Screen::HUNT) {
        memset(_hunt, -127, sizeof(_hunt));
        _huntHead = _huntCount = 0;
        _huntLastSeen = 0;
        int idx = selected();
        if (idx >= 0) {
            Detection* d = detector.at(idx);
            if (d->src == Source::AP && d->channel) detector.lockChannel(d->channel);
        }
    }
    redraw();
}

void Display::redraw() {
    _sbDirty = true;
    switch (_screen) {
        case Screen::HOME:     drawHome(true); break;
        case Screen::LIST:     drawList(true); break;
        case Screen::DETAIL:   drawDetail(true); break;
        case Screen::HUNT:     drawHunt(true); break;
        case Screen::SETTINGS: drawSettings(true); break;
        case Screen::LORA:     drawLora(true); break;
        case Screen::DESKTOP:  drawDesktop(true); break;
        default: break;
    }
}

void Display::refresh() {
    switch (_screen) {
        case Screen::HOME:     drawHome(false); break;
        case Screen::LIST:     buildList(); drawList(false); break;
        case Screen::DETAIL:   drawDetail(false); break;
        case Screen::HUNT:     drawHunt(false); break;
        case Screen::SETTINGS: drawSettings(false); break;
        case Screen::LORA:     drawLora(false); break;
        case Screen::DESKTOP:  drawDesktop(false); break;
        default: break;
    }
}

// --- Widgets ---

void Display::drawStatusBar() {
    if (_sbDirty) {
        _tft.fillRect(0, 0, SCREEN_W, 18, C_PANEL);
        _sbDirty = false;
    }
    _tft.setTextDatum(TL_DATUM);
    _tft.setTextSize(1);

    _tft.setTextColor(detector.bleRunning() ? C_OK : C_DIM, C_PANEL);
    _tft.drawString("BLE", 4, 5, 1);
    _tft.setTextColor(detector.wifiRunning() ? C_OK : C_DIM, C_PANEL);
    _tft.drawString("WiFi", 28, 5, 1);

    char buf[16];
    snprintf(buf, sizeof(buf), "ch%-2u", detector.wifiChannel());
    _tft.setTextColor(C_MUTED, C_PANEL);
    _tft.drawString(buf, 58, 5, 1);

    _tft.setTextColor(sdLog.isReady() ? C_OK : C_DIM, C_PANEL);
    _tft.drawString("SD", 88, 5, 1);

    bool jam = loraLink.online() && loraLink.latest().jamming;
    _tft.setTextColor(jam ? C_DANGER : loraLink.online() ? C_OK : C_DIM, C_PANEL);
    _tft.drawString("LR", 106, 5, 1);

    int threats = detector.countAlerts();
    if (threats > 0) snprintf(buf, sizeof(buf), "!%-3d", threats);
    else strcpy(buf, "    ");
    _tft.setTextColor(C_DANGER, C_PANEL);
    _tft.drawString(buf, 126, 5, 1);

    fmtUptime(buf, sizeof(buf));
    _tft.setTextDatum(TR_DATUM);
    _tft.setTextColor(C_MUTED, C_PANEL);
    _tft.drawString(buf, SCREEN_W - 4, 5, 1);
    _tft.setTextDatum(TL_DATUM);
}

void Display::drawButton(const Button& b, const char* label, uint16_t color) {
    uint16_t bg = color ? color : b.color;
    _tft.fillRoundRect(b.x + 1, b.y + 1, b.w - 2, b.h - 2, 5, bg);
    _tft.drawRoundRect(b.x + 1, b.y + 1, b.w - 2, b.h - 2, 5, C_DIM);
    const char* l = label ? label : b.label;
    if (l) {
        _tft.setTextDatum(MC_DATUM);
        _tft.setTextColor(C_FG, bg);
        _tft.drawString(l, b.x + b.w / 2, b.y + b.h / 2 + 1, 2);
        _tft.setTextDatum(TL_DATUM);
    }
}

void Display::drawArrow(const Button& b, bool up) {
    drawButton(b);
    int cx = b.x + b.w / 2, cy = b.y + b.h / 2;
    if (up) _tft.fillTriangle(cx, cy - 8, cx - 11, cy + 7, cx + 11, cy + 7, C_FG);
    else    _tft.fillTriangle(cx, cy + 8, cx - 11, cy - 7, cx + 11, cy - 7, C_FG);
}

void Display::drawBars(int16_t x, int16_t y, int8_t rssi) {
    int bars = rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -78 ? 2 : rssi > -88 ? 1 : 0;
    for (int i = 0; i < 4; i++) {
        int h = 4 + i * 3;
        _tft.fillRect(x + i * 5, y + 13 - h, 3, h, i < bars ? C_ACCENT : C_DIM);
    }
}

void Display::drawSpark(int16_t x, int16_t y, int16_t w, int16_t h, const Detection& d) {
    _tft.fillRect(x + 1, y + 1, w - 2, h - 2, C_BG);
    _tft.drawRect(x, y, w, h, C_DIM);
    if (d.sparkCount < 2) {
        _tft.setTextDatum(MC_DATUM);
        _tft.setTextColor(C_MUTED, C_BG);
        _tft.drawString("collecting...", x + w / 2, y + h / 2, 1);
        _tft.setTextDatum(TL_DATUM);
        return;
    }
    int step = (w - 8) / (SIGNAL_TRAIL_LEN - 1);
    int px = 0, py = 0;
    for (uint8_t i = 0; i < d.sparkCount; i++) {
        uint8_t idx = (d.sparkHead + SIGNAL_TRAIL_LEN - d.sparkCount + i) % SIGNAL_TRAIL_LEN;
        int v = constrain(d.spark[idx], -100, -30);
        int sx = x + 4 + i * step;
        int sy = y + h - 4 - (v + 100) * (h - 8) / 70;
        if (i > 0) _tft.drawLine(px, py, sx, sy, C_ACCENT);
        _tft.fillCircle(sx, sy, 2, proximityColor(d.spark[idx]));
        px = sx; py = sy;
    }
}

uint16_t Display::colorFor(const Detection& d) const {
    if (d.ignored) return C_MUTED;
    if (d.following) return C_DANGER;
    if (d.type == DetectionType::UNKNOWN) return C_DIM;
    switch (d.conf) {
        case Confidence::CONF_HIGH: return C_HIGH;
        case Confidence::CONF_MED:  return C_MED;
        default:                    return C_LOW;
    }
}

void Display::labelFor(const Detection& d, char* out, size_t n) const {
    if (d.type != DetectionType::UNKNOWN) {
        strlcpy(out, detectionTypeName(d.type), n);
        return;
    }
    switch (d.src) {
        case Source::BLE:
            strlcpy(out, d.name[0] ? d.name : d.vendor ? d.vendor : "BLE device", n);
            break;
        case Source::AP:
            strlcpy(out, d.name[0] ? d.name : "<hidden>", n);
            break;
        case Source::CLIENT:
            if (d.name[0]) snprintf(out, n, "Probe: %s", d.name);
            else strlcpy(out, d.vendor ? d.vendor : "WiFi client", n);
            break;
        default:
            strlcpy(out, d.name, n);
    }
}

static void fitText(TFT_eSPI& tft, char* s, int maxW, int font) {
    size_t len = strlen(s);
    if (tft.textWidth(s, font) <= maxW) return;
    while (len > 2) {
        s[--len] = 0;
        s[len - 1] = '.';
        if (tft.textWidth(s, font) <= maxW) return;
    }
}

// --- Screens ---

void Display::drawHome(bool full) {
    uint32_t now = millis();
    if (full) {
        _tft.fillScreen(C_BG);
        _tft.setTextDatum(MC_DATUM);
        _tft.setTextColor(C_ACCENT, C_BG);
        _tft.drawString(BRAND_TITLE, 120, 31, 2);
        _tft.setTextDatum(TL_DATUM);
        drawButton(Layout::HOME_ALERTS);
        drawButton(Layout::HOME_BLE);
        drawButton(Layout::HOME_WIFI);
        drawButton(Layout::HOME_SET);
        drawButton(Layout::HOME_LORA);
        drawButton(Layout::HOME_DESK);
    }
    drawStatusBar();

    if (now - _rateMs >= 1000) {
        uint32_t dt = now - _rateMs;
        _fps = (detector.framesSeen() - _rateFrames) * 1000 / dt;
        _aps = (detector.advertsSeen() - _rateAdverts) * 1000 / dt;
        _rateFrames = detector.framesSeen();
        _rateAdverts = detector.advertsSeen();
        _rateMs = now;
    }

    if (full) {
        _homeKey = -2;
        for (int& v : _tileVals) v = -1;
    }

    // Threat panel: full repaint only when the headline threat changes
    const Button& p = Layout::HOME_PANEL;
    int top = detector.topAlert();
    char buf[48];
    int key = -1;
    if (top >= 0) {
        const Detection& d = *detector.at(top);
        key = top * 8 + (int)d.conf * 2 + (d.following ? 1 : 0);
    }
    if (key == _homeKey && top >= 0) {
        const Detection& d = *detector.at(top);
        char ago[12];
        fmtAgo(now - d.lastSeen, ago, sizeof(ago));
        snprintf(buf, sizeof(buf), "%d dBm  %s  seen %s ago", d.rssi, sourceName(d.src), ago);
        _tft.fillRect(p.x + 8, p.y + 53, p.w - 16, 10, C_PANEL);
        _tft.setTextColor(C_MUTED, C_PANEL);
        _tft.drawString(buf, p.x + 8, p.y + 54, 1);
    } else if (key != _homeKey) {
      _homeKey = key;
      _tft.fillRoundRect(p.x, p.y, p.w, p.h, 6, C_PANEL);
      if (top >= 0) {
        const Detection& d = *detector.at(top);
        uint16_t col = colorFor(d);
        _tft.drawRoundRect(p.x, p.y, p.w, p.h, 6, col);
        _tft.drawRoundRect(p.x + 1, p.y + 1, p.w - 2, p.h - 2, 5, col);

        strlcpy(buf, detectionTypeName(d.type), sizeof(buf));
        fitText(_tft, buf, 160, 4);
        _tft.setTextColor(col, C_PANEL);
        _tft.drawString(buf, p.x + 8, p.y + 6, 4);

        _tft.setTextDatum(TR_DATUM);
        _tft.drawString(confidenceName(d.conf), p.x + p.w - 8, p.y + 10, 2);
        _tft.setTextDatum(TL_DATUM);

        strlcpy(buf, d.name[0] ? d.name : d.vendor ? d.vendor : "", sizeof(buf));
        if (d.name[0] && d.vendor) snprintf(buf, sizeof(buf), "%s - %s", d.vendor, d.name);
        fitText(_tft, buf, 210, 2);
        _tft.setTextColor(C_FG, C_PANEL);
        _tft.drawString(buf, p.x + 8, p.y + 34, 2);

        char ago[12];
        fmtAgo(now - d.lastSeen, ago, sizeof(ago));
        snprintf(buf, sizeof(buf), "%d dBm  %s  seen %s ago", d.rssi, sourceName(d.src), ago);
        _tft.setTextColor(C_MUTED, C_PANEL);
        _tft.drawString(buf, p.x + 8, p.y + 54, 1);

        if (d.following) {
            _tft.setTextColor(C_DANGER, C_PANEL);
            _tft.drawString("FOLLOWING YOU", p.x + 8, p.y + 66, 1);
        } else {
            _tft.setTextColor(C_DIM, C_PANEL);
            _tft.drawString("tap for details", p.x + 8, p.y + 66, 1);
        }
    } else {
        _tft.drawRoundRect(p.x, p.y, p.w, p.h, 6, C_OK);
        _tft.setTextDatum(MC_DATUM);
        _tft.setTextColor(C_OK, C_PANEL);
        _tft.drawString("ALL CLEAR", 120, p.y + 30, 4);
        _tft.setTextColor(C_MUTED, C_PANEL);
        _tft.drawString("No threats nearby", 120, p.y + 58, 2);
        _tft.setTextDatum(TL_DATUM);
      }
    }

    // Stat tiles
    struct { const Button* b; int n; const char* label; uint16_t col; } tiles[] = {
        {&Layout::HOME_TILE_BLE, detector.countActive(Source::BLE), "BLE", C_ACCENT},
        {&Layout::HOME_TILE_WIFI, detector.countActive(Source::AP) +
                                  detector.countActive(Source::CLIENT), "WIFI", C_ACCENT},
        {&Layout::HOME_TILE_THR, detector.countAlerts(), "THREATS", C_OK},
    };
    if (tiles[2].n > 0) tiles[2].col = C_DANGER;
    for (int i = 0; i < 3; i++) {
        auto& t = tiles[i];
        if (t.n == _tileVals[i]) continue;
        _tileVals[i] = t.n;
        _tft.fillRoundRect(t.b->x, t.b->y, t.b->w, t.b->h, 5, C_PANEL);
        _tft.setTextDatum(MC_DATUM);
        snprintf(buf, sizeof(buf), "%d", t.n);
        _tft.setTextColor(t.col, C_PANEL);
        _tft.drawString(buf, t.b->x + t.b->w / 2, t.b->y + 17, 4);
        _tft.setTextColor(C_MUTED, C_PANEL);
        _tft.drawString(t.label, t.b->x + t.b->w / 2, t.b->y + 37, 1);
        _tft.setTextDatum(TL_DATUM);
    }

    // Radio activity, so it's obvious the sniffers are alive (padded so no clear is needed)
    snprintf(buf, sizeof(buf), "   WiFi %3u frm/s   BLE %3u adv/s   ", _fps, _aps);
    _tft.setTextDatum(MC_DATUM);
    _tft.setTextColor(C_MUTED, C_BG);
    _tft.drawString(buf, 120, 185, 1);
    _tft.setTextDatum(TL_DATUM);
}

void Display::buildList() {
    uint32_t now = millis();
    listCount = 0;
    int32_t score[MAX_DETECTIONS];

    for (int i = 0; i < MAX_DETECTIONS; i++) {
        const Detection& d = *detector.at(i);
        if (!d.used) continue;
        bool active = now - d.lastSeen < ACTIVE_WINDOW_MS;
        bool typed = d.type != DetectionType::UNKNOWN;
        bool keep = false;
        switch (listMode) {
            case ListMode::ALERTS: keep = typed; break;
            case ListMode::BLE:    keep = active && d.src == Source::BLE; break;
            case ListMode::WIFI:   keep = active && (d.src == Source::AP ||
                                                     d.src == Source::CLIENT); break;
        }
        if (!keep) continue;

        int32_t s = d.rssi + 128;
        if (typed) s += 1000 + (int32_t)d.conf * 1000;
        if (d.following) s += 10000;
        if (active) s += 100000;
        if (d.ignored) s -= 200000;
        score[listCount] = s;
        listRows[listCount] = i;
        listCount++;
    }

    for (int i = 1; i < listCount; i++) {
        int32_t s = score[i];
        int16_t r = listRows[i];
        int j = i - 1;
        while (j >= 0 && score[j] < s) {
            score[j + 1] = score[j];
            listRows[j + 1] = listRows[j];
            j--;
        }
        score[j + 1] = s;
        listRows[j + 1] = r;
    }

    int maxScroll = listCount > Layout::LIST_ROWS ? listCount - Layout::LIST_ROWS : 0;
    if (listScroll > maxScroll) listScroll = maxScroll;
    if (listScroll < 0) listScroll = 0;
}

void Display::scrollList(int dir) {
    listScroll += dir * (Layout::LIST_ROWS - 1);
    int maxScroll = listCount > Layout::LIST_ROWS ? listCount - Layout::LIST_ROWS : 0;
    if (listScroll > maxScroll) listScroll = maxScroll;
    if (listScroll < 0) listScroll = 0;
}

int Display::rowAt(int16_t y) const {
    if (y < Layout::LIST_Y0 || y >= Layout::LIST_Y0 + Layout::LIST_ROWS * Layout::LIST_ROW_H) return -1;
    int k = listScroll + (y - Layout::LIST_Y0) / Layout::LIST_ROW_H;
    return k < listCount ? listRows[k] : -1;
}

void Display::drawList(bool full) {
    uint32_t now = millis();
    if (full) {
        _tft.fillScreen(C_BG);
        drawButton(Layout::LIST_BACK);
        drawArrow(Layout::LIST_UP, true);
        drawArrow(Layout::LIST_DOWN, false);
    }
    drawStatusBar();

    char buf[48];
    if (full) {
        _listHdrKey = -1;
        for (auto& c : _rowCache) c.idx = -2;
    }
    int hdrKey = listCount * 1000 + listScroll;
    bool hdrChanged = hdrKey != _listHdrKey;
    if (hdrChanged) {
        _listHdrKey = hdrKey;
        _tft.fillRect(0, 18, SCREEN_W, Layout::LIST_Y0 - 18, C_BG);
        const char* title = listMode == ListMode::ALERTS ? "THREATS"
                          : listMode == ListMode::BLE    ? "BLE DEVICES" : "WIFI";
        _tft.setTextColor(C_ACCENT, C_BG);
        _tft.drawString(title, 6, 22, 2);
        if (listCount > 0) {
            int last = listScroll + Layout::LIST_ROWS;
            if (last > listCount) last = listCount;
            snprintf(buf, sizeof(buf), "%d-%d of %d", listScroll + 1, last, listCount);
        } else {
            strcpy(buf, "0");
        }
        _tft.setTextDatum(TR_DATUM);
        _tft.setTextColor(C_MUTED, C_BG);
        _tft.drawString(buf, SCREEN_W - 6, 24, 2);
        _tft.setTextDatum(TL_DATUM);
    }

    for (int r = 0; r < Layout::LIST_ROWS; r++) {
        int y = Layout::LIST_Y0 + r * Layout::LIST_ROW_H;
        uint16_t bg = (r & 1) ? C_ROW_ALT : C_BG;
        int k = listScroll + r;
        RowCache& rc = _rowCache[r];

        if (k >= listCount) {
            if (rc.idx != -1) {
                _tft.fillRect(0, y, SCREEN_W, Layout::LIST_ROW_H, bg);
                rc.idx = -1;
            }
            continue;
        }
        const Detection& d = *detector.at(listRows[k]);
        bool typed = d.type != DetectionType::UNKNOWN;
        bool active = now - d.lastSeen < ACTIVE_WINDOW_MS;
        uint8_t flags = (d.following ? 1 : 0) | (d.ignored ? 2 : 0) | (active ? 4 : 0) |
                        ((uint8_t)d.conf << 3) | (typed ? 32 : 0);

        char ago[12];
        fmtAgo(now - d.lastSeen, ago, sizeof(ago));
        if (rc.idx == listRows[k] && rc.rssi == d.rssi && rc.flags == flags) {
            // Same device, same reading: only the age ticks
            _tft.fillRect(196, y + 20, 42, 10, bg);
            _tft.setTextDatum(TR_DATUM);
            _tft.setTextColor(C_DIM, bg);
            _tft.drawString(ago, SCREEN_W - 4, y + 21, 1);
            _tft.setTextDatum(TL_DATUM);
            continue;
        }
        rc.idx = listRows[k];
        rc.rssi = d.rssi;
        rc.flags = flags;

        _tft.fillRect(0, y, SCREEN_W, Layout::LIST_ROW_H, bg);
        _tft.fillRect(0, y + 2, 4, Layout::LIST_ROW_H - 4, colorFor(d));

        labelFor(d, buf, sizeof(buf));
        fitText(_tft, buf, 150, 2);
        uint16_t fg = d.ignored || !active ? C_MUTED : typed ? colorFor(d) : C_FG;
        _tft.setTextColor(fg, bg);
        _tft.drawString(buf, 8, y + 2, 2);

        snprintf(buf, sizeof(buf), "%d", d.rssi);
        _tft.setTextDatum(TR_DATUM);
        _tft.setTextColor(active ? C_FG : C_MUTED, bg);
        _tft.drawString(buf, 212, y + 3, 2);
        _tft.setTextDatum(TL_DATUM);
        drawBars(217, y + 3, active ? d.rssi : -127);

        char mac[18];
        fmtMac(d.mac, mac);
        if (d.following) {
            snprintf(buf, sizeof(buf), "FOLLOWING  %s", d.vendor ? d.vendor : mac);
        } else if (typed) {
            snprintf(buf, sizeof(buf), "%s%s%s", d.vendor ? d.vendor : mac,
                     d.ignored ? " (ignored)" : "", "");
        } else if (d.src == Source::AP) {
            snprintf(buf, sizeof(buf), "%s ch%u %s", mac, d.channel, d.encrypted ? "WPA" : "OPEN");
        } else {
            snprintf(buf, sizeof(buf), "%s %s", mac, d.vendor ? d.vendor : "");
        }
        buf[32] = 0;
        _tft.setTextColor(d.following ? C_DANGER : C_MUTED, bg);
        _tft.drawString(buf, 8, y + 21, 1);

        _tft.setTextDatum(TR_DATUM);
        _tft.setTextColor(C_DIM, bg);
        _tft.drawString(ago, SCREEN_W - 4, y + 21, 1);
        _tft.setTextDatum(TL_DATUM);
    }

    if (listCount == 0 && hdrChanged) {
        _tft.setTextDatum(MC_DATUM);
        _tft.setTextColor(C_MUTED, C_BG);
        _tft.drawString(listMode == ListMode::ALERTS ? "No threats seen yet" : "Nothing nearby yet",
                        120, 140, 2);
        _tft.drawString("Scanning...", 120, 162, 2);
        _tft.setTextDatum(TL_DATUM);
    }
}

void Display::drawDetail(bool full) {
    uint32_t now = millis();
    int idx = selected();
    if (full) {
        _tft.fillScreen(C_BG);
        drawButton(Layout::DET_BACK);
    }
    drawStatusBar();

    if (idx < 0) {
        _tft.setTextDatum(MC_DATUM);
        _tft.setTextColor(C_MUTED, C_BG);
        _tft.drawString("Device no longer tracked", 120, 140, 2);
        _tft.setTextDatum(TL_DATUM);
        return;
    }
    const Detection& d = *detector.at(idx);
    bool typed = d.type != DetectionType::UNKNOWN;
    uint16_t col = typed ? colorFor(d) : C_ACCENT;

    if (full) {
        drawButton(Layout::DET_HUNT);
        drawButton(Layout::DET_IGNORE, d.ignored ? "UNIGNORE" : "IGNORE");
    }

    if (full) {
        _detIdx = -2;
        _detSpark = 255;
        for (auto& c : _detCache) c[0] = 0;
    }

    char buf[48], val[48];
    int hdrKey = idx * 64 + (int)d.type * 2 + (d.following ? 1 : 0) + (int)d.conf * 8192;
    if (hdrKey != _detIdx) {
        _detIdx = hdrKey;
        _tft.fillRect(0, 18, SCREEN_W, 32, C_BG);
        strlcpy(buf, typed ? detectionTypeName(d.type) :
                     d.src == Source::BLE ? "BLE Device" : sourceName(d.src), sizeof(buf));
        fitText(_tft, buf, 170, 4);
        _tft.setTextColor(col, C_BG);
        _tft.drawString(buf, 6, 23, 4);
        if (typed) {
            _tft.setTextDatum(TR_DATUM);
            _tft.drawString(confidenceName(d.conf), SCREEN_W - 6, 28, 2);
            _tft.setTextDatum(TL_DATUM);
        }
    }

    char mac[18], ago[12], ago2[12];
    fmtMac(d.mac, mac);
    fmtAgo(now - d.lastSeen, ago, sizeof(ago));
    fmtAgo(now - d.firstSeen, ago2, sizeof(ago2));

    const char* labels[8] = {"Name", "Vendor", "MAC", "Radio", "Signal", "Seen", "First", "Status"};
    for (int i = 0; i < 8; i++) {
        int y = 52 + i * 17;
        uint16_t vc = C_FG;
        switch (i) {
            case 0: strlcpy(val, d.name[0] ? d.name : "-", sizeof(val)); break;
            case 1: strlcpy(val, d.vendor ? d.vendor : "-", sizeof(val)); break;
            case 2:
                snprintf(val, sizeof(val), "%s%s", mac,
                         (d.src != Source::SYSTEM && (d.mac[0] & 0x02)) ? " rnd" : "");
                break;
            case 3:
                if (d.src == Source::AP)
                    snprintf(val, sizeof(val), "WiFi AP  ch%u  %s", d.channel, d.encrypted ? "WPA" : "OPEN");
                else if (d.src == Source::CLIENT)
                    snprintf(val, sizeof(val), "WiFi client  ch%u", d.channel);
                else
                    strlcpy(val, sourceName(d.src), sizeof(val));
                break;
            case 4:
                snprintf(val, sizeof(val), "%d dBm", d.rssi);
                vc = proximityColor(d.rssi);
                break;
            case 5: snprintf(val, sizeof(val), "%u hits, %s ago", d.hits, ago); break;
            case 6:
                if (isTrackerType(d.type))
                    snprintf(val, sizeof(val), "%s ago (with you %lum)", ago2,
                             (unsigned long)((now - d.trackStart) / 60000));
                else
                    snprintf(val, sizeof(val), "%s ago", ago2);
                break;
            case 7:
                if (d.following)      { strcpy(val, "FOLLOWING YOU"); vc = C_DANGER; }
                else if (d.ignored)   { strcpy(val, "Ignored"); vc = C_MUTED; }
                else if (d.alerts)    snprintf(val, sizeof(val), "Alerted %u time%s", d.alerts, d.alerts == 1 ? "" : "s");
                else if (typed)       strcpy(val, "Logged (low confidence)");
                else                  strcpy(val, "Not a known threat");
                break;
        }
        fitText(_tft, val, SCREEN_W - 70, 2);
        if (strncmp(val, _detCache[i], sizeof(_detCache[i]) - 1) == 0) continue;
        strlcpy(_detCache[i], val, sizeof(_detCache[i]));

        _tft.fillRect(0, y, SCREEN_W, 17, C_BG);
        _tft.setTextColor(C_MUTED, C_BG);
        _tft.drawString(labels[i], 6, y, 2);
        _tft.setTextColor(vc, C_BG);
        _tft.drawString(val, 64, y, 2);
    }
    drawBars(SCREEN_W - 26, 52 + 4 * 17 + 1, d.rssi);

    if (full) {
        _tft.setTextColor(C_MUTED, C_BG);
        snprintf(buf, sizeof(buf), "Signal trail (%us per point)", SPARK_INTERVAL_MS / 1000);
        _tft.drawString(buf, 6, 192, 1);
    }
    uint8_t sparkKey = d.sparkHead | (d.sparkCount << 4);
    if (sparkKey != _detSpark) {
        _detSpark = sparkKey;
        drawSpark(6, 204, 228, 62, d);
    }
}

void Display::drawHunt(bool full) {
    uint32_t now = millis();
    int idx = selected();
    if (full) {
        _tft.fillScreen(C_BG);
        drawButton(Layout::HUNT_BACK);
    }
    drawStatusBar();
    if (idx < 0) {
        _tft.setTextDatum(MC_DATUM);
        _tft.setTextColor(C_MUTED, C_BG);
        _tft.drawString("Device no longer tracked", 120, 140, 2);
        _tft.setTextDatum(TL_DATUM);
        return;
    }
    const Detection& d = *detector.at(idx);
    char buf[48];

    if (full) {
        _huntRssiShown = 999;
        _huntTrendShown = -2;
        labelFor(d, buf, sizeof(buf));
        fitText(_tft, buf, 220, 2);
        _tft.setTextDatum(MC_DATUM);
        _tft.setTextColor(d.type != DetectionType::UNKNOWN ? colorFor(d) : C_ACCENT, C_BG);
        _tft.drawString(buf, 120, 28, 2);
        fmtMac(d.mac, buf);
        _tft.setTextColor(C_MUTED, C_BG);
        _tft.drawString(buf, 120, 45, 1);
        _tft.setTextDatum(TL_DATUM);
    }

    // Record a sample per refresh; -127 marks "not heard this tick"
    _hunt[_huntHead] = d.lastSeen != _huntLastSeen ? d.rssi : -127;
    _huntLastSeen = d.lastSeen;
    _huntHead = (_huntHead + 1) % sizeof(_hunt);
    if (_huntCount < sizeof(_hunt)) _huntCount++;

    uint32_t age = now - d.lastSeen;
    bool lost = age > 5000;
    int8_t rssi = d.rssi;
    uint16_t col = lost ? C_DIM : proximityColor(rssi);

    // Big RSSI readout (font 7 is digits only, so draw the minus sign by hand)
    int16_t shownKey = lost ? 1000 : rssi;
    if (shownKey != _huntRssiShown) {
        _huntRssiShown = shownKey;
        _tft.fillRect(0, 54, SCREEN_W, 54, C_BG);
        snprintf(buf, sizeof(buf), "%d", rssi < 0 ? -rssi : rssi);
        int w = _tft.textWidth(buf, 7);
        int x = (SCREEN_W - w - 16 - 30) / 2 + 16;
        _tft.setTextColor(col, C_BG);
        if (rssi < 0) _tft.fillRect(x - 16, 79, 11, 5, col);
        _tft.drawString(buf, x, 56, 7);
        _tft.drawString("dBm", x + w + 4, 88, 2);

        _tft.fillRect(0, 108, SCREEN_W, 30, C_BG);
        _tft.setTextDatum(MC_DATUM);
        _tft.setTextColor(col, C_BG);
        _tft.drawString(lost ? "SIGNAL LOST" : proximityLabel(rssi), 120, 123, 4);
        _tft.setTextDatum(TL_DATUM);
    }

    // Hot/cold meter
    int lit = lost ? 0 : constrain(map(rssi, -100, -35, 0, 22), 0, 22);
    for (int i = 0; i < 22; i++) {
        uint16_t c = i < 7 ? C_COLD : i < 12 ? C_OK : i < 17 ? C_LOW : i < 20 ? C_MED : C_HIGH;
        _tft.fillRect(10 + i * 10, 142, 8, 18, i < lit ? c : C_DIM);
    }

    // Trend: last 4 heard samples vs the 4 before
    int recent = 0, older = 0, nr = 0, no = 0;
    for (int i = 0; i < _huntCount && (nr < 4 || no < 4); i++) {
        int8_t s = _hunt[(_huntHead + sizeof(_hunt) - 1 - i) % sizeof(_hunt)];
        if (s == -127) continue;
        if (nr < 4) { recent += s; nr++; }
        else        { older += s; no++; }
    }
    int8_t trend = -1;
    if (nr == 4 && no == 4) {
        int diff = (recent - older) / 4;
        trend = diff >= 3 ? 1 : diff <= -3 ? 2 : 0;
    }
    if (trend != _huntTrendShown) {
        _huntTrendShown = trend;
        _tft.fillRect(0, 163, SCREEN_W, 20, C_BG);
        _tft.setTextDatum(MC_DATUM);
        switch (trend) {
            case 1:  _tft.setTextColor(C_OK, C_BG);    _tft.drawString("GETTING CLOSER", 120, 173, 2); break;
            case 2:  _tft.setTextColor(C_MED, C_BG);   _tft.drawString("MOVING AWAY", 120, 173, 2); break;
            case 0:  _tft.setTextColor(C_MUTED, C_BG); _tft.drawString("STEADY", 120, 173, 2); break;
            default: _tft.setTextColor(C_MUTED, C_BG); _tft.drawString("walk around slowly...", 120, 173, 2);
        }
        _tft.setTextDatum(TL_DATUM);
    }

    // Rolling history, painted column by column so nothing is blanked first
    const int gx = 10, gy = 188, gh = 72;
    const int cols = sizeof(_hunt);
    for (int j = 0; j < cols; j++) {
        int8_t s = -127;
        int age = cols - 1 - j;
        if (age < _huntCount) s = _hunt[(_huntHead + cols - 1 - age) % cols];
        int bh = s == -127 ? 0 : 2 + (constrain(s, -100, -30) + 100) * (gh - 2) / 70;
        int cx = gx + j * 4;
        if (bh < gh) _tft.fillRect(cx, gy, 3, gh - bh, C_BG);
        if (bh > 0)  _tft.fillRect(cx, gy + gh - bh, 3, bh, proximityColor(s));
    }
    if (full) _tft.drawFastHLine(gx, gy + gh, 220, C_DIM);

    char ago[12];
    fmtAgo(age, ago, sizeof(ago));
    if (d.src == Source::AP)
        snprintf(buf, sizeof(buf), "locked ch%u   heard %s ago   x%u", d.channel, ago, d.hits);
    else
        snprintf(buf, sizeof(buf), "%s   heard %s ago   x%u", sourceName(d.src), ago, d.hits);
    // Centre inside a fixed 38-char field so the old text is overwritten, not blanked
    char line[40];
    int len = strlen(buf);
    if (len > 38) len = 38;
    int pad = (38 - len) / 2;
    snprintf(line, sizeof(line), "%*s%.*s%*s", pad, "", len, buf, 38 - len - pad, "");
    _tft.setTextColor(C_MUTED, C_BG);
    _tft.drawString(line, 6, 265, 1);
}

void Display::drawSettings(bool full) {
    if (full) {
        _tft.fillScreen(C_BG);
        _tft.setTextColor(C_ACCENT, C_BG);
        _tft.drawString("SETTINGS", 6, 24, 2);

        bool ble = detector.bleRunning(), wifi = detector.wifiRunning();
        drawButton(Layout::SET_BLE, ble ? "BLE: ON" : "BLE: OFF", ble ? 0x0320 : 0x6000);
        drawButton(Layout::SET_WIFI, wifi ? "WIFI: ON" : "WIFI: OFF", wifi ? 0x0320 : 0x6000);
        drawButton(Layout::SET_DIM);
        drawButton(Layout::SET_BRIGHT);
        drawButton(Layout::SET_POPUP, popupsOn ? "POPUPS: ON" : "POPUPS: OFF", popupsOn ? 0x0320 : 0x6000);
        drawButton(Layout::SET_CAL);
        drawButton(Layout::SET_UNIGN);
        drawButton(Layout::SET_CLEAR);
        drawButton(Layout::SET_NET);
        drawButton(Layout::SET_BACK);
    }
    drawStatusBar();

    char buf[48];
    char lines[3][48];
    if (sdLog.isReady()) snprintf(lines[0], 48, "SD: %s  (%lu alerts)", sdLog.fileName(), (unsigned long)sdLog.lines());
    else strcpy(lines[0], "SD: not found");
    snprintf(lines[1], 48, "Heap %uKB (min %uKB)  Bright %d%%", ESP.getFreeHeap() / 1024,
             ESP.getMinFreeHeap() / 1024, _brightness * 100 / 255);
    strlcpy(lines[2], resetInfo, 48);
    for (int i = 0; i < 3; i++) {
        snprintf(buf, sizeof(buf), "%-38.38s", lines[i]);
        _tft.setTextColor(i == 2 && strstr(resetInfo, "CRASH") ? C_DANGER : C_MUTED, C_BG);
        _tft.drawString(buf, 6, 252 + i * 9, 1);
    }
}

static uint16_t heatColor(int marginDb) {
    if (marginDb < 4)  return C_BG;
    if (marginDb < 8)  return 0x000F;
    if (marginDb < 12) return C_COLD;
    if (marginDb < 18) return C_OK;
    if (marginDb < 24) return C_LOW;
    if (marginDb < 30) return C_MED;
    return C_HIGH;
}

// Writes a fixed-width line so a refresh overwrites the old text without blanking
static void padLine(TFT_eSPI& tft, const char* text, int y, uint16_t color) {
    char line[40];
    snprintf(line, sizeof(line), "%-38.38s", text);
    tft.setTextColor(color, C_BG);
    tft.drawString(line, 6, y, 1);
}

void Display::drawLora(bool full) {
    uint32_t now = millis();
    const LoraReport& r = loraLink.latest();
    static const char* bandNames[] = {"433", "868", "915"};
    static const uint32_t meshKhz[] = {433875, 869525, 906875};
    char buf[64];

    if (full) {
        _tft.fillScreen(C_BG);
        drawButton(Layout::LORA_BACK);
        _loraDrawnAcc = 0;
    }
    drawStatusBar();

    _tft.setTextColor(C_ACCENT, C_BG);
    snprintf(buf, sizeof(buf), "LORA %s MHz", loraLink.everSeen() ? bandNames[r.band] : "---");
    _tft.drawString(buf, 6, 22, 2);

    char ago[12];
    fmtAgo(now - loraLink.lastRxMs(), ago, sizeof(ago));
    if (loraLink.online())       snprintf(buf, sizeof(buf), "%14s", "LINK OK");
    else if (loraLink.everSeen()) snprintf(buf, sizeof(buf), "LOST %8s", ago);
    else                          snprintf(buf, sizeof(buf), "%14s", "NO COMPANION");
    _tft.setTextDatum(TR_DATUM);
    _tft.setTextColor(loraLink.online() ? C_OK : C_DANGER, C_BG);
    _tft.drawString(buf, SCREEN_W - 6, 26, 1);
    _tft.setTextDatum(TL_DATUM);

    if (!loraLink.everSeen()) {
        if (full) {
            _tft.setTextDatum(MC_DATUM);
            _tft.setTextColor(C_MUTED, C_BG);
            _tft.drawString("Waiting for the LoRa32", 120, 130, 2);
            _tft.drawString("companion over ESP-NOW...", 120, 150, 2);
            _tft.setTextDatum(TL_DATUM);
        }
        return;
    }

    // Band status banner
    bool recent = false;
    for (int i = 0; i < r.hitCount; i++) if (r.hits[i].ageS < 60) recent = true;
    uint16_t bannerBg = !r.radioOk ? C_DIM : r.jamming ? C_DANGER : recent ? 0x8200 : 0x0320;
    const char* banner = !r.radioOk ? "COMPANION RADIO FAULT"
                       : r.jamming  ? "JAMMING DETECTED"
                       : recent     ? "LORA ACTIVITY NEARBY" : "BAND QUIET";
    _tft.fillRoundRect(6, 40, 228, 18, 4, bannerBg);
    _tft.setTextDatum(MC_DATUM);
    _tft.setTextColor(C_FG, bannerBg);
    _tft.drawString(banner, 120, 49, 2);
    _tft.setTextDatum(TL_DATUM);

    snprintf(buf, sizeof(buf), "floor %d dBm   baseline %d dBm", r.noiseFloor, r.baseline);
    padLine(_tft, buf, 62, C_MUTED);
    snprintf(buf, sizeof(buf), "Mesh %u nodes / %u pkts   LoRa %u",
             r.meshNodes, r.meshPackets, r.loraPackets);
    padLine(_tft, buf, 73, r.meshNodes ? C_FG : C_MUTED);

    // Spectrum: 64 bins, 3 px apart, drawn bar by bar so nothing is blanked first
    const int gx = 24, gy = 85, gh = 66;
    for (int i = 0; i < LORA_SPECTRUM_BINS; i++) {
        int v = constrain(r.spectrum[i], -125, -45);
        int h = 1 + (v + 125) * (gh - 1) / 80;
        int x = gx + i * 3;
        _tft.fillRect(x, gy, 2, gh - h, C_BG);
        uint16_t c = heatColor(r.spectrum[i] - r.noiseFloor);
        _tft.fillRect(x, gy + gh - h, 2, h, c == C_BG ? C_DIM : c);
    }
    if (full) {
        _tft.drawFastHLine(gx, gy + gh, LORA_SPECTRUM_BINS * 3, C_DIM);
        uint32_t step = r.stepKhz ? r.stepKhz : 1;
        uint32_t mk = meshKhz[r.band];
        if (mk >= r.startKhz) {
            int mb = (mk - r.startKhz + step / 2) / step;
            if (mb < LORA_SPECTRUM_BINS) {
                int mx = gx + mb * 3;
                _tft.fillTriangle(mx, gy + gh + 2, mx - 3, gy + gh + 7, mx + 3, gy + gh + 7, C_ACCENT);
            }
        }
        _tft.setTextColor(C_MUTED, C_BG);
        snprintf(buf, sizeof(buf), "%.0f", r.startKhz / 1000.0f);
        _tft.drawString(buf, gx, gy + gh + 4, 1);
        snprintf(buf, sizeof(buf), "%.0f MHz", (r.startKhz + 63 * step) / 1000.0f);
        _tft.setTextDatum(TR_DATUM);
        _tft.drawString(buf, gx + LORA_SPECTRUM_BINS * 3, gy + gh + 4, 1);
        _tft.setTextDatum(TL_DATUM);
        _tft.setTextColor(C_ACCENT, C_BG);
        _tft.drawString("mesh", 104, gy + gh + 9, 1);
    }

    // Waterfall: one row per report, newest on top; only when a new report landed
    const int wy = 172;
    if (loraLink.accepted() != _loraDrawnAcc) {
        _loraDrawnAcc = loraLink.accepted();
        for (int a = 0; a < LORA_WATER_ROWS; a++) {
            const int8_t* row = a < loraLink.rows() ? loraLink.row(a) : nullptr;
            for (int i = 0; i < LORA_SPECTRUM_BINS; i++) {
                uint16_t c = row ? heatColor(row[i] - r.noiseFloor) : C_BG;
                _tft.drawFastHLine(gx + i * 3, wy + a, 3, c);
            }
        }
    }

    // Recent hits
    for (int i = 0; i < 5; i++) {
        int y = 216 + i * 12;
        if (i >= r.hitCount) {
            padLine(_tft, i == 0 ? "No LoRa transmissions heard yet" : "", y, C_DIM);
            continue;
        }
        const LoraHit& h = r.hits[i];
        fmtAgo(h.ageS * 1000UL, ago, sizeof(ago));
        if (h.kind == HIT_MESHTASTIC)
            snprintf(buf, sizeof(buf), "MESH !%08lx %4ddBm snr%3d %s",
                     (unsigned long)h.nodeId, h.rssi, h.snr, ago);
        else
            snprintf(buf, sizeof(buf), "LoRa %.3f SF%u %4ddBm %s",
                     h.freqKhz / 1000.0f, h.sf, h.rssi, ago);
        padLine(_tft, buf, y, h.ageS < 60 ? C_FG : C_MUTED);
    }
}

void Display::drawPopup(const Detection& d) {
    const Button& p = Layout::POPUP;
    uint16_t bg = (d.following || d.conf == Confidence::CONF_HIGH) ? 0x9000 : 0xA200;
    mechStrike(bg);
    _tft.fillRoundRect(p.x, p.y, p.w, p.h, 8, bg);
    _tft.drawRoundRect(p.x, p.y, p.w, p.h, 8, C_FG);
    _tft.drawRoundRect(p.x + 1, p.y + 1, p.w - 2, p.h - 2, 7, C_FG);

    char buf[48];
    _tft.setTextDatum(MC_DATUM);
    _tft.setTextColor(C_FG, bg);
    _tft.drawString(d.following ? "FOLLOWING YOU" : "! ALERT !", 120, p.y + 20, 4);
    strlcpy(buf, detectionTypeName(d.type), sizeof(buf));
    fitText(_tft, buf, 200, 4);
    _tft.drawString(buf, 120, p.y + 52, 4);
    strlcpy(buf, d.vendor ? d.vendor : d.name, sizeof(buf));
    fitText(_tft, buf, 200, 2);
    _tft.drawString(buf, 120, p.y + 82, 2);
    snprintf(buf, sizeof(buf), "%d dBm  %s  %s", d.rssi, sourceName(d.src), confidenceName(d.conf));
    _tft.drawString(buf, 120, p.y + 102, 2);
    _tft.drawString("tap to view  -  tap outside to close", 120, p.y + 132, 1);
    _tft.setTextDatum(TL_DATUM);
}

void Display::showCalTarget(int16_t x, int16_t y, int step, int total) {
    _tft.fillScreen(C_BG);
    _tft.drawLine(x - 15, y, x + 15, y, C_DANGER);
    _tft.drawLine(x, y - 15, x, y + 15, C_DANGER);
    _tft.drawCircle(x, y, 6, C_DANGER);
    char buf[24];
    snprintf(buf, sizeof(buf), "Point %d of %d", step, total);
    _tft.setTextDatum(MC_DATUM);
    _tft.setTextColor(C_FG, C_BG);
    _tft.drawString("TOUCH THE +", 120, 210, 4);
    _tft.drawString(buf, 120, 236, 2);
    _tft.setTextColor(C_MUTED, C_BG);
    _tft.drawString("Use a stylus or fingernail", 120, 254, 2);
    _tft.setTextDatum(TL_DATUM);
    _screen = Screen::CALIBRATE;
}

void Display::showMessage(const char* line1, const char* line2, uint16_t color) {
    _tft.fillScreen(C_BG);
    _tft.setTextDatum(MC_DATUM);
    _tft.setTextColor(color, C_BG);
    _tft.drawString(line1, 120, 150, 4);
    if (line2) {
        _tft.setTextColor(C_FG, C_BG);
        _tft.drawString(line2, 120, 180, 2);
    }
    _tft.setTextDatum(TL_DATUM);
}
