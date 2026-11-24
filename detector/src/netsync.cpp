#include "netsync.h"
#include "config.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <time.h>

NetSync netSync;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static Preferences s_prefs;
static WebServer* s_web = nullptr;
static DNSServer* s_dns = nullptr;

// --- tiny JSON helpers (Open-Meteo / ip-api bodies are small and flat) ---

static bool jsonNum(const char* from, const char* key, float& out) {
    if (!from) return false;
    const char* p = strstr(from, key);
    if (!p) return false;
    p += strlen(key);
    char* end;
    float v = strtof(p, &end);
    if (end == p) return false;
    out = v;
    return true;
}

static bool jsonStr(const char* from, const char* key, char* out, size_t n) {
    if (!from) return false;
    const char* p = strstr(from, key);
    if (!p) return false;
    p += strlen(key);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) {
        char c = *p++;
        out[i++] = (c >= 32 && c < 127) ? c : '?';   // TFT fonts are ASCII only
    }
    out[i] = 0;
    return i > 0;
}

static void urlEncode(const char* in, char* out, size_t n) {
    size_t o = 0;
    for (; *in && *in != ',' && o + 4 < n; in++) {   // geocoder wants the bare place name
        unsigned char c = *in;
        if (isalnum(c) || c == '-' || c == '.' || c == '_') out[o++] = c;
        else if (c == ' ') out[o++] = '+';
        else o += snprintf(out + o, n - o, "%%%02X", c);
    }
    out[o] = 0;
}

static String htmlEscape(const String& s) {
    String o;
    o.reserve(s.length() + 8);
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&#39;"; break;
            default: o += c;
        }
    }
    return o;
}

// --- lifecycle ---

static void netTask(void*) {
    for (;;) netSync.taskBody();
}

void NetSync::begin() {
    s_prefs.begin("net", false);
    if (s_prefs.isKey("ssid")) s_prefs.getString("ssid", _ssid, sizeof(_ssid));
    if (s_prefs.isKey("pass")) s_prefs.getString("pass", _pass, sizeof(_pass));
    if (s_prefs.isKey("city")) s_prefs.getString("city", _city, sizeof(_city));
    if (s_prefs.isKey("place")) s_prefs.getString("place", _place, sizeof(_place));
    _unitF = s_prefs.getBool("unitF", true);
    _haveLoc = s_prefs.getBool("hasLoc", false);
    if (_haveLoc) {
        _lat = s_prefs.getFloat("lat", 0);
        _lon = s_prefs.getFloat("lon", 0);
    }
    _tzOffset = s_prefs.getInt("tz", 0);
    Serial.printf("[NET] %s, location %s\n", configured() ? "WiFi configured" : "no WiFi setup",
                  _haveLoc ? "cached" : "unknown");
    xTaskCreatePinnedToCore(netTask, "net", 8192, nullptr, 1, nullptr, 0);
}

bool NetSync::timeValid() const {
    return time(nullptr) > 1700000000;
}

bool NetSync::weather(Weather& out) const {
    portENTER_CRITICAL(&s_mux);
    out = _wx;
    portEXIT_CRITICAL(&s_mux);
    return out.valid;
}

void NetSync::startDesktop() {
    if (!configured()) {
        _state = NetState::NO_SETUP;
        return;
    }
    _state = NetState::CONNECTING;
    _active = true;
}

void NetSync::stopDesktop() {
    _active = false;
    uint32_t t0 = millis();
    while (_busy && millis() - t0 < 12000) delay(20);
    _state = NetState::IDLE;
}

