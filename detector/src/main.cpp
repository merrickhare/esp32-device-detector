#include <Arduino.h>
#include <Preferences.h>
#include "config.h"
#include "detection.h"
#include "ui.h"
#include "touch.h"
#include "sdlog.h"
#include "pcap.h"
#include "loralink.h"
#include "netsync.h"

static Preferences prefs;

// Survives panic / watchdog resets (not power loss), so a crash can be traced
struct Breadcrumb {
    uint32_t magic;
    uint32_t uptimeS;
    uint8_t screen;
    int16_t tapX, tapY;
    uint32_t tapAtS;
};
RTC_NOINIT_ATTR static Breadcrumb crumb;
static const uint32_t CRUMB_MAGIC = 0xD37EC7ED;
static char resetInfo[48];

static const char* resetName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "power on / reset btn";
        case ESP_RST_EXT:       return "reset pin";
        case ESP_RST_SW:        return "software restart";
        case ESP_RST_PANIC:     return "CRASH (panic)";
        case ESP_RST_INT_WDT:   return "CRASH (irq watchdog)";
        case ESP_RST_TASK_WDT:  return "CRASH (task watchdog)";
        case ESP_RST_WDT:       return "CRASH (watchdog)";
        case ESP_RST_BROWNOUT:  return "BROWNOUT (low power)";
        case ESP_RST_DEEPSLEEP: return "deep sleep wake";
        default:                return "unknown";
    }
}

static bool abnormalReset(esp_reset_reason_t r) {
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
           r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
}

static void recordBoot() {
    esp_reset_reason_t r = esp_reset_reason();
    uint32_t boots = prefs.getUInt("boots", 0) + 1;
    prefs.putUInt("boots", boots);
    uint32_t prevUp = prefs.getUInt("uptime", 0);
    prefs.putUInt("uptime", 0);

    Serial.printf("[BOOT] #%lu reset reason: %s (prev run ~%lus)\n",
                  (unsigned long)boots, resetName(r), (unsigned long)prevUp);
    if (crumb.magic == CRUMB_MAGIC) {
        Serial.printf("[BOOT] before reset: up %lus, screen %u, last tap (%d,%d) at %lus\n",
                      (unsigned long)crumb.uptimeS, crumb.screen, crumb.tapX, crumb.tapY,
                      (unsigned long)crumb.tapAtS);
    }
    snprintf(resetInfo, sizeof(resetInfo), "Boot #%lu: %s", (unsigned long)boots, resetName(r));

    if (abnormalReset(r)) {
        prefs.putUInt("crashes", prefs.getUInt("crashes", 0) + 1);
        char line2[48];
        snprintf(line2, sizeof(line2), "after %lus  (screen %u)", (unsigned long)
                 (crumb.magic == CRUMB_MAGIC ? crumb.uptimeS : prevUp), crumb.screen);
        ui.showMessage(resetName(r), line2, C_DANGER);
        delay(4000);
    }
    memset(&crumb, 0, sizeof(crumb));
    crumb.magic = CRUMB_MAGIC;
}
static uint32_t lastRefreshMs = 0;
static uint32_t popupUntil = 0;
static uint8_t popupMac[6];
static Source popupSrc;

// --- Touch calibration ---

// Wait for a press, average stable raw samples, then wait for release
static void captureRaw(int16_t& rx, int16_t& ry) {
    for (;;) {
        while (touch.isTouched()) delay(20);
        delay(150);
        while (!touch.isTouched()) delay(20);
        delay(80);

        int32_t sx = 0, sy = 0;
        int n = 0;
        for (int i = 0; i < 12; i++) {
            int16_t x, y;
            if (touch.readRaw(x, y)) { sx += x; sy += y; n++; }
            delay(15);
        }
        if (n >= 6) {
            rx = sx / n;
            ry = sy / n;
            while (touch.isTouched()) delay(20);
            return;
        }
    }
}

static void runTouchCalibration() {
    const int16_t sx[3] = {30, 210, 120};
    const int16_t sy[3] = {40, 160, 290};
    int16_t rx[3], ry[3];

    for (;;) {
        for (int i = 0; i < 3; i++) {
            ui.showCalTarget(sx[i], sy[i], i + 1, 3);
            captureRaw(rx[i], ry[i]);
            Serial.printf("[CAL] point %d screen(%d,%d) raw(%d,%d)\n",
                          i + 1, sx[i], sy[i], rx[i], ry[i]);
        }
        if (touch.computeCal(sx, sy, rx, ry)) break;
        ui.showMessage("RETRY", "Points too close - try again", C_DANGER);
        delay(1500);
    }
    touch.saveCal();
    ui.showMessage("CALIBRATED", "Saved to flash", C_OK);
    delay(1000);
}

