#pragma once
#include "config.h"
#include "detection.h"
#include <SD.h>

class SDLogger {
public:
    bool init();
    void logDetection(const Detection& d);
    void flush();
    bool isReady() const { return _ready; }
    const char* fileName() const { return _name; }
    uint32_t lines() const { return _lines; }

private:
    bool _ready = false;
    uint32_t _lastFlushMs = 0;
    uint32_t _lines = 0;
    char _name[24] = "";
    File _logFile;
};

extern SDLogger sdLog;
