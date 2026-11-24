#include "signatures.h"
#include <string.h>
#include <ctype.h>

using DT = DetectionType;
using CF = Confidence;

// MAC prefix → device. Type UNKNOWN entries only label the vendor.
static const OuiEntry ouiTable[] = {
    {{0x4C, 0x57, 0xCA}, "Flock Safety",  DT::FLOCK_CAMERA,  CF::CONF_HIGH},
    {{0x00, 0x25, 0xDF}, "Axon/Taser",    DT::AXON_BODYCAM,  CF::CONF_HIGH},
    {{0x44, 0x19, 0xB6}, "Hikvision",     DT::CAMERA,        CF::CONF_MED},
    {{0xBC, 0xAD, 0x28}, "Hikvision",     DT::CAMERA,        CF::CONF_MED},
    {{0x3C, 0xEF, 0x8C}, "Dahua",         DT::CAMERA,        CF::CONF_MED},
    {{0x00, 0x40, 0x8C}, "Axis Comms",    DT::CAMERA,        CF::CONF_MED},
    {{0xAC, 0xCC, 0x8E}, "Axis Comms",    DT::CAMERA,        CF::CONF_MED},
    {{0x2C, 0xAA, 0x8E}, "Wyze",          DT::CAMERA,        CF::CONF_LOW},
    {{0xD0, 0x3F, 0x27}, "Wyze",          DT::CAMERA,        CF::CONF_LOW},
    {{0xAC, 0x23, 0x3F}, "Shenzhen Cam",  DT::CAMERA,        CF::CONF_MED},
    {{0xE0, 0x46, 0xEE}, "Netgear/Arlo",  DT::CAMERA,        CF::CONF_MED},
    {{0x80, 0x7D, 0x3A}, "Ring",          DT::RING_DEVICE,   CF::CONF_HIGH},
    {{0x60, 0x60, 0x1F}, "DJI",           DT::DRONE_REMOTEID,CF::CONF_MED},
    {{0x34, 0xD2, 0x62}, "DJI",           DT::DRONE_REMOTEID,CF::CONF_MED},
    {{0x80, 0xE1, 0x26}, "Flipper Zero",  DT::FLIPPER_ZERO,  CF::CONF_MED},
    {{0x80, 0xE1, 0x27}, "Flipper Zero",  DT::FLIPPER_ZERO,  CF::CONF_MED},
    {{0xDC, 0xA6, 0x32}, "Raspberry Pi",  DT::HACKER_TOOL,   CF::CONF_LOW},
    {{0xB8, 0x27, 0xEB}, "Raspberry Pi",  DT::HACKER_TOOL,   CF::CONF_LOW},
    {{0x28, 0xCD, 0xC1}, "Raspberry Pi",  DT::HACKER_TOOL,   CF::CONF_LOW},
    {{0xE4, 0x5F, 0x01}, "Raspberry Pi",  DT::HACKER_TOOL,   CF::CONF_LOW},
    {{0x00, 0x13, 0x37}, "Hak5",          DT::HACKER_TOOL,   CF::CONF_HIGH},
    {{0x34, 0x94, 0x54}, "Espressif",     DT::UNKNOWN,       CF::CONF_LOW},
    {{0xAC, 0x67, 0xB2}, "Espressif",     DT::UNKNOWN,       CF::CONF_LOW},
    {{0xD8, 0xA0, 0x1D}, "Espressif",     DT::UNKNOWN,       CF::CONF_LOW},
    {{0x24, 0x6F, 0x28}, "Espressif",     DT::UNKNOWN,       CF::CONF_LOW},
    {{0xF4, 0x2D, 0xC9}, "Espressif",     DT::UNKNOWN,       CF::CONF_LOW},
};
static const int ouiCount = sizeof(ouiTable) / sizeof(ouiTable[0]);

// 16-bit service UUIDs (advertised list or service data)
static const Uuid16Entry uuidTable[] = {
    {0x3081, "Flipper Zero",    DT::FLIPPER_ZERO,   CF::CONF_HIGH},
    {0x3082, "Flipper Zero",    DT::FLIPPER_ZERO,   CF::CONF_HIGH},
    {0x3083, "Flipper Zero",    DT::FLIPPER_ZERO,   CF::CONF_HIGH},
    {0xFEED, "Tile",            DT::TILE_TRACKER,   CF::CONF_HIGH},
    {0xFEEC, "Tile",            DT::TILE_TRACKER,   CF::CONF_HIGH},
    {0xFD5A, "Samsung SmartTag",DT::SAMSUNG_TAG,    CF::CONF_HIGH},
    {0xFD44, "Find My Network", DT::FINDMY,         CF::CONF_MED},
    {0xFFFA, "Drone RemoteID",  DT::DRONE_REMOTEID, CF::CONF_HIGH},
};
static const int uuidCount = sizeof(uuidTable) / sizeof(uuidTable[0]);

