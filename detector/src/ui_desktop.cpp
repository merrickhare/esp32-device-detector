// Boot splash, desktop screen (clock, weather, HUD animation) and setup-portal screen.
#include "ui.h"
#include "netsync.h"
#include "mech_sprites.h"

// Optional custom images (git-ignored). Each header defines a baseline JPEG array:
//   include/local_boot_img.h   -> boot_jpg   (240x320, boot splash)
//   include/local_header_img.h -> header_jpg (240x150, desktop header)
// Without them the built-in drawn art is used.
#if __has_include("local_boot_img.h")
#include "local_boot_img.h"
#define HAVE_BOOT_IMG 1
#endif
#if __has_include("local_header_img.h")
#include "local_header_img.h"
#define HAVE_HEADER_IMG 1
#endif
#if defined(HAVE_BOOT_IMG) || defined(HAVE_HEADER_IMG)
#include <TJpg_Decoder.h>
static TFT_eSPI* s_jpgTft = nullptr;
static bool jpgOut(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
    if (y >= s_jpgTft->height()) return false;
    s_jpgTft->pushImage(x, y, w, h, bmp);
    return true;
}
static void drawJpg(TFT_eSPI& g, const uint8_t* d, size_t n) {
    s_jpgTft = &g;
    TJpgDec.setJpgScale(1);
    TJpgDec.setSwapBytes(true);
    TJpgDec.setCallback(jpgOut);
    TJpgDec.drawJpg(0, 0, d, n);
}
#endif
#include <time.h>


// Arc segment that may wrap past 360 degrees
static void arcSeg(TFT_eSPI& g, int cx, int cy, int r, int ir, int start, int len, uint16_t col) {
    start %= 360;
    int end = start + len;
    if (end <= 360) {
        g.drawArc(cx, cy, r, ir, start, end, col, C_BG, false);
    } else {
        g.drawArc(cx, cy, r, ir, start, 360, col, C_BG, false);
        g.drawArc(cx, cy, r, ir, 0, end - 360, col, C_BG, false);
    }
}

// --- Mech sprites (palette-indexed, run-length encoded; see tools/mech_sprites.py) ---

static void blitMech(TFT_eSprite& s, int frame, int x0, int y0) {
    const uint8_t* d = mech_frames[frame];
    const int total = MECH_W * MECH_H;
    int px = 0;
    while (px < total) {
        uint8_t n = pgm_read_byte(d++);
        uint8_t c = pgm_read_byte(d++);
        if (c) {
            uint16_t col = mech_palette[c];
            for (int k = 0; k < n; k++) {
                int p = px + k;
                s.drawPixel(x0 + p % MECH_W, y0 + p / MECH_W, col);
            }
        }
        px += n;
    }
}

// Original header art (no external images): night sky, starfield, hangar grid and the
// mech drawn at 2x with wings deployed.
static void drawHeaderArt(TFT_eSPI& g) {
    for (int y = 0; y < 150; y++) {
        uint8_t b = 4 + y / 10;                           // navy fading up from black
        g.drawFastHLine(0, y, 240, g.color565(0, b, b * 3));
    }
    uint32_t r = 0x2E0B;                                  // fixed seed: same stars every time
    for (int i = 0; i < 70; i++) {
        r = r * 1103515245 + 12345;
        int x = (r >> 8) % 240, y = (r >> 16) % 100;
        g.drawPixel(x, y, (i % 7) ? 0x8410 : 0xFFFF);
    }
    const int hz = 112;                                   // horizon
    for (int x = -240; x <= 480; x += 30) g.drawLine(120, hz, x, 150, C_ZCYDIM);
    for (int k = 0; k < 5; k++) { int y = hz + (k * k * 38) / 16; if (y < 150) g.drawFastHLine(0, y, 240, C_ZCYDIM); }
    const uint8_t* d = mech_frames[MECH_WINGS_START + MECH_WINGS_COUNT - 1];
    const int x0 = 78, y0 = 12, total = MECH_W * MECH_H;
    int px = 0;
    while (px < total) {
        uint8_t n = pgm_read_byte(d++);
        uint8_t c = pgm_read_byte(d++);
        if (c) for (int k = 0; k < n; k++) {
            int p = px + k;
            g.fillRect(x0 + (p % MECH_W) * 2, y0 + (p / MECH_W) * 2, 2, 2, mech_palette[c]);
        }
        px += n;
    }
}

