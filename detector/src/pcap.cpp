#include "pcap.h"

PcapWriter pcapLog;

// Standard pcap file header (little-endian)
struct __attribute__((packed)) PcapGlobalHeader {
    uint32_t magic;      // 0xA1B2C3D4
    uint16_t vMajor;     // 2
    uint16_t vMinor;     // 4
    int32_t  thiszone;   // 0
    uint32_t sigfigs;    // 0
    uint32_t snaplen;
    uint32_t network;    // DLT
};

struct __attribute__((packed)) PcapPacketHeader {
    uint32_t tsSec;
    uint32_t tsUsec;
    uint32_t inclLen;
    uint32_t origLen;
};

bool PcapWriter::begin() {
    if (_running) return true;
    if (!SD.cardType()) return false;

    if (!SD.exists("/pcap")) SD.mkdir("/pcap");

    char path[32];
    for (int n = 1; n < 1000; n++) {
        snprintf(path, sizeof(path), "/pcap/capture_%03d.pcap", n);
        if (!SD.exists(path)) break;
    }
    _file = SD.open(path, FILE_WRITE);
    if (!_file) {
        Serial.printf("[PCAP] Could not open %s\n", path);
        return false;
    }
    strlcpy(_name, path + 6, sizeof(_name));

    PcapGlobalHeader gh = {};
    gh.magic = 0xA1B2C3D4;
    gh.vMajor = 2;
    gh.vMinor = 4;
    gh.snaplen = PCAP_SNAPLEN;
    gh.network = PCAP_DLT_80211;
    _file.write((const uint8_t*)&gh, sizeof(gh));
    _file.flush();

    _running = true;
    _packets = 0;
    Serial.printf("[PCAP] Capturing to %s\n", path);
    return true;
}

void PcapWriter::writePacket(const uint8_t* data, uint16_t len) {
    if (!_running) return;
    uint16_t capLen = len > PCAP_SNAPLEN ? PCAP_SNAPLEN : len;
    uint32_t ms = millis();

    PcapPacketHeader ph = {};
    ph.tsSec = ms / 1000;
    ph.tsUsec = (ms % 1000) * 1000;
    ph.inclLen = capLen;
    ph.origLen = len;

    _file.write((const uint8_t*)&ph, sizeof(ph));
    _file.write(data, capLen);
    _packets++;
}

void PcapWriter::flush() {
    if (!_running) return;
    uint32_t now = millis();
    if (now - _lastFlushMs > LOG_FLUSH_INTERVAL_MS) {
        _file.flush();
        _lastFlushMs = now;
    }
}

void PcapWriter::stop() {
    if (!_running) return;
    _file.flush();
    _file.close();
    _running = false;
    Serial.printf("[PCAP] Stopped: %lu packets\n", (unsigned long)_packets);
}
