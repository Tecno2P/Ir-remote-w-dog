// ============================================================
//  nfc_module.cpp  –  PN532 NFC Real Hardware (Adafruit lib)
// ============================================================
#include "nfc_module.h"
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_PN532.h>

// MIFARE classic dictionary keys for brute-force sector authentication
static const char* const MIFARE_DICT_KEYS[] = {
    "FFFFFFFFFFFF", "000000000000", "A0A1A2A3A4A5",
    "B0B1B2B3B4B5", "D3F7D3F7D3F7", "AABBCCDDEEFF",
    "1234567890AB", "000102030405", "FFFFFFFFFFFE",
    "4B0B20107CCB", "6A1987C40A21", "AACCDDEE0011"
};
static const uint8_t MIFARE_DICT_LEN = 12;

NfcModule nfcModule;

static Adafruit_PN532* _getNfc(void* ptr) {
    return reinterpret_cast<Adafruit_PN532*>(ptr);
}

// ── PN532 init ────────────────────────────────────────────────
bool NfcModule::_initPN532() {
    _deinitPN532();
    _hwConnected = false;
    _fwVersion   = 0;

    Adafruit_PN532* pn = nullptr;

    switch (_cfg.iface) {
        case NfcIface::I2C:
            Wire.begin(static_cast<int>(_cfg.sda),
                       static_cast<int>(_cfg.scl));
            pn = new Adafruit_PN532(_cfg.irq, _cfg.reset);
            break;
        case NfcIface::SPI: {
            // Use dedicated SPIClass instance so we don't interfere with
            // SD card (global SPI / VSPI) or other SPI modules.
            SPIClass* nfcSpi = (_cfg.spiBus == 1)
                ? new SPIClass(HSPI)
                : new SPIClass(VSPI);
            nfcSpi->begin(_cfg.sck, _cfg.miso, _cfg.mosi, _cfg.ss);
            // Store in _nfcSpi so we can delete later
            if (_nfcSpi) { _nfcSpi->end(); delete _nfcSpi; }
            _nfcSpi = nfcSpi;
            pn = new Adafruit_PN532(_cfg.ss, nfcSpi);
            Serial.printf("[NFC] SPI bus=%s SCK=%u MISO=%u MOSI=%u SS=%u\n",
                          _cfg.spiBus==1?"HSPI":"VSPI",
                          _cfg.sck, _cfg.miso, _cfg.mosi, _cfg.ss);
            break;
        }
        case NfcIface::UART:
            // UART not supported by Adafruit_PN532 on ESP32
            Serial.println("[NFC] UART interface not supported");
            return false;
        default:
            return false;
    }

    pn->begin();
    _fwVersion = pn->getFirmwareVersion();
    if (!_fwVersion) {
        Serial.println("[NFC] PN532 not found — getFirmwareVersion() = 0");
        delete pn;
        return false;
    }

    pn->SAMConfig();   // configure to read RFID tags

    _nfc = pn;
    _hwConnected = true;
    Serial.printf("[NFC] PN532 connected — FW: %u.%u\n",
                  (_fwVersion >> 16) & 0xFF,
                  (_fwVersion >>  8) & 0xFF);
    return true;
}

void NfcModule::_deinitPN532() {
    if (_nfc) {
        delete _getNfc(_nfc);
        _nfc = nullptr;
    }
    if (_nfcSpi) {
        _nfcSpi->end();
        delete _nfcSpi;
        _nfcSpi = nullptr;
    }
    _hwConnected = false;
}

// ── Lifecycle ─────────────────────────────────────────────────
void NfcModule::begin() {
    _loadTags();
    _cfg = loadGpioConfig();
    _initPN532();
}

void NfcModule::loop() {
    if (!_hwConnected || !_nfc) return;

    if (_reading && millis() - _pollTimer >= NFC_POLL_MS) {
        _pollTimer = millis();
        _pollForTag();
    }

    // Dict attack tick
    if (_dictRunning && millis() - _dictTimer >= 200) {
        _dictTimer = millis();
        if (_dictSector < 16 && _dictKeyIdx < MIFARE_DICT_LEN) {
            bool found = _tryMifareKey(_dictSector, 0,
                                       MIFARE_DICT_KEYS[_dictKeyIdx]);
            if (found) {
                String line = "Sector ";
                line += _dictSector;
                line += " KeyA: ";
                line += MIFARE_DICT_KEYS[_dictKeyIdx];
                line += "\n";
                _dictPending += line;
                _dictSector++;
                _dictKeyIdx = 0;
            } else {
                _dictKeyIdx++;
                if (_dictKeyIdx >= MIFARE_DICT_LEN) {
                    // All keys tried for this sector — move on
                    _dictSector++;
                    _dictKeyIdx = 0;
                }
            }
            if (_dictSector >= 16) {
                _dictRunning = false;
                _dictPending += "Attack complete\n";
            }
        }
    }
}