// --- Helpers ---

static void openList(ListMode mode) {
    ui.listMode = mode;
    ui.listScroll = 0;
    ui.go(Screen::LIST);
}

static void openDetail(int idx) {
    if (idx < 0) return;
    ui.select(idx);
    ui.go(Screen::DETAIL);
}

static void saveSettings() {
    prefs.putUChar("bright", ui.brightness());
    prefs.putBool("popups", ui.popupsOn);
}

static void handleAlerts() {
    int idx;
    while ((idx = detector.popAlert()) >= 0) {
        const Detection& d = *detector.at(idx);
        sdLog.logDetection(d);

        char mac[18];
        fmtMac(d.mac, mac);
        Serial.printf("[ALERT] %s %s %s %ddBm %s%s\n", detectionTypeName(d.type),
                      confidenceName(d.conf), mac, d.rssi,
                      d.vendor ? d.vendor : "", d.following ? " FOLLOWING" : "");

        bool worthPopup = d.following || d.conf != Confidence::CONF_LOW;
        if (ui.popupsOn && worthPopup && ui.screen() != Screen::HUNT) {
            memcpy(popupMac, d.mac, 6);
            popupSrc = d.src;
            ui.flash();
            ui.drawPopup(d);
            popupUntil = millis() + POPUP_MS;
        }
    }
}

// --- Desktop mode: everything else is paused while it is shown ---

static bool deskBle = false, deskWifi = false;

static void enterDesktop() {
    deskBle = detector.bleRunning();
    deskWifi = detector.wifiRunning();
    detector.lockChannel(0);
    detector.stopBLE();
    detector.stopWiFi();
    popupUntil = 0;
    sdLog.flush();
    ui.wake();
    Serial.println("[DESK] desktop mode: scanning paused");
    ui.go(Screen::DESKTOP);
    netSync.startDesktop();
}

static void exitDesktop() {
    ui.showMessage("RESUMING SCAN", "Leaving desktop mode...", C_ACCENT);
    netSync.stopDesktop();
    if (deskWifi) detector.startWiFi();
    if (deskBle) detector.startBLE();
    Serial.println("[DESK] desktop mode off: scanning restored");
    ui.wake();
    ui.go(Screen::HOME);
}

static void runPortal() {
    char apPass[12];
    detector.stopBLE();
    detector.stopWiFi();
    ui.showMessage("WIFI SETUP", "Starting setup hotspot...", C_ACCENT);
    netSync.portalBegin(apPass, sizeof(apPass));
    ui.showPortal(apPass);
    Serial.printf("[NET] portal heap=%u\n", ESP.getFreeHeap());
    uint32_t savedAt = 0;
    for (;;) {
        netSync.portalLoop();
        if (netSync.portalSaved() && !savedAt) {
            savedAt = millis();
            ui.showMessage("SAVED", "Rebooting...", C_OK);
        }
        if (savedAt && millis() - savedAt > 1500) break;
        TouchPoint tp = touch.read();
        bool serialCancel = Serial.available() && Serial.read() == 'c';
        if (!savedAt && (serialCancel || (tp.pressed && Display::hit(Layout::PORTAL_CANCEL, tp.x, tp.y)))) {
            ui.showMessage("CANCELLED", "Rebooting...", C_MUTED);
            delay(800);
            break;
        }
        delay(5);
    }
    ESP.restart();   // cleanest way back to sniffer mode
}

// --- Per-screen touch handling ---

static void touchHome(int16_t x, int16_t y) {
    using namespace Layout;
    if (Display::hit(HOME_PANEL, x, y))          openDetail(detector.topAlert());
    else if (Display::hit(HOME_TILE_BLE, x, y))  openList(ListMode::BLE);
    else if (Display::hit(HOME_TILE_WIFI, x, y)) openList(ListMode::WIFI);
    else if (Display::hit(HOME_TILE_THR, x, y))  openList(ListMode::ALERTS);
    else if (Display::hit(HOME_ALERTS, x, y))    openList(ListMode::ALERTS);
    else if (Display::hit(HOME_BLE, x, y))       openList(ListMode::BLE);
    else if (Display::hit(HOME_WIFI, x, y))      openList(ListMode::WIFI);
    else if (Display::hit(HOME_SET, x, y))       ui.go(Screen::SETTINGS);
    else if (Display::hit(HOME_LORA, x, y))      ui.go(Screen::LORA);
    else if (Display::hit(HOME_DESK, x, y))      enterDesktop();
}

static void touchLora(int16_t x, int16_t y) {
    if (Display::hit(Layout::LORA_BACK, x, y)) ui.go(Screen::HOME);
}

