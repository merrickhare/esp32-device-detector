#pragma once
#include "config.h"
#include <SD.h>

class PcapWriter {
public:
    bool begin();
    void writePacket(const uint8_t* data, uint16_t len);
    void flush();
    void stop();
    bool running() const { return _running; }
    const char* fileName() const { return _name; }
    uint32_t packets() const { return _packets; }

private:
    bool _running = false;
    uint32_t _packets = 0;
    uint32_t _lastFlushMs = 0;
    char _name[24] = "";
    File _file;
};

extern PcapWriter pcapLog;