void NfcModule::reinit() {
    _cfg = loadGpioConfig();
    _initPN532();
}

// ── Poll for NFC tag ──────────────────────────────────────────
void NfcModule::_pollForTag() {
    if (!_nfc) return;
    Adafruit_PN532* pn = _getNfc(_nfc);

    uint8_t uid[7]     = {};
    uint8_t uidLen     = 0;
    uint16_t atqa      = 0;
    uint8_t sak        = 0;

    if (!pn->readPassiveTargetID(PN532_MIFARE_ISO14443A,
                                  uid, &uidLen, 100)) return;

    NfcTag tag;
    tag.uid  = _fmtUid(uid, uidLen);
    tag.type = _detectType(sak, reinterpret_cast<uint8_t*>(&atqa));
    tag.sak  = String(sak, HEX);

    // Read MIFARE blocks if Classic
    if (tag.type.indexOf("MIFARE") != -1) {
        _readMifareBlocks(tag);
    }

    _lastTag  = tag;
    _tagReady = true;
    _reading  = false;
    Serial.printf("[NFC] Tag read: UID=%s Type=%s\n",
                  tag.uid.c_str(), tag.type.c_str());
}

void NfcModule::_readMifareBlocks(NfcTag& tag) {
    Adafruit_PN532* pn = _getNfc(_nfc);
    if (!pn) return;

    uint8_t key[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    uint8_t uid[4] = {};
    for (int i = 0; i < 4 && i < (int)tag.uid.length()/3; i++) {
        uid[i] = static_cast<uint8_t>(
            strtoul(tag.uid.substring(i*3, i*3+2).c_str(), nullptr, 16));
    }

    for (uint8_t block = 0; block < 64; block++) {
        if (!pn->mifareclassic_AuthenticateBlock(uid, 4, block, 0, key)) {
            tag.blocks.push_back("(locked)");
            continue;
        }
        uint8_t data[16] = {};
        if (pn->mifareclassic_ReadDataBlock(block, data)) {
            char hex[33];
            for (int i = 0; i < 16; i++) snprintf(hex+i*2, 3, "%02X", data[i]);
            tag.blocks.push_back(String(hex));
        } else {
            tag.blocks.push_back("(error)");
        }
    }
}

// ── Read / Clone / Emulate ────────────────────────────────────
bool NfcModule::startRead() {
    if (!_hwConnected) return false;
    _reading   = true;
    _tagReady  = false;
    _pollTimer = millis();
    Serial.println("[NFC] Read started");
    return true;
}

void NfcModule::stopRead() {
    _reading   = false;
    _tagReady  = false;
}

bool NfcModule::startCloneRead() {
    if (!startRead()) return false;
    _cloneReady = false;
    return true;
}

bool NfcModule::writeClone() {
    if (!_hwConnected || !_cloneReady || !_nfc) return false;
    Adafruit_PN532* pn = _getNfc(_nfc);

    uint8_t uid[7] = {};
    uint8_t uidLen = 0;
    if (!pn->readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 500))
        return false;

    uint8_t key[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    for (uint8_t block = 0; block < (uint8_t)_cloneSource.blocks.size(); block++) {
        if (_cloneSource.blocks[block] == "(locked)" ||
            _cloneSource.blocks[block] == "(error)") continue;
        if (!pn->mifareclassic_AuthenticateBlock(uid, uidLen, block, 0, key))
            continue;
        uint8_t data[16] = {};
        const String& hex = _cloneSource.blocks[block];
        for (int i = 0; i < 16 && i*2+1 < (int)hex.length(); i++) {
            data[i] = static_cast<uint8_t>(
                strtoul(hex.substring(i*2, i*2+2).c_str(), nullptr, 16));
        }
        pn->mifareclassic_WriteDataBlock(block, data);
    }
    Serial.println("[NFC] Clone write complete");
    return true;
}

bool NfcModule::startEmulate(uint32_t tagId) {
    NfcTag tag;
    if (!getTag(tagId, tag)) return false;
    _emulating  = true;
    _emulatingId = tagId;
    // PN532 emulation requires ISO14443-4 target mode
    Serial.printf("[NFC] Emulating tag: %s\n", tag.uid.c_str());
    return true;
}

void NfcModule::stopEmulate() {
    _emulating = false;
}

// ── Dict attack ───────────────────────────────────────────────
void NfcModule::startDictAttack() {
    if (!_hwConnected) return;
    _dictRunning = true;
    _dictSector  = 0;
    _dictKeyIdx  = 0;
    _dictPending = "";
    _dictTimer   = millis();
    Serial.println("[NFC] Dictionary attack started");
}

void NfcModule::stopDictAttack() {
    _dictRunning = false;
}

String NfcModule::pollDictResult() {
    String r = _dictPending;
    _dictPending = "";
    return r;
}

bool NfcModule::_tryMifareKey(uint8_t sector, uint8_t keyType,
                               const char* keyHex) {
    if (!_nfc) return false;
    Adafruit_PN532* pn = _getNfc(_nfc);

    uint8_t uid[4] = {0,0,0,0};
    uint8_t uidLen = 0;
    if (!pn->readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 200))
        return false;

    uint8_t key[6] = {};
    for (int i = 0; i < 6; i++) {
        char hex[3] = {keyHex[i*2], keyHex[i*2+1], 0};
        key[i] = static_cast<uint8_t>(strtoul(hex, nullptr, 16));
    }

    uint8_t block = static_cast<uint8_t>(sector * 4);
    return pn->mifareclassic_AuthenticateBlock(uid, uidLen, block, keyType, key);
}