// Device name substrings (case-insensitive)
static const NameEntry nameTable[] = {
    {"flipper",    "Flipper Zero",  DT::FLIPPER_ZERO,  CF::CONF_HIGH},
    {"flock",      "Flock Safety",  DT::FLOCK_CAMERA,  CF::CONF_MED},
    {"penguin",    "Flock Safety",  DT::FLOCK_CAMERA,  CF::CONF_LOW},
    {"axon",       "Axon BWC",      DT::AXON_BODYCAM,  CF::CONF_MED},
    {"bodyworn",   "Body Camera",   DT::AXON_BODYCAM,  CF::CONF_MED},
    {"bwcdevice",  "Body Camera",   DT::AXON_BODYCAM,  CF::CONF_HIGH},
    {"dji",        "DJI Drone",     DT::DRONE_REMOTEID,CF::CONF_MED},
    {"mavic",      "DJI Drone",     DT::DRONE_REMOTEID,CF::CONF_MED},
    {"skydio",     "Skydio Drone",  DT::DRONE_REMOTEID,CF::CONF_MED},
    {"pwnagotchi", "Pwnagotchi",    DT::PWNAGOTCHI,    CF::CONF_HIGH},
    {"marauder",   "ESP32 Marauder",DT::HACKER_TOOL,   CF::CONF_HIGH},
    {"deauth",     "Deauther",      DT::HACKER_TOOL,   CF::CONF_MED},
    {"hc-05",      "HC-05 Module",  DT::CARD_SKIMMER,  CF::CONF_LOW},
    {"hc-06",      "HC-06 Module",  DT::CARD_SKIMMER,  CF::CONF_LOW},
    {"ray-ban",    "Meta Glasses",  DT::META_GLASSES,  CF::CONF_HIGH},
    {"rayban",     "Meta Glasses",  DT::META_GLASSES,  CF::CONF_HIGH},
    {"oakley meta","Meta Glasses",  DT::META_GLASSES,  CF::CONF_HIGH},
    {"echo",       "Amazon Echo",   DT::SMART_SPEAKER, CF::CONF_LOW},
};
static const int nameCount = sizeof(nameTable) / sizeof(nameTable[0]);

// SSID prefixes (case-insensitive)
static const SsidEntry ssidTable[] = {
    {"flock-",      "Flock Safety",    DT::FLOCK_CAMERA,   CF::CONF_HIGH},
    {"alpr",        "ALPR System",     DT::ALPR,           CF::CONF_HIGH},
    {"p2p-cam",     "IP Camera P2P",   DT::CAMERA,         CF::CONF_MED},
    {"ipcam",       "IP Camera",       DT::CAMERA,         CF::CONF_MED},
    {"ring-",       "Ring Camera",     DT::RING_DEVICE,    CF::CONF_MED},
    {"arlo",        "Arlo Camera",     DT::CAMERA,         CF::CONF_MED},
    {"wyze",        "Wyze Camera",     DT::CAMERA,         CF::CONF_LOW},
    {"blink",       "Blink Camera",    DT::CAMERA,         CF::CONF_LOW},
    {"shotspotter", "ShotSpotter",     DT::CUSTOM_ALERT,   CF::CONF_HIGH},
    {"tello-",      "DJI Tello",       DT::DRONE_REMOTEID, CF::CONF_MED},
    {"mavic",       "DJI Drone",       DT::DRONE_REMOTEID, CF::CONF_MED},
    {"skydio",      "Skydio Drone",    DT::DRONE_REMOTEID, CF::CONF_MED},
    {"pineapple",   "WiFi Pineapple",  DT::HACKER_TOOL,    CF::CONF_HIGH},
    {"marauder",    "ESP32 Marauder",  DT::HACKER_TOOL,    CF::CONF_HIGH},
    {"pwned",       "Deauther",        DT::HACKER_TOOL,    CF::CONF_MED},
};
static const int ssidCount = sizeof(ssidTable) / sizeof(ssidTable[0]);