// Muzzle sits at column 79, row 19 of a frame (bob 0)
static const int MECH_MUZZLE_X = 79, MECH_MUZZLE_Y = 19;

void Display::mechBootScene() {
    TFT_eSprite s(&_tft);
    s.setColorDepth(16);
    s.setFreeFont(nullptr);
    if (!s.createSprite(MECH_W, MECH_H + 16)) return;
    _tft.fillScreen(C_BG);
    for (int i = 0; i < 70; i++) {
        uint32_t r = esp_random();
        _tft.drawPixel(r % SCREEN_W, (r >> 8) % SCREEN_H, (r >> 20) & 1 ? C_MUTED : C_DIM);
    }
    const int x = (SCREEN_W - MECH_W) / 2 + 8;
    // Boost up from below the screen, trail cleared by the sprite's empty bottom rows
    for (int y = SCREEN_H, i = 0; y > 96; y -= 12, i++) {
        s.fillSprite(C_BG);
        blitMech(s, MECH_BOOST_START + i % MECH_BOOST_COUNT, 0, 0);
        s.pushSprite(x, y);
        delay(30);
    }
    for (int i = 0; i < MECH_WINGS_COUNT; i++) {
        s.fillSprite(C_BG);
        blitMech(s, MECH_WINGS_START + i, 0, 0);
        s.pushSprite(x, 96);
        delay(110);
    }
    s.deleteSprite();
    _tft.setTextDatum(MC_DATUM);
    _tft.setTextColor(C_FG, C_BG);
    _tft.drawString(BRAND_NAME, 120, 200, 4);
    _tft.setTextColor(C_ZCYAN, C_BG);
    _tft.drawString("UNIT ONLINE", 120, 226, 2);
    _tft.setTextDatum(TL_DATUM);
    delay(700);
}

// Quick rifle shot inside the popup box before an alert's text is drawn
void Display::mechStrike(uint16_t bg) {
    const Button& p = Layout::POPUP;
    TFT_eSprite s(&_tft);
    s.setColorDepth(16);
    s.setFreeFont(nullptr);
    if (!s.createSprite(MECH_W, MECH_H)) return;
    _tft.fillRoundRect(p.x, p.y, p.w, p.h, 8, bg);
    const int mx = p.x + 6, my = p.y + (p.h - MECH_H) / 2;
    const int by = my + MECH_MUZZLE_Y;
    for (int f = 0; f < MECH_FIRE_COUNT; f++) {
        s.fillSprite(bg);
        blitMech(s, MECH_FIRE_START + f, 0, 0);
        s.pushSprite(mx, my);
        if (f == 1) {
            _tft.fillRect(mx + MECH_W, by - 1, p.x + p.w - 4 - (mx + MECH_W), 3, C_ZCYAN);
            _tft.drawFastHLine(mx + MECH_W, by, p.x + p.w - 4 - (mx + MECH_W), C_FG);
        }
        if (f == 3) _tft.fillRect(mx + MECH_W, by - 1, p.x + p.w - 4 - (mx + MECH_W), 3, bg);
        delay(f == 0 ? 160 : 90);
    }
    s.deleteSprite();
}

// --- Boot splash ---

// Original pilot-helmet splash art drawn with primitives (no external images).
static void drawPilotArt(TFT_eSPI& g) {
    for (int y = 0; y < 262; y++) {
        uint8_t b = 2 + y / 16;
        g.drawFastHLine(0, y, 240, (y % 4 == 0) ? g.color565(0, b * 2, b * 4) : g.color565(0, b, b * 3));
    }
    // shoulders / suit collar
    g.fillRoundRect(10, 214, 220, 70, 24, C_NAVY);
    g.fillRect(10, 236, 220, 3, C_ZGOLD);
    g.fillRect(96, 214, 48, 48, 0x216D);
    // helmet shell + shading
    g.fillEllipse(120, 134, 80, 92, 0xEF7E);
    g.fillEllipse(140, 140, 56, 82, 0xD6BA);
    g.fillEllipse(116, 128, 70, 84, 0xEF7E);
    // red crest down the centre
    g.fillTriangle(110, 50, 130, 50, 120, 26, C_ZRED);
    g.fillRect(110, 50, 20, 52, C_ZRED);
    g.fillRect(118, 30, 4, 72, 0xF800);
    // side ear pods
    g.fillRoundRect(30, 120, 18, 54, 6, C_NAVY);
    g.fillRoundRect(192, 120, 18, 54, 6, C_NAVY);
    g.fillRect(34, 140, 10, 4, C_ZGOLD);
    g.fillRect(196, 140, 10, 4, C_ZGOLD);
    // visor
    g.fillRoundRect(56, 108, 128, 54, 22, 0x0863);
    g.drawRoundRect(56, 108, 128, 54, 22, C_ZCYAN);
    g.drawRoundRect(57, 109, 126, 52, 21, C_ZCYDIM);
    for (int i = 0; i < 3; i++) g.drawLine(74 + i * 6, 152, 98 + i * 6, 116, C_ZCYDIM);
    g.drawFastHLine(70, 135, 100, C_ZCYDIM);
    // chin guard
    g.fillTriangle(84, 176, 156, 176, 120, 214, C_NAVY);
    g.fillRect(112, 188, 16, 3, C_ZRED);
}

