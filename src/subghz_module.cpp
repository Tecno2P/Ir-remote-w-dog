// ============================================================
//  subghz_module.cpp  –  CC1101 Sub-1GHz Real Implementation
//  Direct SPI register access — no external CC1101 library needed
// ============================================================
#include "subghz_module.h"
#include <SPI.h>

#define SUBGHZ_TAG "[SUBGHZ]"

// CC1101 registers
#define CC1101_SRES     0x30
#define CC1101_SRX      0x34
#define CC1101_STX      0x35
#define CC1101_SIDLE    0x36
#define CC1101_VERSION  0xF1  // status register 0x31 burst
#define CC1101_FREQ2    0x0D
#define CC1101_FREQ1    0x0E
#define CC1101_FREQ0    0x0F
#define CC1101_MDMCFG4  0x10
#define CC1101_MDMCFG2  0x12
#define CC1101_PKTCTRL0 0x08
#define CC1101_IOCFG2   0x00
#define CC1101_IOCFG0   0x02

static SPIClass* _cc1101Spi = nullptr;

SubGhzModule subGhzModule;

// ─────────────────────────────────────────────────────────────
void SubGhzModule::_spiByte(uint8_t b) {
    (void)_cc1101Spi->transfer(b);
}

void SubGhzModule::_spiWrite(uint8_t addr, uint8_t val) {
    digitalWrite(_cfg.cs, LOW);
    _cc1101Spi->transfer(addr & 0x3F);
    _cc1101Spi->transfer(val);
    digitalWrite(_cfg.cs, HIGH);
}

uint8_t SubGhzModule::_spiRead(uint8_t addr) {
    digitalWrite(_cfg.cs, LOW);
    _cc1101Spi->transfer((addr & 0x3F) | 0x80);
    uint8_t val = _cc1101Spi->transfer(0);
    digitalWrite(_cfg.cs, HIGH);
    return val;
}

uint8_t SubGhzModule::_spiReadStatus(uint8_t addr) {
    digitalWrite(_cfg.cs, LOW);
    _cc1101Spi->transfer((addr & 0x3F) | 0xC0);
    uint8_t val = _cc1101Spi->transfer(0);
    digitalWrite(_cfg.cs, HIGH);
    return val;
}

void SubGhzModule::_spiCommand(uint8_t cmd) {
    digitalWrite(_cfg.cs, LOW);
    _cc1101Spi->transfer(cmd);
    digitalWrite(_cfg.cs, HIGH);
}

void SubGhzModule::_reset() {
    _spiCommand(CC1101_SRES);
    delayMicroseconds(100);
}

bool SubGhzModule::_detectCC1101() {
    _reset();
    uint8_t ver = _spiReadStatus(0x31); // CC1101_VERSION status reg
    Serial.printf(SUBGHZ_TAG " CC1101 version byte: 0x%02X\n", ver);
    return (ver == 0x14 || ver == 0x04);
}

// ─────────────────────────────────────────────────────────────
void SubGhzModule::begin() {
    _cfg = loadGpioConfig();
    _loadSignals();

    if (_cc1101Spi) { _cc1101Spi->end(); delete _cc1101Spi; _cc1101Spi = nullptr; }

    _cc1101Spi = (_cfg.spiBus == 1)
        ? new SPIClass(HSPI)
        : new SPIClass(VSPI);
    _cc1101Spi->begin(_cfg.sck, _cfg.miso, _cfg.mosi, _cfg.cs);
    _cc1101Spi->setFrequency(4000000);
    _cc1101Spi->setDataMode(SPI_MODE0);

    pinMode(_cfg.cs,   OUTPUT); digitalWrite(_cfg.cs,   HIGH);
    pinMode(_cfg.gdo0, INPUT);
    if (_cfg.gdo2 != 0) pinMode(_cfg.gdo2, INPUT);

    _hwConnected = _detectCC1101();

    if (_hwConnected) {
        // Basic ASK/OOK config for 433.92 MHz
        _setFrequency(433.92f);
        _spiWrite(CC1101_PKTCTRL0, 0x00); // infinite packet length (raw)
        _spiWrite(CC1101_MDMCFG4,  0x87); // data rate ~4.8kBaud
        _spiWrite(CC1101_MDMCFG2,  0x30); // ASK/OOK modulation
        _spiWrite(CC1101_IOCFG0,   0x0D); // GDO0 = carrier sense
        _setModeIdle();
        Serial.printf(SUBGHZ_TAG " CC1101 connected — GDO0=%u CS=%u\n",
                      _cfg.gdo0, _cfg.cs);
    } else {
        Serial.println(SUBGHZ_TAG " CC1101 not detected");
    }
}

