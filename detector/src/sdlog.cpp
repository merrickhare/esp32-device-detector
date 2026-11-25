#include "sdlog.h"
#include <SPI.h>

SDLogger sdLog;

bool SDLogger::init() {
    // Deselect both chips
    pinMode(PIN_TFT_CS, OUTPUT);
    digitalWrite(PIN_TFT_CS, HIGH);
    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);

    // Raw GPIO read BEFORE SPI init — checks the pull-up at electrical level
    pinMode(PIN_SPI_MISO, INPUT);
    delay(5);
    int raw = digitalRead(PIN_SPI_MISO);
    Serial.printf("[SD] Raw GPIO %d read: %s\n", PIN_SPI_MISO, raw ? "HIGH (pull-up OK)" : "LOW (line held down)");

    // Init the global SPI instance
    SPI.begin(PIN_SPI_CLK, PIN_SPI_MISO, PIN_SPI_MOSI);

    Serial.printf("[SD] CS=%d  CLK=%d MISO=%d MOSI=%d\n",
                  PIN_SD_CS, PIN_SPI_CLK, PIN_SPI_MISO, PIN_SPI_MOSI);

    // SPI probe: read MISO with all CS high, then with SD selected
    SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
    uint8_t idle = SPI.transfer(0xFF);
    SPI.endTransaction();
    Serial.printf("[SD] SPI idle (all CS high): 0x%02X\n", idle);

    // 80+ clocks with CS high (SD spec init requirement)
    SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
    for (int i = 0; i < 10; i++) SPI.transfer(0xFF);
    SPI.endTransaction();

    digitalWrite(PIN_SD_CS, LOW);
    SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
    uint8_t sel = SPI.transfer(0xFF);
    SPI.endTransaction();
    digitalWrite(PIN_SD_CS, HIGH);
    Serial.printf("[SD] SPI with SD selected: 0x%02X\n", sel);

    // Try mount — slow clock first
    for (int attempt = 0; attempt < 3; attempt++) {
        uint32_t freq = (attempt == 0) ? 400000 : 4000000;
        if (SD.begin(PIN_SD_CS, SPI, freq)) goto mounted;
        Serial.printf("[SD] Attempt %d failed (freq %lu)\n", attempt + 1, (unsigned long)freq);
        SD.end();
        delay(200);
    }
    Serial.println("[SD] Card mount failed after 3 attempts");
    return false;

mounted:
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