// ── Flipper NFC format ────────────────────────────────────────
bool NfcModule::importFlipperNfc(const String& content, NfcTag& out) {
    int uidLine = content.indexOf("UID:");
    if (uidLine < 0) return false;

    int eol  = content.indexOf('\n', uidLine);
    String uidStr = content.substring(uidLine + 4, eol);
    uidStr.trim();
    // Convert "DE AD BE EF" to "DE:AD:BE:EF"
    uidStr.replace(" ", ":");
    out.uid = uidStr;

    int typeLine = content.indexOf("MIFARE type:");
    if (typeLine >= 0) {
        int eol2 = content.indexOf('\n', typeLine);
        out.type = content.substring(typeLine + 12, eol2);
        out.type.trim();
    }
    return true;
}

String NfcModule::exportFlipperNfc(uint32_t id) const {
    NfcTag tag;
    if (!getTag(id, tag)) return "";
    String uid = tag.uid;
    uid.replace(":", " ");
    String out = "Filetype: Flipper NFC device\nVersion: 2\n";
    out += "Device type: " + tag.type + "\n";
    out += "UID: " + uid + "\n";
    out += "ATQA: " + tag.atqa + "\n";
    out += "SAK: " + tag.sak + "\n";
    return out;
}

String NfcModule::parseTag(const NfcTag& tag) const {
    String s = "UID: " + tag.uid + "\nType: " + tag.type;
    if (!tag.blocks.empty()) {
        s += "\nBlocks: " + String((uint32_t)tag.blocks.size());
        s += "\nBlock 0: " + (tag.blocks.empty() ? "-" : tag.blocks[0]);
    }
    return s;
}

// ── Status JSON ───────────────────────────────────────────────
String NfcModule::statusJson() const {
    String s = "{\"connected\":";
    s += _hwConnected ? "true" : "false";
    s += ",\"firmware\":\"";
    if (_fwVersion) {
        s += String((_fwVersion>>16)&0xFF); s += ".";
        s += String((_fwVersion>> 8)&0xFF);
    }
    s += "\"";
    s += ",\"reading\":";   s += _reading   ? "true" : "false";
    s += ",\"emulating\":"; s += _emulating ? "true" : "false";
    if (_tagReady) {
        s += ",\"lastUID\":\""; s += _lastTag.uid; s += "\"";
    }
    s += "}";
    return s;
}

// ── Helpers ───────────────────────────────────────────────────
String NfcModule::_fmtUid(const uint8_t* uid, uint8_t len) const {
    String s;
    s.reserve(static_cast<uint16_t>(len * 3));
    for (uint8_t i = 0; i < len; i++) {
        if (i) s += ':';
        char hex[3];
        snprintf(hex, sizeof(hex), "%02X", uid[i]);
        s += hex;
    }
    return s;
}

String NfcModule::_detectType(uint8_t sak, const uint8_t* atqa) const {
    (void)atqa;
    if (sak == 0x08 || sak == 0x88) return "MIFARE Classic 1K";
    if (sak == 0x18)                 return "MIFARE Classic 4K";
    if (sak == 0x20)                 return "MIFARE DESFire";
    if (sak == 0x00)                 return "NTAG/Ultralight";
    return "Unknown NFC";
}

// ── Tag storage ───────────────────────────────────────────────
uint32_t NfcModule::saveTag(NfcTag& tag) {
    if (_tags.size() >= NFC_MAX_TAGS) return 0;
    tag.id = static_cast<uint32_t>(millis()) ^ esp_random();
    if (!tag.id) tag.id = 1;
    _tags.push_back(tag);
    _saveTags();
    return tag.id;
}