void SubGhzModule::reinit(const SubGhzGpioConfig& cfg) {
    saveGpioConfig(cfg);
    _cfg = cfg;
    begin();
}

// ─────────────────────────────────────────────────────────────
void SubGhzModule::_setFrequency(float mhz) {
    _freqMhz = mhz;
    uint32_t freq = (uint32_t)((mhz * 1000000.0f / 26000000.0f) * 65536.0f);
    _spiWrite(CC1101_FREQ2, (freq >> 16) & 0xFF);
    _spiWrite(CC1101_FREQ1, (freq >>  8) & 0xFF);
    _spiWrite(CC1101_FREQ0,  freq        & 0xFF);
}

void SubGhzModule::_setModeTx()   { _spiCommand(CC1101_STX); }
void SubGhzModule::_setModeRx()   { _spiCommand(CC1101_SRX); }
void SubGhzModule::_setModeIdle() { _spiCommand(CC1101_SIDLE); }

// ─────────────────────────────────────────────────────────────
void SubGhzModule::loop() {
    if (!_hwConnected) return;
    if (_capturing) _captureLoop();
}

void SubGhzModule::_captureLoop() {
    bool gdo0 = digitalRead(_cfg.gdo0);
    if (gdo0 != _lastGdo0) {
        unsigned long now = micros();
        uint16_t dur = (uint16_t)min((unsigned long)65535UL, now - _lastEdge);
        _captureTimings.push_back(dur);
        _lastGdo0 = gdo0;
        _lastEdge = now;
        if (_captureTimings.size() > 2000) stopCapture();
    }
    // Auto-stop after 2s of silence
    if (_captureTimings.size() > 10 &&
        (micros() - _lastEdge) > 2000000UL) {
        stopCapture();
    }
}

bool SubGhzModule::startCapture(float freqMhz) {
    if (!_hwConnected) return false;
    _setFrequency(freqMhz);
    _captureTimings.clear();
    _capBuffer      = "";
    _capturing      = true;
    _captureStart   = millis();
    _lastEdge       = micros();
    _lastGdo0       = digitalRead(_cfg.gdo0);
    _setModeRx();
    Serial.printf(SUBGHZ_TAG " Capture started @ %.2f MHz\n", freqMhz);
    return true;
}

void SubGhzModule::stopCapture() {
    _capturing = false;
    _setModeIdle();
    if (!_captureTimings.empty()) {
        _capBuffer = _rawToHex(_captureTimings);
        Serial.printf(SUBGHZ_TAG " Capture done: %u timings\n",
                      (unsigned)_captureTimings.size());
    }
    _captureTimings.clear();
}

String SubGhzModule::pollCaptured() {
    String r = _capBuffer;
    _capBuffer = "";
    return r;
}

String SubGhzModule::_rawToHex(const std::vector<uint16_t>& t) const {
    String s;
    s.reserve(t.size() * 5);
    for (size_t i = 0; i < t.size(); i++) {
        if (i) s += ',';
        s += String(t[i]);
    }
    return s;
}

// ─────────────────────────────────────────────────────────────
bool SubGhzModule::replaySignal(uint32_t id) {
    if (!_hwConnected) return false;
    SubGhzSignal sig;
    if (!getSignal(id, sig)) return false;
    _setFrequency(sig.freqMhz);
    _setModeTx();

    // Parse timings from rawData
    std::vector<uint16_t> timings;
    String raw = sig.rawData;
    int pos = 0;
    while (pos < (int)raw.length()) {
        int comma = raw.indexOf(',', pos);
        String tok = (comma < 0) ? raw.substring(pos) : raw.substring(pos, comma);
        timings.push_back((uint16_t)tok.toInt());
        if (comma < 0) break;
        pos = comma + 1;
    }

    // Bit-bang OOK on GDO0 (TX path via CC1101 modulation)
    bool high = true;
    for (uint16_t t : timings) {
        // CC1101 in ASK: set power on/off via PATABLE
        _spiWrite(0x3E, high ? 0xC0 : 0x00); // PATABLE[0]
        delayMicroseconds(t);
        high = !high;
    }
    _setModeIdle();
    Serial.printf(SUBGHZ_TAG " Replayed signal #%u\n", id);
    return true;
}

// ─────────────────────────────────────────────────────────────
String SubGhzModule::statusJson() const {
    char buf[120];
    snprintf(buf, sizeof(buf),
             "{\"connected\":%s,\"frequency\":%.2f,\"mode\":\"%s\"}",
             _hwConnected?"true":"false",
             _freqMhz,
             _capturing?"RX":"IDLE");
    return String(buf);
}

