#pragma once
#include "detection.h"

struct OuiEntry {
    uint8_t oui[3];
    const char* vendor;
    DetectionType type;
    Confidence conf;
};

struct Uuid16Entry {
    uint16_t uuid;
    const char* vendor;
    DetectionType type;
    Confidence conf;
};

struct NameEntry {
    const char* substr;
    const char* vendor;
    DetectionType type;
    Confidence conf;
};

struct SsidEntry {
    const char* prefix;
    const char* vendor;
    DetectionType type;
    Confidence conf;
};

struct MfgIdEntry {
    uint16_t companyId;
    const char* vendor;
    DetectionType type;
    Confidence conf;
};

namespace Signatures {
    DetectionType lookupOui(const uint8_t* mac, const char** vendor, Confidence* conf);
    const char*   ouiVendor(const uint8_t* mac);
    DetectionType lookupUuid16(uint16_t uuid, const char** vendor, Confidence* conf);
    DetectionType lookupBtName(const char* name, const char** vendor, Confidence* conf);
    DetectionType lookupSsid(const char* ssid, const char** vendor, Confidence* conf);
    DetectionType lookupMfgId(uint16_t id, const char** vendor, Confidence* conf);
    const char*   mfgVendor(uint16_t id);
}