static void touchList(int16_t x, int16_t y) {
    using namespace Layout;
    if (Display::hit(LIST_BACK, x, y)) {
        ui.go(Screen::HOME);
    } else if (Display::hit(LIST_UP, x, y)) {
        ui.scrollList(-1);
        ui.redraw();
    } else if (Display::hit(LIST_DOWN, x, y)) {
        ui.scrollList(1);
        ui.redraw();
    } else {
        openDetail(ui.rowAt(y));
    }
}

static void touchDetail(int16_t x, int16_t y) {
    using namespace Layout;
    int idx = ui.selected();
    if (Display::hit(DET_BACK, x, y)) {
        ui.go(Screen::LIST);
    } else if (idx >= 0 && Display::hit(DET_HUNT, x, y)) {
        ui.go(Screen::HUNT);
    } else if (idx >= 0 && Display::hit(DET_IGNORE, x, y)) {
        Detection* d = detector.at(idx);
        d->ignored = !d->ignored;
        ui.redraw();
    }
}

static void touchHunt(int16_t x, int16_t y) {
    if (Display::hit(Layout::HUNT_BACK, x, y)) ui.go(Screen::DETAIL);
}

static void touchSettings(int16_t x, int16_t y) {
    using namespace Layout;
    if (Display::hit(SET_BLE, x, y)) {
        if (detector.bleRunning()) detector.stopBLE(); else detector.startBLE();
        ui.redraw();
    } else if (Display::hit(SET_WIFI, x, y)) {
        if (detector.wifiRunning()) detector.stopWiFi(); else detector.startWiFi();
        ui.redraw();
    } else if (Display::hit(SET_DIM, x, y)) {
        ui.setBrightness(ui.brightness() > 45 ? ui.brightness() - 25 : 20);
        saveSettings();
        ui.refresh();
    } else if (Display::hit(SET_BRIGHT, x, y)) {
        ui.setBrightness(ui.brightness() < 230 ? ui.brightness() + 25 : 255);
        saveSettings();
        ui.refresh();
    } else if (Display::hit(SET_POPUP, x, y)) {
        ui.popupsOn = !ui.popupsOn;
        saveSettings();
        ui.redraw();
    } else if (Display::hit(SET_CAL, x, y)) {
        runTouchCalibration();
        ui.redraw();
    } else if (Display::hit(SET_CLEAR, x, y)) {
        detector.clearAll();
        ui.showMessage("CLEARED", "All tracked devices removed", C_OK);
        delay(700);
        ui.redraw();
    } else if (Display::hit(SET_UNIGN, x, y)) {
        for (int i = 0; i < detector.capacity(); i++) detector.at(i)->ignored = false;
        ui.showMessage("DONE", "All devices un-ignored", C_OK);
        delay(700);
        ui.redraw();
    } else if (Display::hit(SET_STEALTH, x, y)) {
        bool on = !detector.stealthMode();
        detector.setStealthMode(on);
        if (on) {
            ui.setBrightness(STEALTH_BRIGHTNESS);
        } else {
            ui.setBrightness(prefs.getUChar("bright", BACKLIGHT_DEFAULT));
        }
        ui.redraw();
    } else if (Display::hit(SET_PCAP, x, y)) {
        if (pcapLog.running()) {
            pcapLog.stop();
        } else {
            if (!pcapLog.begin())
                ui.showMessage("PCAP", "SD card not ready", C_DANGER);
        }
        ui.redraw();
    } else if (Display::hit(SET_NET, x, y)) {
        runPortal();
    } else if (Display::hit(SET_BACK, x, y)) {
        ui.go(Screen::HOME);
    }
}

// --- Arduino entry points ---

void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\n[+] " BRAND_NAME " Device Detector v3.0");

    prefs.begin("detector", false);
    ui.init();
    ui.setBrightness(prefs.getUChar("bright", BACKLIGHT_DEFAULT));
    ui.popupsOn = prefs.getBool("popups", true);
    touch.init();
    recordBoot();
    ui.resetInfo = resetInfo;
    ui.showBoot();

    if (!touch.loadCal() || touch.isTouched()) runTouchCalibration();

    bool sdOk = sdLog.init();
    Serial.printf("[+] SD Card: %s\n", sdOk ? "OK" : "Not found");

    analogSetPinAttenuation(PIN_BATTERY, ADC_11db);

    detector.init();
    detector.startBLE();
    detector.startWiFi();
    loraLink.begin();
    netSync.begin();
    Serial.printf("[+] Free heap: %u bytes\n", ESP.getFreeHeap());

    ui.go(Screen::HOME);
}