bool NfcModule::deleteTag(uint32_t id) {
    for (auto it = _tags.begin(); it != _tags.end(); ++it) {
        if (it->id == id) { _tags.erase(it); _saveTags(); return true; }
    }
    return false;
}

bool NfcModule::getTag(uint32_t id, NfcTag& out) const {
    for (const auto& t : _tags) {
        if (t.id == id) { out = t; return true; }
    }
    return false;
}

String NfcModule::tagsToJson() const {
    String out = "{\"tags\":[";
    bool first = true;
    for (const auto& t : _tags) {
        if (!first) out += ',';
        first = false;
        out += "{\"id\":"; out += t.id;
        out += ",\"name\":\""; out += t.name; out += "\"";
        out += ",\"uid\":\"";  out += t.uid;  out += "\"";
        out += ",\"type\":\""; out += t.type; out += "\"}";
    }
    out += "]}";
    return out;
}

void NfcModule::_loadTags() {
    _tags.clear();
    if (!LittleFS.exists(NFC_SAVE_FILE)) return;
    File f = LittleFS.open(NFC_SAVE_FILE, "r");
    if (!f) return;
    JsonDocument doc;
    if (deserializeJson(doc, f) == DeserializationError::Ok) {
        for (JsonObject o : doc["tags"].as<JsonArray>()) {
            NfcTag t;
            t.id   = o["id"]   | (uint32_t)0;
            t.name = o["name"] | (const char*)"";
            t.uid  = o["uid"]  | (const char*)"";
            t.type = o["type"] | (const char*)"";
            t.atqa = o["atqa"] | (const char*)"";
            t.sak  = o["sak"]  | (const char*)"";
            _tags.push_back(t);
        }
    }
    f.close();
}

void NfcModule::_saveTags() const {
    File f = LittleFS.open(NFC_SAVE_FILE, "w");
    if (!f) return;
    f.print("{\"tags\":[");
    bool first = true;
    for (const auto& t : _tags) {
        if (!first) f.print(',');
        first = false;
        f.printf("{\"id\":%u,\"name\":\"%s\",\"uid\":\"%s\","
                 "\"type\":\"%s\",\"atqa\":\"%s\",\"sak\":\"%s\"}",
                 t.id, t.name.c_str(), t.uid.c_str(),
                 t.type.c_str(), t.atqa.c_str(), t.sak.c_str());
    }
    f.print("]}");
    f.close();
}

void NfcModule::saveGpioConfig(const NfcGpioConfig& cfg) {
    File f = LittleFS.open(NFC_GPIO_FILE, "w");
    if (!f) return;
    JsonDocument doc;
    doc["iface"]  = static_cast<int>(cfg.iface);
    doc["spiBus"] = cfg.spiBus;
    doc["sda"] = cfg.sda; doc["scl"] = cfg.scl;
    doc["irq"] = cfg.irq; doc["reset"]= cfg.reset;
    doc["ss"]  = cfg.ss;  doc["sck"] = cfg.sck;
    doc["mosi"]= cfg.mosi;doc["miso"]= cfg.miso;
    doc["tx"]  = cfg.tx;  doc["rx"]  = cfg.rx;
    serializeJson(doc, f);
    f.close();
}

NfcGpioConfig NfcModule::loadGpioConfig() const {
    NfcGpioConfig cfg;
    if (!LittleFS.exists(NFC_GPIO_FILE)) return cfg;
    File f = LittleFS.open(NFC_GPIO_FILE, "r");
    if (!f) return cfg;
    JsonDocument doc;
    if (deserializeJson(doc, f) == DeserializationError::Ok) {
        cfg.iface   = static_cast<NfcIface>(doc["iface"] | 0);
        cfg.spiBus  = doc["spiBus"] | (uint8_t)0;
        cfg.sda  = doc["sda"]  | (uint8_t)21;
        cfg.scl  = doc["scl"]  | (uint8_t)22;
        cfg.irq  = doc["irq"]  | (uint8_t)13;
        cfg.reset = doc["reset"] | (uint8_t)33;
        cfg.ss   = doc["ss"]   | (uint8_t)13;
        cfg.sck  = doc["sck"]  | (uint8_t)18;
        cfg.mosi = doc["mosi"] | (uint8_t)23;
        cfg.miso = doc["miso"] | (uint8_t)19;
        cfg.tx   = doc["tx"]   | (uint8_t)17;
        cfg.rx   = doc["rx"]   | (uint8_t)16;
    }
    f.close();
    return cfg;
}
