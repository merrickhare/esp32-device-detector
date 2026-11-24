#include "touch.h"
#include <Preferences.h>

ResistiveTouch touch;

static void releaseAllPins() {
    pinMode(PIN_TOUCH_XP, INPUT);
    pinMode(PIN_TOUCH_YP, INPUT);
    pinMode(PIN_TOUCH_XM, INPUT);
    pinMode(PIN_TOUCH_YM, INPUT);
}

void ResistiveTouch::init() {
    releaseAllPins();
    _lastPressMs = 0;
}

int16_t ResistiveTouch::readAxis(uint8_t plusPin, uint8_t minusPin, uint8_t measurePin) {
    releaseAllPins();
    pinMode(plusPin, OUTPUT);
    pinMode(minusPin, OUTPUT);
    digitalWrite(plusPin, HIGH);
    digitalWrite(minusPin, LOW);
    delayMicroseconds(500);

    analogRead(measurePin);
    int32_t sum = 0;
    for (int i = 0; i < TOUCH_SAMPLES; i++) {
        sum += analogRead(measurePin);
    }
    releaseAllPins();
    return (int16_t)(sum / TOUCH_SAMPLES);
}

bool ResistiveTouch::isTouched() {
    // X+ HIGH, Y- LOW: Y+ only sees voltage when the layers touch
    releaseAllPins();
    pinMode(PIN_TOUCH_XP, OUTPUT);
    pinMode(PIN_TOUCH_YM, OUTPUT);
    digitalWrite(PIN_TOUCH_XP, HIGH);
    digitalWrite(PIN_TOUCH_YM, LOW);
    delayMicroseconds(500);

    analogRead(PIN_TOUCH_YP);
    int32_t sum = 0;
    for (int i = 0; i < TOUCH_SAMPLES; i++) {
        sum += analogRead(PIN_TOUCH_YP);
    }
    releaseAllPins();
    return (sum / TOUCH_SAMPLES) > TOUCH_PRESSURE_THRESHOLD;
}

bool ResistiveTouch::readRaw(int16_t& rx, int16_t& ry) {
    if (!isTouched()) return false;
    rx = readAxis(PIN_TOUCH_YP, PIN_TOUCH_YM, PIN_TOUCH_XP);
    ry = readAxis(PIN_TOUCH_XP, PIN_TOUCH_XM, PIN_TOUCH_YP);
    // Reject reads where the finger lifted mid-sample
    return isTouched();
}

TouchPoint ResistiveTouch::read() {
    TouchPoint pt = {0, 0, false};
    int16_t rx, ry;
    if (!readRaw(rx, ry)) {
        if (_down && ++_misses >= TOUCH_RELEASE_MISSES) _down = false;
        return pt;
    }
    _misses = 0;
    if (_down) return pt;  // report each tap once, not while held

    uint32_t now = millis();
    if (now - _lastPressMs < TOUCH_DEBOUNCE_MS) return pt;
    _down = true;

    float fx = _cal.a * rx + _cal.b * ry + _cal.c;
    float fy = _cal.d * rx + _cal.e * ry + _cal.f;
    pt.x = constrain((int)fx, 0, SCREEN_W - 1);
    pt.y = constrain((int)fy, 0, SCREEN_H - 1);
    pt.pressed = true;
    _lastPressMs = now;

    Serial.printf("[TOUCH] raw(%d,%d) -> screen(%d,%d)\n", rx, ry, pt.x, pt.y);
    return pt;
}

bool ResistiveTouch::computeCal(const int16_t sx[3], const int16_t sy[3],
                                const int16_t rx[3], const int16_t ry[3]) {
    float det = (float)(rx[0] - rx[2]) * (ry[1] - ry[2]) -
                (float)(rx[1] - rx[2]) * (ry[0] - ry[2]);
    if (fabsf(det) < 1.0f) return false;

    _cal.a = ((sx[0] - sx[2]) * (float)(ry[1] - ry[2]) - (sx[1] - sx[2]) * (float)(ry[0] - ry[2])) / det;
    _cal.b = ((rx[0] - rx[2]) * (float)(sx[1] - sx[2]) - (rx[1] - rx[2]) * (float)(sx[0] - sx[2])) / det;
    _cal.c = sx[0] - _cal.a * rx[0] - _cal.b * ry[0];

    _cal.d = ((sy[0] - sy[2]) * (float)(ry[1] - ry[2]) - (sy[1] - sy[2]) * (float)(ry[0] - ry[2])) / det;
    _cal.e = ((rx[0] - rx[2]) * (float)(sy[1] - sy[2]) - (rx[1] - rx[2]) * (float)(sy[0] - sy[2])) / det;
    _cal.f = sy[0] - _cal.d * rx[0] - _cal.e * ry[0];

    _hasCal = true;
    Serial.printf("[CAL] x = %.5f*rx + %.5f*ry + %.1f\n", _cal.a, _cal.b, _cal.c);
    Serial.printf("[CAL] y = %.5f*rx + %.5f*ry + %.1f\n", _cal.d, _cal.e, _cal.f);
    return true;
}

bool ResistiveTouch::loadCal() {
    Preferences p;
    p.begin("touch", true);
    size_t n = p.getBytes("cal", &_cal, sizeof(_cal));
    p.end();
    _hasCal = (n == sizeof(_cal));
    return _hasCal;
}

void ResistiveTouch::saveCal() {
    Preferences p;
    p.begin("touch", false);
    p.putBytes("cal", &_cal, sizeof(_cal));
    p.end();
}