// Runs forever on the "net" task; does nothing unless the desktop is open.
void NetSync::taskBody() {
    static uint32_t nextFetch = 0;
    if (!_active) {
        if (_busy) {
            WiFi.disconnect(false, false);   // keep STA mode up for the ESP-NOW receiver
            Serial.println("[NET] link down");
            _busy = false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        return;
    }
    if (!_busy) {
        _busy = true;
        nextFetch = 0;
    }

    if (WiFi.status() != WL_CONNECTED) {
        _state = NetState::CONNECTING;
        Serial.println("[NET] joining home WiFi");
        WiFi.setAutoReconnect(false);
        WiFi.begin(_ssid, _pass);
        uint32_t t0 = millis();
        while (_active && WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) vTaskDelay(pdMS_TO_TICKS(100));
        if (!_active) return;
        if (WiFi.status() != WL_CONNECTED) {
            _fail = "WIFI JOIN FAILED";
            _state = NetState::FAILED;
            Serial.printf("[NET] join failed (status %d)\n", WiFi.status());
            WiFi.disconnect(false, false);
            for (uint32_t w = 0; _active && w < NET_RETRY_MS; w += 100) vTaskDelay(pdMS_TO_TICKS(100));
            return;
        }
        Serial.printf("[NET] joined, ch %d rssi %d\n", WiFi.channel(), WiFi.RSSI());
    }

    if ((int32_t)(millis() - nextFetch) >= 0) {
        _state = NetState::SYNCING;
        bool ok = fetchWeather();
        nextFetch = millis() + (ok ? NET_REFRESH_MS : NET_RETRY_MS);
        _state = ok ? NetState::LIVE : NetState::FAILED;
    }
    vTaskDelay(pdMS_TO_TICKS(250));
}

// --- HTTP ---

bool NetSync::httpGet(const char* url, String& body) {
    WiFiClient client;
    HTTPClient http;
    http.setTimeout(6000);
    http.setConnectTimeout(6000);
    http.setUserAgent("ESP32");
    if (!http.begin(client, url)) return false;
    int code = http.GET();
    bool ok = code == 200;
    if (ok) body = http.getString();
    http.end();
    if (!ok) Serial.printf("[NET] HTTP %d\n", code);
    return ok && body.length() > 0;
}

bool NetSync::locate() {
    String body;
    char url[200];
    char place[32] = "";
    float lat, lon;
    if (_city[0]) {
        char q[100];
        urlEncode(_city, q, sizeof(q));
        snprintf(url, sizeof(url), "http://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&format=json", q);
        if (!httpGet(url, body)) { _fail = "GEOCODE FAILED"; return false; }
        const char* r = strstr(body.c_str(), "\"results\":[");
        if (!jsonNum(r, "\"latitude\":", lat) || !jsonNum(r, "\"longitude\":", lon)) {
            _fail = "CITY NOT FOUND";
            return false;
        }
        jsonStr(r, "\"name\":\"", place, sizeof(place));
    } else {
        if (!httpGet("http://ip-api.com/json/?fields=status,city,lat,lon", body)) { _fail = "LOCATE FAILED"; return false; }
        const char* b = body.c_str();
        if (!strstr(b, "\"success\"") || !jsonNum(b, "\"lat\":", lat) || !jsonNum(b, "\"lon\":", lon)) {
            _fail = "LOCATE FAILED";
            return false;
        }
        jsonStr(b, "\"city\":\"", place, sizeof(place));
    }
    _lat = lat;
    _lon = lon;
    strlcpy(_place, place[0] ? place : "LOCAL", sizeof(_place));
    _haveLoc = true;
    s_prefs.putFloat("lat", _lat);
    s_prefs.putFloat("lon", _lon);
    s_prefs.putString("place", _place);
    s_prefs.putBool("hasLoc", true);
    Serial.printf("[NET] location resolved: %s\n", _place);
    return true;
}

bool NetSync::fetchWeather() {
    if (!_haveLoc && !locate()) return false;
    if (!_active) return false;

    char url[360];
    snprintf(url, sizeof(url),
             "http://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f"
             "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m,is_day"
             "&daily=temperature_2m_max,temperature_2m_min&temperature_unit=%s&wind_speed_unit=%s"
             "&timezone=auto&forecast_days=1",
             _lat, _lon, _unitF ? "fahrenheit" : "celsius", _unitF ? "mph" : "kmh");
    String body;
    if (!httpGet(url, body)) { _fail = "WEATHER FETCH FAILED"; return false; }

    const char* b = body.c_str();
    const char* cur = strstr(b, "\"current\":{");
    const char* daily = strstr(b, "\"daily\":{");
    Weather w = {};
    float off, hum, code, day;
    bool ok = jsonNum(b, "\"utc_offset_seconds\":", off) &&
              jsonNum(cur, "\"temperature_2m\":", w.temp) &&
              jsonNum(cur, "\"weather_code\":", code) &&
              jsonNum(daily, "\"temperature_2m_max\":[", w.hi) &&
              jsonNum(daily, "\"temperature_2m_min\":[", w.lo);
    if (!ok) { _fail = "BAD WEATHER DATA"; return false; }
    jsonNum(cur, "\"wind_speed_10m\":", w.wind);
    if (jsonNum(cur, "\"relative_humidity_2m\":", hum)) w.humidity = (int)hum;
    w.day = !jsonNum(cur, "\"is_day\":", day) || day > 0.5f;
    w.code = (int)code;
    w.valid = true;
    w.stamp = millis();
    portENTER_CRITICAL(&s_mux);
    _wx = w;
    portEXIT_CRITICAL(&s_mux);

    // Local time: Open-Meteo gives the current UTC offset (DST-aware), SNTP gives UTC
    int32_t tz = (int32_t)off;
    if (!_sntpStarted || tz != _tzOffset) {
        _tzOffset = tz;
        s_prefs.putInt("tz", tz);
        configTime(tz, 0, "pool.ntp.org", "time.google.com");
        _sntpStarted = true;
    }
    for (int i = 0; i < 50 && _active && !timeValid(); i++) vTaskDelay(pdMS_TO_TICKS(100));
    Serial.printf("[NET] weather %.0f code %d, time %s\n", w.temp, w.code, timeValid() ? "synced" : "NOT synced");
    _fail = "";
    return true;
}

// --- Setup portal ---

void NetSync::handleRoot() {
    String h;
    h.reserve(2600);
    h += F("<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
           "<title>" BRAND_NAME " setup</title><style>body{background:#0a0f24;color:#e8ecff;font-family:sans-serif;"
           "max-width:420px;margin:auto;padding:16px}h2{color:#3cf;letter-spacing:2px}label{display:block;margin-top:14px}"
           "input,select{width:100%;padding:10px;margin-top:4px;box-sizing:border-box;background:#141b3a;color:#fff;"
           "border:1px solid #3cf;border-radius:6px}button{margin-top:20px;width:100%;padding:12px;background:#d8102a;"
           "color:#fff;border:0;border-radius:6px;font-size:16px}small{color:#8a93b8}</style></head><body>"
           "<h2>" BRAND_NAME " // SETUP</h2><form method=post action=/save>"
           "<label>Home WiFi name<input name=ssid list=nets maxlength=32 required value='");
    h += htmlEscape(_ssid);
    h += F("'></label><datalist id=nets>");
    h += _scanList;
    h += F("</datalist><label>WiFi password<input name=pass type=password maxlength=63 placeholder='");
    h += _pass[0] ? F("(unchanged if blank)") : F("");
    h += F("'></label><label>City for weather<input name=city maxlength=40 value='");
    h += htmlEscape(_city);
    h += F("'></label><small>Leave blank to estimate from your internet connection.</small>"
           "<label>Units<select name=unit><option value=F");
    h += _unitF ? F(" selected") : F("");
    h += F(">Fahrenheit / mph</option><option value=C");
    h += _unitF ? F("") : F(" selected");
    h += F(">Celsius / km/h</option></select></label><button>SAVE &amp; REBOOT</button></form>"
           "<p><small>Used only while the detector is in Desktop mode.</small></p></body></html>");
    s_web->send(200, "text/html", h);
}

void NetSync::handleSave() {
    String ssid = s_web->arg("ssid"), pass = s_web->arg("pass"), city = s_web->arg("city");
    ssid.trim();
    city.trim();
    if (ssid.length() == 0 || ssid.length() > 32 || pass.length() > 63 || city.length() > 40) {
        s_web->send(400, "text/plain", "Invalid input");
        return;
    }
    bool keepPass = pass.length() == 0 && ssid == _ssid;
    s_prefs.putString("ssid", ssid);
    if (!keepPass) s_prefs.putString("pass", pass);
    s_prefs.putString("city", city);
    s_prefs.putBool("unitF", s_web->arg("unit") != "C");
    s_prefs.putBool("hasLoc", city == _city && _haveLoc);
    s_web->send(200, "text/html",
                "<html><body style='background:#0a0f24;color:#3cf;font-family:sans-serif;text-align:center;padding:40px'>"
                "<h2>SAVED</h2><p>The detector is rebooting.</p></body></html>");
    Serial.println("[NET] setup saved");   // never print the credentials
    _portalSaved = true;
}

void NetSync::portalBegin(char* apPass, size_t n) {
    snprintf(apPass, n, "%08lu", (unsigned long)(esp_random() % 100000000UL));
    WiFi.mode(WIFI_AP_STA);
    int found = WiFi.scanNetworks();
    _scanList = "";
    for (int i = 0; i < found && i < 20; i++) {
        String s = WiFi.SSID(i);
        if (s.length() == 0 || _scanList.indexOf(">" + htmlEscape(s) + "<") >= 0) continue;
        _scanList += "<option value='" + htmlEscape(s) + "'>" + htmlEscape(s) + "</option>";
    }
    WiFi.scanDelete();
    WiFi.softAP(BRAND_AP_SSID, apPass);
    s_dns = new DNSServer();
    s_dns->start(53, "*", WiFi.softAPIP());
    s_web = new WebServer(80);
    s_web->on("/", HTTP_GET, [] { netSync.handleRoot(); });
    s_web->on("/save", HTTP_POST, [] { netSync.handleSave(); });
    s_web->onNotFound([] {   // captive portal: send every URL to the form
        s_web->sendHeader("Location", "http://192.168.4.1/");
        s_web->send(302, "text/plain", "");
    });
    s_web->begin();
    Serial.printf("[NET] setup portal up, %d networks listed\n", found);
}

void NetSync::portalLoop() {
    if (s_dns) s_dns->processNextRequest();
    if (s_web) s_web->handleClient();
}

// Serial demo: cycles every weather icon with sample data and sets a clock if none is synced
void NetSync::demoNext() {
    static const int codes[] = {0, 2, 3, 45, 61, 71, 95, 0};
    static int i = 0;
    Weather w = {};
    w.valid = true;
    w.code = codes[i % 8];
    w.day = i != 7;   // last one shows the night icon
    w.temp = 48 + i;
    w.hi = 55;
    w.lo = 31;
    w.wind = 8;
    w.humidity = 61;
    w.stamp = millis();
    i++;
    portENTER_CRITICAL(&s_mux);
    _wx = w;
    portEXIT_CRITICAL(&s_mux);
    strlcpy(_place, "DEMO DATA", sizeof(_place));
    _state = NetState::LIVE;
    if (!timeValid()) {
        struct timeval tv = {1795000000, 0};
        settimeofday(&tv, nullptr);
    }
    Serial.printf("[NET] demo weather code %d\n", w.code);
}