void Display::showBoot() {
    _screen = Screen::BOOT;
    _tft.fillScreen(C_BG);
#ifdef HAVE_BOOT_IMG
    drawJpg(_tft, boot_jpg, sizeof(boot_jpg));
#else
    drawPilotArt(_tft);
#endif

    // Targeting reticle locks on over the visor HUD
#ifdef BOOT_RETICLE_X
    const int rx = BOOT_RETICLE_X, ry = BOOT_RETICLE_Y;   // set in local_config.h for a custom image
#else
    const int rx = 120, ry = 135;
#endif
    for (int a = 30; a <= 360; a += 30) {
        _tft.drawArc(rx, ry, 17, 15, 0, a, C_ZCYAN, C_BG, false);
        delay(18);
    }
    for (int q = 0; q < 4; q++) {
        _tft.drawArc(rx, ry, 25, 23, q * 90 + 15, q * 90 + 75, C_ZCYAN, C_BG, false);
        delay(40);
    }
    _tft.drawFastHLine(rx - 32, ry, 10, C_ZCYAN);
    _tft.drawFastHLine(rx + 23, ry, 10, C_ZCYAN);
    _tft.drawFastVLine(rx, ry - 32, 10, C_ZCYAN);
    _tft.drawFastVLine(rx, ry + 23, 10, C_ZCYAN);
    for (int i = 0; i < 3; i++) {
        _tft.fillCircle(rx, ry, 3, i % 2 ? C_ZCYAN : C_ZRED);
        delay(90);
    }

    // Name plate over the chest
    _tft.fillRect(0, 262, SCREEN_W, 58, C_BG);
    _tft.fillRect(0, 262, SCREEN_W, 2, C_ZCYAN);
    _tft.fillRect(0, 264, 70, 2, C_ZRED);
    _tft.fillRect(70, 264, 24, 2, C_ZGOLD);

    const char* name = BRAND_NAME;
    char part[12];
    _tft.setTextDatum(TL_DATUM);
    for (size_t i = 1; i <= strlen(name); i++) {
        strlcpy(part, name, i + 1);
        _tft.setTextColor(C_FG, C_BG);
        _tft.drawString(part, 10, 270, 4);
        delay(70);
    }
    _tft.setTextColor(C_ZGOLD, C_BG);
    _tft.drawString("v3.0", 200, 276, 2);

    const char* tag = BRAND_BOOT_TAG;
    char line[48];
    for (size_t i = 1; i <= strlen(tag); i += 2) {
        strlcpy(line, tag, i + 1);
        _tft.setTextColor(C_ZCYAN, C_BG);
        _tft.drawString(line, 10, 295, 1);
        delay(14);
    }
    _tft.drawRect(10, 305, 220, 5, C_ZCYDIM);
    for (int x = 0; x <= 216; x += 8) {
        _tft.fillRect(12, 306, x, 3, x > 180 ? C_ZRED : C_ZCYAN);
        delay(16);
    }
    _tft.setTextColor(C_DIM, C_BG);
    _tft.drawString("Hold screen to recalibrate", 10, 312, 1);
    delay(500);
    mechBootScene();
    _tft.setTextColor(C_DIM, C_BG);
    _tft.setTextDatum(MC_DATUM);
    _tft.drawString("Hold screen to recalibrate", 120, 310, 1);
    _tft.setTextDatum(TL_DATUM);
}

