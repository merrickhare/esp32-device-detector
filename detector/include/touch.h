#pragma once
#include "config.h"

struct TouchPoint {
    int16_t x;
    int16_t y;
    bool pressed;
};

// screen_x = a*rx + b*ry + c ; screen_y = d*rx + e*ry + f
struct TouchCal {
    float a, b, c, d, e, f;
};

class ResistiveTouch {
public:
    void init();
    TouchPoint read();

    bool isTouched();
    bool readRaw(int16_t& rx, int16_t& ry);

    bool loadCal();
    void saveCal();
    bool computeCal(const int16_t sx[3], const int16_t sy[3],
                    const int16_t rx[3], const int16_t ry[3]);
    bool hasCal() const { return _hasCal; }

private:
    uint32_t _lastPressMs = 0;
    bool _down = false;
    uint8_t _misses = 0;
    TouchCal _cal = {0, 0, 0, 0, 0, 0};
    bool _hasCal = false;
    int16_t readAxis(uint8_t plusPin, uint8_t minusPin, uint8_t measurePin);
};

extern ResistiveTouch touch;