// Serial debug keys, so screens can be exercised without touching the panel
static void serialKeys() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == 'd' && ui.screen() != Screen::DESKTOP) enterDesktop();
        else if (c == 'h' && ui.screen() == Screen::DESKTOP) exitDesktop();
        else if (c == 'h') ui.go(Screen::HOME);
        else if (c == 'w' && ui.screen() == Screen::DESKTOP) netSync.demoNext();
        else if (c == 'p') runPortal();
    }
}

static void desktopLoop() {
    TouchPoint tp = touch.read();
    crumb.uptimeS = millis() / 1000;
    crumb.screen = (uint8_t)ui.screen();
    if (tp.pressed) {
        exitDesktop();
        return;
    }
    if (millis() - lastRefreshMs >= DESKTOP_ANIM_MS) {
        lastRefreshMs = millis();
        ui.refresh();
    }
    static uint32_t lastStat = 0;
    if (millis() - lastStat > 10000) {
        lastStat = millis();
        Serial.printf("[DESK] net=%u time=%s heap=%u min=%u\n", (unsigned)netSync.state(),
                      netSync.timeValid() ? "ok" : "none", ESP.getFreeHeap(), ESP.getMinFreeHeap());
    }
    delay(2);
}

void loop() {
    serialKeys();
    if (ui.screen() == Screen::DESKTOP) {
        desktopLoop();
        return;
    }
    uint32_t now = millis();
    detector.loop();
    loraLink.loop();
    bool loraFresh = loraLink.fresh();
    if (loraFresh && loraLink.latest().jamming) {
        detector.raiseLoraJamming(loraLink.latest().noiseFloor, loraLink.latest().baseline);
    }
    handleAlerts();
    ui.dimCheck();

    TouchPoint tp = touch.read();
    if (tp.pressed && ui.wake()) tp.pressed = false;  // first tap only wakes the screen

    crumb.uptimeS = now / 1000;
    crumb.screen = (uint8_t)ui.screen();
    if (tp.pressed) {
        crumb.tapX = tp.x;
        crumb.tapY = tp.y;
        crumb.tapAtS = crumb.uptimeS;
    }
    static uint32_t lastUptimeSave = 0;
    if (now - lastUptimeSave > 60000) {
        lastUptimeSave = now;
        prefs.putUInt("uptime", now / 1000);
    }

    if (popupUntil) {
        if (tp.pressed) {
            popupUntil = 0;
            int idx = detector.find(popupMac, popupSrc);
            if (Display::hit(Layout::POPUP, tp.x, tp.y) && idx >= 0) openDetail(idx);
            else ui.redraw();
        } else if (now > popupUntil) {
            popupUntil = 0;
            ui.redraw();
        }
        sdLog.flush();
        delay(10);
        return;
    }

    if (tp.pressed) {
        switch (ui.screen()) {
            case Screen::HOME:     touchHome(tp.x, tp.y); break;
            case Screen::LIST:     touchList(tp.x, tp.y); break;
            case Screen::DETAIL:   touchDetail(tp.x, tp.y); break;
            case Screen::HUNT:     touchHunt(tp.x, tp.y); break;
            case Screen::SETTINGS: touchSettings(tp.x, tp.y); break;
            case Screen::LORA:     touchLora(tp.x, tp.y); break;
            default: break;
        }
        lastRefreshMs = millis();
    }

    uint32_t interval = ui.screen() == Screen::HUNT ? HUNT_REFRESH_MS
                      : ui.screen() == Screen::LIST ? LIST_REFRESH_MS
                      : HOME_REFRESH_MS;
    bool loraUpdate = loraFresh && ui.screen() == Screen::LORA;
    if (loraUpdate || millis() - lastRefreshMs > interval) {
        ui.refresh();
        lastRefreshMs = millis();
    }

    static uint32_t lastStatMs = 0;
    if (millis() - lastStatMs > 10000) {
        lastStatMs = millis();
        Serial.printf("[STAT] ble=%d ap=%d client=%d threats=%d frames=%lu adverts=%lu heap=%u\n",
                      detector.countActive(Source::BLE), detector.countActive(Source::AP),
                      detector.countActive(Source::CLIENT), detector.countAlerts(),
                      (unsigned long)detector.framesSeen(), (unsigned long)detector.advertsSeen(),
                      ESP.getFreeHeap());
        Serial.printf("[STAT] lora %s seq=%u acc=%lu miss=%lu rej=%lu wifi=%s ch=%u\n",
                      loraLink.online() ? "online" : "offline", loraLink.latest().seq,
                      (unsigned long)loraLink.accepted(), (unsigned long)loraLink.missed(),
                      (unsigned long)loraLink.rejected(),
                      detector.wifiRunning() ? "sniff" : "off", detector.wifiChannel());
    }

    sdLog.flush();
    pcapLog.flush();
    delay(10);
}