// --- Desktop ---

void Display::freeDesktop() {
    if (_radar) { _radar->deleteSprite(); delete _radar; _radar = nullptr; }
    if (_wxIcon) { _wxIcon->deleteSprite(); delete _wxIcon; _wxIcon = nullptr; }
}

static TFT_eSprite* makeSprite(TFT_eSPI* tft, int w, int h) {
    TFT_eSprite* s = new TFT_eSprite(tft);
    s->setColorDepth(16);
    s->setFreeFont(nullptr);   // sprite font state is not initialized by the constructor
    s->setTextFont(1);
    s->setTextSize(1);
    s->setTextDatum(TL_DATUM);
    if (!s->createSprite(w, h)) {
        delete s;
        return nullptr;
    }
    return s;
}

void Display::drawDesktop(bool full) {
    uint32_t t = millis();
    if (full) {
        _tft.fillScreen(C_BG);
#ifdef HAVE_HEADER_IMG
        drawJpg(_tft, header_jpg, sizeof(header_jpg));
#else
        drawHeaderArt(_tft);
#endif

        // HUD corner brackets
        const int L = 14;
        _tft.drawFastHLine(3, 3, L, C_ZCYAN);   _tft.drawFastVLine(3, 3, L, C_ZCYAN);
        _tft.drawFastHLine(236 - L, 3, L, C_ZCYAN); _tft.drawFastVLine(236, 3, L, C_ZCYAN);
        _tft.drawFastHLine(3, 146, L, C_ZCYAN); _tft.drawFastVLine(3, 146 - L, L, C_ZCYAN);
        _tft.drawFastHLine(236 - L, 146, L, C_ZCYAN); _tft.drawFastVLine(236, 146 - L, L, C_ZCYAN);

        // Name with a drop shadow so it reads over the art
        _tft.setTextDatum(TL_DATUM);
        _tft.setTextColor(C_BG);
        _tft.drawString(BRAND_LABEL, 12, 106, 1);
        _tft.drawString(BRAND_NAME, 12, 118, 4);
        _tft.setTextColor(C_ZCYAN);
        _tft.drawString(BRAND_LABEL, 11, 105, 1);
        _tft.setTextColor(C_FG);
        _tft.drawString(BRAND_NAME, 10, 116, 4);

        // Suit trim stripe
        _tft.fillRect(0, 150, SCREEN_W, 3, C_NAVY);
        _tft.fillRect(0, 150, 96, 3, C_ZRED);
        _tft.fillRect(96, 150, 28, 3, C_ZGOLD);

        if (!_radar) _radar = makeSprite(&_tft, 100, 100);
        if (!_wxIcon) _wxIcon = makeSprite(&_tft, 52, 46);
        _clockSec = -1;
        _wxStamp = 1;
        _netShown = 255;
        _beamOn = false;
        mechStartIdle(t);
        _mechNextAction = t + 6000;   // first trick shortly after opening
    }

    // Clock
    time_t now = time(nullptr);
    bool valid = netSync.timeValid();
    struct tm tmv;
    if (valid) localtime_r(&now, &tmv);
    int sec = valid ? tmv.tm_sec : (int)((t / 1000) % 60);
    if (sec != _clockSec) {
        bool firstOrResync = _clockSec < 0;
        _clockSec = sec;
        _secBase = t;
        char buf[24];
        _tft.setTextDatum(TR_DATUM);
        _tft.setTextColor(C_FG, C_BG);
        _tft.setTextPadding(_tft.textWidth("88:88", 7));
        if (valid) {
            int h = tmv.tm_hour % 12;
            snprintf(buf, sizeof(buf), "%d:%02d", h ? h : 12, tmv.tm_min);
        } else {
            strcpy(buf, "--:--");
        }
        _tft.drawString(buf, 150, 158, 7);

        _tft.setTextDatum(TL_DATUM);
        _tft.setTextColor(C_ZCYAN, C_BG);
        _tft.setTextPadding(_tft.textWidth("88", 4));
        snprintf(buf, sizeof(buf), "%02d", valid ? tmv.tm_sec : 0);
        _tft.drawString(valid ? buf : "--", 156, 160, 4);

        _tft.setTextColor(C_ZGOLD, C_BG);
        _tft.setTextPadding(_tft.textWidth("PM", 2));
        _tft.drawString(valid ? (tmv.tm_hour < 12 ? "AM" : "PM") : "", 196, 165, 2);

        if (valid || firstOrResync) {
            _tft.setTextColor(C_MUTED, C_BG);
            _tft.setTextPadding(84);
            if (valid) strftime(buf, sizeof(buf), "%a %b %d", &tmv);
            else strcpy(buf, "NO TIME");
            for (char* p = buf; *p; p++) *p = toupper(*p);
            _tft.drawString(buf, 156, 188, 2);
        }
        _tft.setTextPadding(0);
    }

    // Seconds sweep bar
    uint32_t into = t - _secBase;
    if (into > 1000) into = 1000;
    int w = (int)(((sec * 1000UL + into) * SCREEN_W) / 60000UL);
    _tft.fillRect(0, 210, w, 3, C_ZCYAN);
    _tft.fillRect(w, 210, SCREEN_W - w, 3, C_NAVY);

    drawMechPanel(t);

    Weather wx;
    netSync.weather(wx);
    drawWxIcon(t, wx.valid ? wx.code : -1, wx.valid ? wx.day : true);

    // Beam shot reaches across the weather panel (row 261 sits between its text lines)
    uint8_t mf = _mechLen ? _mechSeq[_mechPos].frame : 0;
    bool beam = mf == MECH_FIRE_START + 1 || mf == MECH_FIRE_START + 2;
    const int beamY = 216 + 26 + MECH_MUZZLE_Y;
    if (beam) {
        _tft.fillRect(104, beamY - 1, SCREEN_W - 104, 3, C_ZCYAN);
        _tft.drawFastHLine(104, beamY, SCREEN_W - 104, C_FG);
        _beamOn = true;
    } else if (_beamOn) {
        _tft.fillRect(104, beamY - 1, SCREEN_W - 104, 3, C_BG);
        _beamOn = false;
    }
    if (wx.stamp != _wxStamp || (uint8_t)netSync.state() != _netShown) {
        _wxStamp = wx.stamp;
        _netShown = (uint8_t)netSync.state();
        drawWxText(full);
    }
}

