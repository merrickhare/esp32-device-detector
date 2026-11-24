#include "sdlog.h"
#include <SPI.h>

SDLogger sdLog;

bool SDLogger::init() {
    SPI.begin(PIN_SPI_CLK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_SD_CS);
    if (!SD.begin(PIN_SD_CS)) {
        Serial.println("[SD] Card mount failed");
        return false;
    }
    Serial.printf("[SD] Card mounted. Size: %lluMB\n", SD.cardSize() / (1024 * 1024));
    if (!SD.exists("/logs")) SD.mkdir("/logs");

    // One file per boot: next free /logs/alerts_NNN.csv
    char path[32];
    for (int n = 1; n < 1000; n++) {
        snprintf(path, sizeof(path), "/logs/alerts_%03d.csv", n);
        if (!SD.exists(path)) break;
    }
    _logFile = SD.open(path, FILE_WRITE);
    if (!_logFile) {
        Serial.printf("[SD] Could not open %s\n", path);
        return false;
    }
    strlcpy(_name, path + 6, sizeof(_name));
    _logFile.println("uptime_s,mac,source,type,confidence,rssi,channel,vendor,name,hits,following");
    _logFile.flush();
    Serial.printf("[SD] Logging alerts to %s\n", path);
    _ready = true;
    return true;
}

static void csvField(File& f, const char* s) {
    f.print('"');
    for (; s && *s; s++) {
        if (*s == '"') f.print('"');
        f.print(*s);
    }
    f.print('"');
}

void SDLogger::logDetection(const Detection& d) {
    if (!_ready) return;
    char head[96];
    snprintf(head, sizeof(head), "%lu,%02X:%02X:%02X:%02X:%02X:%02X,%s,%s,%s,%d,%u,",
             (unsigned long)(millis() / 1000),
             d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5],
             sourceName(d.src), detectionTypeName(d.type), confidenceName(d.conf),
             d.rssi, d.channel);
    _logFile.print(head);
    csvField(_logFile, d.vendor);
    _logFile.print(',');
    csvField(_logFile, d.name);
    _logFile.printf(",%u,%d\n", d.hits, d.following ? 1 : 0);
    _lines++;
}

void SDLogger::flush() {
    if (!_ready) return;
    uint32_t now = millis();
    if (now - _lastFlushMs > LOG_FLUSH_INTERVAL_MS) {
        _logFile.flush();
        _lastFlushMs = now;
    }
}