// BLE company IDs. UNKNOWN entries only label the vendor.
static const MfgIdEntry mfgIdTable[] = {
    {0x01AB, "Meta",           DT::META_GLASSES,  CF::CONF_MED},
    {0x058E, "Meta",           DT::META_GLASSES,  CF::CONF_MED},
    {0x004C, "Apple",          DT::UNKNOWN,       CF::CONF_LOW},
    {0x0075, "Samsung",        DT::UNKNOWN,       CF::CONF_LOW},
    {0x00E0, "Google",         DT::UNKNOWN,       CF::CONF_LOW},
    {0x0006, "Microsoft",      DT::UNKNOWN,       CF::CONF_LOW},
    {0x0059, "Nordic Semi",    DT::UNKNOWN,       CF::CONF_LOW},
    {0x0171, "Amazon",         DT::UNKNOWN,       CF::CONF_LOW},
    {0x0087, "Garmin",         DT::UNKNOWN,       CF::CONF_LOW},
    {0x0157, "Huami",          DT::UNKNOWN,       CF::CONF_LOW},
    {0x038F, "Xiaomi",         DT::UNKNOWN,       CF::CONF_LOW},
    {0x02E5, "Espressif",      DT::UNKNOWN,       CF::CONF_LOW},
};
static const int mfgIdCount = sizeof(mfgIdTable) / sizeof(mfgIdTable[0]);

static bool containsNoCase(const char* hay, const char* needle) {
    size_t n = strlen(needle);
    for (const char* p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && tolower((unsigned char)p[i]) == needle[i]) i++;
        if (i == n) return true;
    }
    return false;
}

static bool startsNoCase(const char* s, const char* prefix) {
    while (*prefix) {
        if (tolower((unsigned char)*s) != *prefix) return false;
        s++; prefix++;
    }
    return true;
}

DetectionType Signatures::lookupOui(const uint8_t* mac, const char** vendor, Confidence* conf) {
    *vendor = nullptr;
    *conf = CF::CONF_LOW;
    for (int i = 0; i < ouiCount; i++) {
        if (memcmp(mac, ouiTable[i].oui, 3) == 0) {
            *vendor = ouiTable[i].vendor;
            *conf = ouiTable[i].conf;
            return ouiTable[i].type;
        }
    }
    return DT::UNKNOWN;
}

const char* Signatures::ouiVendor(const uint8_t* mac) {
    for (int i = 0; i < ouiCount; i++) {
        if (memcmp(mac, ouiTable[i].oui, 3) == 0) return ouiTable[i].vendor;
    }
    return nullptr;
}

DetectionType Signatures::lookupUuid16(uint16_t uuid, const char** vendor, Confidence* conf) {
    *vendor = nullptr;
    *conf = CF::CONF_LOW;
    for (int i = 0; i < uuidCount; i++) {
        if (uuidTable[i].uuid == uuid) {
            *vendor = uuidTable[i].vendor;
            *conf = uuidTable[i].conf;
            return uuidTable[i].type;
        }
    }
    return DT::UNKNOWN;
}

DetectionType Signatures::lookupBtName(const char* name, const char** vendor, Confidence* conf) {
    *vendor = nullptr;
    *conf = CF::CONF_LOW;
    for (int i = 0; i < nameCount; i++) {
        if (containsNoCase(name, nameTable[i].substr)) {
            *vendor = nameTable[i].vendor;
            *conf = nameTable[i].conf;
            return nameTable[i].type;
        }
    }
    return DT::UNKNOWN;
}

DetectionType Signatures::lookupSsid(const char* ssid, const char** vendor, Confidence* conf) {
    *vendor = nullptr;
    *conf = CF::CONF_LOW;
    for (int i = 0; i < ssidCount; i++) {
        if (startsNoCase(ssid, ssidTable[i].prefix)) {
            *vendor = ssidTable[i].vendor;
            *conf = ssidTable[i].conf;
            return ssidTable[i].type;
        }
    }
    return DT::UNKNOWN;
}

DetectionType Signatures::lookupMfgId(uint16_t id, const char** vendor, Confidence* conf) {
    *vendor = nullptr;
    *conf = CF::CONF_LOW;
    for (int i = 0; i < mfgIdCount; i++) {
        if (mfgIdTable[i].companyId == id) {
            *vendor = mfgIdTable[i].vendor;
            *conf = mfgIdTable[i].conf;
            return mfgIdTable[i].type;
        }
    }
    return DT::UNKNOWN;
}

const char* Signatures::mfgVendor(uint16_t id) {
    for (int i = 0; i < mfgIdCount; i++) {
        if (mfgIdTable[i].companyId == id) return mfgIdTable[i].vendor;
    }
    return nullptr;
}