void Display::drawRadar(uint32_t t) {
    if (!_radar) return;
    TFT_eSprite& s = *_radar;
    const int c = 50;
    s.fillSprite(C_BG);
    s.drawCircle(c, c, 48, C_ZCYDIM);
    s.drawCircle(c, c, 33, C_ZCYDIM);
    s.drawCircle(c, c, 18, C_NAVY);
    s.drawFastHLine(4, c, 92, C_NAVY);
    s.drawFastVLine(c, 4, 92, C_NAVY);
    for (int i = 0; i < 12; i++) {   // bezel ticks
        float a = i * PI / 6;
        s.drawLine(c + cosf(a) * 44, c + sinf(a) * 44, c + cosf(a) * 48, c + sinf(a) * 48, C_ZCYDIM);
    }

    // Sweep beam with fading trail
    float a0 = (t % 2800) / 2800.0f * 2 * PI;
    for (int i = 13; i >= 0; i--) {
        float a = a0 - i * 0.045f;
        uint8_t g = 255 - i * 18;
        s.drawLine(c, c, c + cosf(a) * 46, c + sinf(a) * 46, s.color565(0, g, g));
    }

    // Rotating outer segments, counter-rotating inner ones (visor HUD feel)
    int d1 = (t / 10) % 360, d2 = 360 - (t / 16) % 360;
    for (int q = 0; q < 3; q++) arcSeg(s, c, c, 47, 45, d1 + q * 120, 40, C_ZCYAN);
    for (int q = 0; q < 4; q++) arcSeg(s, c, c, 26, 24, d2 + q * 90, 30, C_ZGOLD);

    float pulse = (sinf(t / 180.0f) + 1) * 0.5f;
    s.fillCircle(c, c, 2 + (int)(pulse * 3), C_ZRED);
    s.pushSprite(4, 216);
}

// --- Mech panel: idles, and every ~20 s walks, boosts, deploys wings or fires ---

void Display::mechStartIdle(uint32_t t) {
    _mechLen = 0;
    for (int i = 0; i < MECH_IDLE_COUNT; i++) _mechSeq[_mechLen++] = {(uint8_t)(MECH_IDLE_START + i), 220};
    _mechPos = 0;
    _mechStepAt = t;
}