// ─────────────────────────────────────────────────────────────
uint32_t SubGhzModule::saveSignal(SubGhzSignal& sig) {
    sig.id = _nextId++;
    _signals.push_back(sig);
    _saveSignals();
    return sig.id;
}

bool SubGhzModule::deleteSignal(uint32_t id) {
    for (auto it = _signals.begin(); it != _signals.end(); ++it) {
        if (it->id == id) { _signals.erase(it); _saveSignals(); return true; }
    }
    return false;
}

bool SubGhzModule::renameSignal(uint32_t id, const String& name) {
    for (auto& s : _signals) {
        if (s.id == id) { s.name = name; _saveSignals(); return true; }
    }
    return false;
}

bool SubGhzModule::getSignal(uint32_t id, SubGhzSignal& out) const {
    for (const auto& s : _signals) {
        if (s.id == id) { out = s; return true; }
    }
    return false;
}

String SubGhzModule::signalsToJson() const {
    String out = "{\"signals\":[";
    for (size_t i = 0; i < _signals.size(); i++) {
        if (i) out += ',';
        const auto& s = _signals[i];
        out += "{\"id\":" + String(s.id)
             + ",\"name\":\"" + s.name
             + "\",\"freqMhz\":" + String(s.freqMhz, 2)
             + ",\"protocol\":\"" + s.protocol
             + "\",\"captured\":\"" + s.captured + "\"}";
    }
    out += "]}";
    return out;
}

// ─────────────────────────────────────────────────────────────
void SubGhzModule::_loadSignals() {
    _signals.clear(); _nextId = 1;
    if (!LittleFS.exists(SUBGHZ_SAVE_FILE)) return;
    File f = LittleFS.open(SUBGHZ_SAVE_FILE, "r");
    if (!f) return;
    JsonDocument doc;
    if (deserializeJson(doc, f) == DeserializationError::Ok) {
        for (JsonObject o : doc["signals"].as<JsonArray>()) {
            SubGhzSignal s;
            s.id       = o["id"]       | (uint32_t)0;
            s.name     = o["name"]     | "";
            s.freqMhz  = o["freqMhz"]  | 433.92f;
            s.protocol = o["protocol"] | "";
            s.rawData  = o["rawData"]  | "";
            s.captured = o["captured"] | "";
            _signals.push_back(s);
            if (s.id >= _nextId) _nextId = s.id + 1;
        }
    }
    f.close();
}

void SubGhzModule::_saveSignals() const {
    File f = LittleFS.open(SUBGHZ_SAVE_FILE, "w");
    if (!f) return;
    f.print("{\"signals\":[");
    bool first = true;
    for (const auto& s : _signals) {
        if (!first) f.print(',');
        first = false;
        JsonDocument doc;
        doc["id"]       = s.id;
        doc["name"]     = s.name;
        doc["freqMhz"]  = s.freqMhz;
        doc["protocol"] = s.protocol;
        doc["rawData"]  = s.rawData;
        doc["captured"] = s.captured;
        serializeJson(doc, f);
    }
    f.print("]}");
    f.close();
}

void SubGhzModule::saveGpioConfig(const SubGhzGpioConfig& cfg) {
    File f = LittleFS.open(SUBGHZ_GPIO_FILE, "w");
    if (!f) return;
    JsonDocument doc;
    doc["gdo0"] = cfg.gdo0; doc["gdo2"] = cfg.gdo2;
    doc["cs"]   = cfg.cs;   doc["sck"]  = cfg.sck;
    doc["mosi"] = cfg.mosi; doc["miso"] = cfg.miso;
    doc["spiBus"] = cfg.spiBus;
    serializeJson(doc, f);
    f.close();
}

SubGhzGpioConfig SubGhzModule::loadGpioConfig() const {
    SubGhzGpioConfig cfg;
    if (!LittleFS.exists(SUBGHZ_GPIO_FILE)) return cfg;
    File f = LittleFS.open(SUBGHZ_GPIO_FILE, "r");
    if (!f) return cfg;
    JsonDocument doc;
    if (deserializeJson(doc, f) == DeserializationError::Ok) {
        cfg.gdo0   = doc["gdo0"]   | (uint8_t)34;
        cfg.gdo2   = doc["gdo2"]   | (uint8_t)35;
        cfg.cs     = doc["cs"]     | (uint8_t)32;
        cfg.sck    = doc["sck"]    | (uint8_t)18;
        cfg.mosi   = doc["mosi"]   | (uint8_t)23;
        cfg.miso   = doc["miso"]   | (uint8_t)19;
        cfg.spiBus = doc["spiBus"] | (uint8_t)0;
    }
    f.close();
    return cfg;
}