void Display::mechStartAction(uint32_t t) {
    _mechLen = 0;
    auto add = [&](int frame, uint16_t ms) { if (_mechLen < 40) _mechSeq[_mechLen++] = {(uint8_t)frame, ms}; };
    switch (esp_random() % 4) {
        case 0:   // walk
            for (int r = 0; r < 2; r++) for (int i = 0; i < MECH_WALK_COUNT; i++) add(MECH_WALK_START + i, 120);
            break;
        case 1:   // boost
            for (int i = 0; i < MECH_WINGS_COUNT; i++) add(MECH_WINGS_START + i, 80);
            for (int r = 0; r < 4; r++) for (int i = 0; i < MECH_BOOST_COUNT; i++) add(MECH_BOOST_START + i, 70);
            for (int i = MECH_WINGS_COUNT - 1; i >= 0; i--) add(MECH_WINGS_START + i, 80);
            break;
        case 2:   // fire
            add(MECH_FIRE_START, 350);
            for (int r = 0; r < 2; r++) {
                add(MECH_FIRE_START + 1, 80);
                add(MECH_FIRE_START + 2, 80);
                add(MECH_FIRE_START + 3, 260);
            }
            add(MECH_FIRE_START, 250);
            break;
        default:  // wings
            for (int i = 0; i < MECH_WINGS_COUNT; i++) add(MECH_WINGS_START + i, 140);
            add(MECH_WINGS_START + MECH_WINGS_COUNT - 1, 1000);
            for (int i = MECH_WINGS_COUNT - 1; i >= 0; i--) add(MECH_WINGS_START + i, 140);
            break;
    }
    _mechPos = 0;
    _mechStepAt = t;
    _mechNextAction = 0;   // set when the action finishes
}

void Display::drawMechPanel(uint32_t t) {
    if (!_radar || !_mechLen) return;
    if (t - _mechStepAt >= _mechSeq[_mechPos].ms) {
        _mechStepAt = t;
        if (++_mechPos >= _mechLen) {
            bool wasIdle = _mechSeq[0].frame == MECH_IDLE_START;
            if (wasIdle && _mechNextAction && (int32_t)(t - _mechNextAction) >= 0) {
                mechStartAction(t);
            } else {
                if (!wasIdle) _mechNextAction = t + 15000 + esp_random() % 10000;
                mechStartIdle(t);
            }
        }
    }

    TFT_eSprite& s = *_radar;
    s.fillSprite(C_BG);
    // Hangar floor and HUD brackets
    s.drawFastHLine(0, 91, 100, C_ZCYDIM);
    for (int x = (t / 60) % 12; x < 100; x += 12) s.drawPixel(x, 93, C_ZCYDIM);
    s.drawFastHLine(0, 0, 8, C_ZCYDIM);  s.drawFastVLine(0, 0, 8, C_ZCYDIM);
    s.drawFastHLine(92, 0, 8, C_ZCYDIM); s.drawFastVLine(99, 0, 8, C_ZCYDIM);
    s.setTextColor(C_ZCYDIM, C_BG);
    s.drawString(BRAND_UNIT, 4, 4, 1);

    uint8_t frame = _mechSeq[_mechPos].frame;
    blitMech(s, frame, 6, 26);
    if (frame == MECH_FIRE_START + 1 || frame == MECH_FIRE_START + 2) {
        s.fillRect(6 + MECH_MUZZLE_X + 1, 26 + MECH_MUZZLE_Y - 1, 100, 3, C_ZCYAN);
        s.drawFastHLine(6 + MECH_MUZZLE_X + 1, 26 + MECH_MUZZLE_Y, 100, C_FG);
    }
    s.pushSprite(4, 216);
}

enum WxKind { WX_SUN, WX_MOON, WX_PARTLY, WX_CLOUD, WX_FOG, WX_RAIN, WX_SNOW, WX_STORM, WX_NONE };

static WxKind wxKind(int code, bool day) {
    if (code < 0) return WX_NONE;
    if (code == 0) return day ? WX_SUN : WX_MOON;
    if (code <= 2) return WX_PARTLY;
    if (code == 3) return WX_CLOUD;
    if (code == 45 || code == 48) return WX_FOG;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return WX_SNOW;
    if (code >= 95) return WX_STORM;
    if (code >= 51) return WX_RAIN;
    return WX_CLOUD;
}

static const char* wxName(int code) {
    if (code < 0) return "NO DATA";
    if (code == 0) return "CLEAR";
    if (code == 1) return "MOSTLY CLEAR";
    if (code == 2) return "PARTLY CLOUDY";
    if (code == 3) return "OVERCAST";
    if (code == 45 || code == 48) return "FOG";
    if (code >= 51 && code <= 57) return "DRIZZLE";
    if (code >= 61 && code <= 67) return "RAIN";
    if (code >= 71 && code <= 77) return "SNOW";
    if (code >= 80 && code <= 82) return "SHOWERS";
    if (code == 85 || code == 86) return "SNOW SHOWERS";
    if (code >= 95) return "THUNDERSTORM";
    return "UNKNOWN";
}

static void drawCloud(TFT_eSprite& s, int cx, int cy, uint16_t col) {
    s.fillCircle(cx - 9, cy + 2, 7, col);
    s.fillCircle(cx + 1, cy - 4, 10, col);
    s.fillCircle(cx + 11, cy + 2, 7, col);
    s.fillRoundRect(cx - 16, cy + 1, 34, 9, 4, col);
}

static void drawSun(TFT_eSprite& s, int cx, int cy, int r, uint32_t t) {
    float spin = t / 900.0f;
    for (int i = 0; i < 8; i++) {
        float a = spin + i * PI / 4;
        s.drawLine(cx + cosf(a) * (r + 3), cy + sinf(a) * (r + 3),
                   cx + cosf(a) * (r + 8), cy + sinf(a) * (r + 8), C_ZGOLD);
    }
    s.fillCircle(cx, cy, r, C_ZGOLD);
}

void Display::drawWxIcon(uint32_t t, int code, bool day) {
    if (!_wxIcon) return;
    TFT_eSprite& s = *_wxIcon;
    s.fillSprite(C_BG);
    int drift = (int)(sinf(t / 700.0f) * 2);
    switch (wxKind(code, day)) {
        case WX_SUN:
            drawSun(s, 26, 23, 10, t);
            break;
        case WX_MOON: {
            s.fillCircle(24, 22, 12, 0xF79E);
            s.fillCircle(30, 17, 11, C_BG);
            const int sx[3] = {40, 10, 44}, sy[3] = {8, 36, 32};
            for (int i = 0; i < 3; i++) {
                uint8_t b = 120 + (uint8_t)(sinf(t / 300.0f + i * 2) * 120 + 15);
                s.drawPixel(sx[i], sy[i], s.color565(b, b, b));
            }
            break;
        }
        case WX_PARTLY:
            if (day) drawSun(s, 17, 15, 7, t);
            else { s.fillCircle(17, 14, 8, 0xF79E); s.fillCircle(21, 11, 7, C_BG); }
            drawCloud(s, 29 + drift, 27, C_FG);
            break;
        case WX_CLOUD:
            drawCloud(s, 18 + drift, 16, C_MUTED);
            drawCloud(s, 28 - drift, 26, C_FG);
            break;
        case WX_FOG:
            drawCloud(s, 26, 15, C_MUTED);
            for (int i = 0; i < 3; i++) {
                int off = (int)((t / 40 + i * 9) % 12);
                s.drawFastHLine(6 + off, 30 + i * 5, 30, C_FG);
            }
            break;
        case WX_RAIN:
        case WX_STORM: {
            bool storm = wxKind(code, day) == WX_STORM;
            for (int i = 0; i < 5; i++) {
                int x = 13 + i * 7;
                int y = 28 + (int)((t / 28 + i * 5) % 15);
                s.drawLine(x, y, x - 2, y + 4, C_ZCYAN);
            }
            drawCloud(s, 26 + drift, 15, storm ? C_DIM : C_MUTED);
            if (storm && (t % 1800) < 160) {
                s.fillTriangle(26, 20, 19, 33, 25, 33, C_ZGOLD);
                s.fillTriangle(25, 31, 31, 31, 22, 45, C_ZGOLD);
            }
            break;
        }
        case WX_SNOW:
            for (int i = 0; i < 6; i++) {
                int y = 28 + (int)((t / 70 + i * 4) % 17);
                int x = 10 + i * 6 + (int)(sinf(t / 400.0f + i) * 2);
                s.fillCircle(x, y, 1, C_FG);
            }
            drawCloud(s, 26 + drift, 15, C_FG);
            break;
        default: {   // no data yet: slow spinner
            arcSeg(s, 26, 23, 14, 12, (t / 6) % 360, 90, C_ZCYDIM);
            arcSeg(s, 26, 23, 14, 12, (t / 6 + 180) % 360, 90, C_ZCYDIM);
            break;
        }
    }
    s.pushSprite(110, 218);
}

void Display::drawWxText(bool full) {
    Weather wx;
    bool have = netSync.weather(wx);
    char buf[40];

    // Temperature block (right of the icon)
    _tft.fillRect(164, 218, 76, 46, C_BG);
    _tft.setTextDatum(TL_DATUM);
    _tft.setTextColor(C_FG, C_BG);
    if (have) snprintf(buf, sizeof(buf), "%d", (int)lroundf(wx.temp));
    else strcpy(buf, "--");
    _tft.drawString(buf, 166, 220, 4);
    int tx = 166 + _tft.textWidth(buf, 4);
    _tft.drawCircle(tx + 4, 224, 2, C_FG);
    _tft.drawString(netSync.fahrenheit() ? "F" : "C", tx + 9, 222, 2);
    if (have) {
        _tft.setTextColor(C_MUTED, C_BG);
        snprintf(buf, sizeof(buf), "%d%% %d%s", wx.humidity, (int)lroundf(wx.wind),
                 netSync.fahrenheit() ? "mph" : "kmh");
        _tft.drawString(buf, 166, 248, 1);
    }

    // Condition / hi-lo / place-or-status lines
    _tft.fillRect(110, 266, 130, 54, C_BG);
    _tft.setTextColor(C_ZCYAN, C_BG);
    _tft.drawString(have ? wxName(wx.code) : "WEATHER", 112, 268, 2);
    if (have) {
        snprintf(buf, sizeof(buf), "H%d  L%d", (int)lroundf(wx.hi), (int)lroundf(wx.lo));
        _tft.setTextColor(C_ZGOLD, C_BG);
        _tft.drawString(buf, 112, 286, 2);
    }

    const char* status;
    uint16_t col = C_MUTED;
    switch (netSync.state()) {
        case NetState::NO_SETUP:   status = "SETTINGS>WIFI SETUP"; col = C_ZGOLD; break;
        case NetState::CONNECTING: status = "LINKING..."; break;
        case NetState::SYNCING:    status = "SYNCING..."; break;
        case NetState::FAILED:     status = netSync.failReason(); col = C_DANGER; break;
        default:                   status = netSync.place(); break;
    }
    strlcpy(buf, status && *status ? status : "LOCAL", 22);
    for (char* p = buf; *p; p++) *p = toupper(*p);
    _tft.setTextColor(col, C_BG);
    _tft.drawString(buf, 112, 306, 1);
}

// --- Setup portal screen ---

void Display::showPortal(const char* apPass) {
    _screen = Screen::PORTAL;
    _tft.fillScreen(C_BG);
    _tft.fillRect(0, 0, SCREEN_W, 3, C_ZRED);
    _tft.setTextDatum(MC_DATUM);
    _tft.setTextColor(C_ZCYAN, C_BG);
    _tft.drawString("WIFI SETUP", 120, 26, 4);
    _tft.setTextColor(C_FG, C_BG);
    _tft.drawString("1. On your phone, join:", 120, 64, 2);
    _tft.setTextColor(C_ZGOLD, C_BG);
    _tft.drawString(BRAND_AP_SSID, 120, 90, 4);
    _tft.setTextColor(C_FG, C_BG);
    _tft.drawString("Password:", 120, 122, 2);
    _tft.setTextColor(C_ZGOLD, C_BG);
    _tft.drawString(apPass, 120, 148, 4);
    _tft.setTextColor(C_FG, C_BG);
    _tft.drawString("2. Open http://192.168.4.1", 120, 186, 2);
    _tft.setTextColor(C_MUTED, C_BG);
    _tft.drawString("(the page may open by itself)", 120, 206, 2);
    _tft.drawString("Enter home WiFi + city, then Save.", 120, 236, 2);
    _tft.drawString("Scanning is paused during setup.", 120, 256, 2);
    _tft.setTextDatum(TL_DATUM);
    drawButton(Layout::PORTAL_CANCEL);
}
