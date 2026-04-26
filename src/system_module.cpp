// ============================================================
//  system_module.cpp  –  GPS + LED Real Implementation
// ============================================================
#include "system_module.h"
#include <WiFi.h>

SystemModule sysModule;

// ─────────────────────────────────────────────────────────────
void SystemModule::begin() {
    _loadConfig();
    _loadScheduleTasks();
    _ledCfg  = loadLedConfig();
    _gpsCfg  = loadGpsGpio();
    _initGps();
    _initFastLED();
    Serial.println("[SYS] System module initialized");
}

void SystemModule::loop() {
    // LED tick every 20ms
    if (millis() - _ledTimer > 20) {
        _ledTimer = millis();
        ledTick();
    }
    // GPS feed every 1ms
    if (_gpsSerial && (millis() - _gpsTimer > 1)) {
        _gpsTimer = millis();
        while (_gpsSerial->available()) {
            char c = (char)_gpsSerial->read();
            _gps.encode(c);
        }
        if (_gps.location.isValid()) {
            _gpsInfo.lat   = _gps.location.lat();
            _gpsInfo.lon   = _gps.location.lng();
            _gpsInfo.valid = true;
            _gpsInfo.fix   = "GPS Fix";
        }
        if (_gps.speed.isValid())
            _gpsInfo.speed = (float)_gps.speed.kmph();
        if (_gps.satellites.isValid())
            _gpsInfo.sats  = _gps.satellites.value();
        if (_gps.altitude.isValid())
            _gpsInfo.alt   = (float)_gps.altitude.meters();

        // Detect GPS by chars processed
        if (!_gpsConnected && _gps.charsProcessed() > 10)
            _gpsConnected = true;
    }
}

// ─────────────────────────────────────────────────────────────
void SystemModule::_initGps() {
    if (_gpsSerial) { _gpsSerial->end(); _gpsSerial = nullptr; }
    if (_gpsCfg.rxPin == 0) return;

    switch (_gpsCfg.uartNum) {
        case 2:  _gpsSerial = &Serial2; break;
        default: _gpsSerial = &Serial1; break;
    }
    _gpsSerial->begin(_gpsCfg.baud, SERIAL_8N1,
                       _gpsCfg.rxPin, _gpsCfg.txPin);
    _gpsConnected = false;
    Serial.printf("[SYS] GPS UART%u RX=%u TX=%u @%u\n",
                  _gpsCfg.uartNum, _gpsCfg.rxPin,
                  _gpsCfg.txPin, _gpsCfg.baud);
}

void SystemModule::reinitGps(const GpsGpioConfig& cfg) {
    saveGpsGpio(cfg);
    _gpsCfg = cfg;
    _initGps();
}

// ─────────────────────────────────────────────────────────────
void SystemModule::_initFastLED() {
    if (_ledCfg.numLeds == 0) return;
    uint8_t n = min((uint8_t)MAX_LEDS, _ledCfg.numLeds);

    // FastLED requires compile-time DATA_PIN.
    // We use GPIO 2 as the compiled-in default.
    // Users can wire their LED strip to GPIO 2 (default) or change
    // via the LED config page (requires reflash for different pin).
    FastLED.clearData();
    FastLED.addLeds<WS2812B, 2, GRB>(_leds, n);
    FastLED.setBrightness(_ledCfg.brightness);
    FastLED.clear(true);
    _fastledInited = true;
    Serial.printf("[SYS] FastLED: %u LEDs GPIO2 brightness=%u\n",
                  n, _ledCfg.brightness);
}

void SystemModule::ledTick() {
    if (!_fastledInited) return;
    uint8_t n = min((uint8_t)MAX_LEDS, _ledCfg.numLeds);

    switch (_ledCfg.mode) {
        case LedMode::SOLID:
            for (int i = 0; i < n; i++)
                _leds[i] = CRGB(_ledCfg.r, _ledCfg.g, _ledCfg.b);
            FastLED.show();
            break;

        case LedMode::RAINBOW:
            for (int i = 0; i < n; i++)
                _leds[i] = CHSV(_ledHue + (i * 255 / n), 255, 200);
            _ledHue++;
            FastLED.show();
            break;

        case LedMode::RAVE:
            for (int i = 0; i < n; i++)
                _leds[i] = CHSV(random8(), 255, 200);
            FastLED.show();
            break;

        case LedMode::BLINK:
            _ledBlink = (_ledBlink + 1) & 0x1F;
            for (int i = 0; i < n; i++)
                _leds[i] = (_ledBlink < 16)
                    ? CRGB(_ledCfg.r, _ledCfg.g, _ledCfg.b)
                    : CRGB::Black;
            FastLED.show();
            break;

        case LedMode::PULSE: {
            uint8_t bright = (uint8_t)(128 + 127 * sin(_ledHue * 0.05f));
            for (int i = 0; i < n; i++)
                _leds[i] = CRGB(_ledCfg.r, _ledCfg.g, _ledCfg.b).nscale8(bright);
            _ledHue++;
            FastLED.show();
            break;
        }

        case LedMode::OFF:
        default:
            FastLED.clear(); FastLED.show();
            break;
    }
}

// ─────────────────────────────────────────────────────────────
void SystemModule::setLedMode(const LedConfig& cfg) {
    _ledCfg = cfg;
    saveLedConfig(cfg);
    if (!_fastledInited) _initFastLED();
    FastLED.setBrightness(cfg.brightness);
    _applyLed();
}

void SystemModule::_applyLed() {
    if (_ledCfg.mode == LedMode::OFF) {
        FastLED.clear(); FastLED.show();
    }
}

void SystemModule::saveLedConfig(const LedConfig& cfg) {
    File f = LittleFS.open(LED_CFG_FILE, "w");
    if (!f) return;
    JsonDocument doc;
    doc["type"]       = (int)cfg.type;
    doc["mode"]       = (int)cfg.mode;
    doc["dataPin"]    = cfg.dataPin;
    doc["numLeds"]    = cfg.numLeds;
    doc["r"]          = cfg.r;
    doc["g"]          = cfg.g;
    doc["b"]          = cfg.b;
    doc["brightness"] = cfg.brightness;
    serializeJson(doc, f);
    f.close();
}

LedConfig SystemModule::loadLedConfig() const {
    LedConfig cfg;
    if (!LittleFS.exists(LED_CFG_FILE)) return cfg;
    File f = LittleFS.open(LED_CFG_FILE, "r");
    if (!f) return cfg;
    JsonDocument doc;
    if (deserializeJson(doc, f) == DeserializationError::Ok) {
        cfg.type       = (LedType)(doc["type"]       | 0);
        cfg.mode       = (LedMode)(doc["mode"]       | 0);
        cfg.dataPin    = doc["dataPin"]    | (uint8_t)2;
        cfg.numLeds    = doc["numLeds"]    | (uint8_t)8;
        cfg.r          = doc["r"]          | (uint8_t)255;
        cfg.g          = doc["g"]          | (uint8_t)0;
        cfg.b          = doc["b"]          | (uint8_t)128;
        cfg.brightness = doc["brightness"] | (uint8_t)128;
    }
    f.close();
    return cfg;
}

// ─────────────────────────────────────────────────────────────
void SystemModule::saveGpsGpio(const GpsGpioConfig& cfg) {
    File f = LittleFS.open(GPS_GPIO_FILE, "w");
    if (!f) return;
    JsonDocument doc;
    doc["rxPin"]   = cfg.rxPin;
    doc["txPin"]   = cfg.txPin;
    doc["baud"]    = cfg.baud;
    doc["uartNum"] = cfg.uartNum;
    serializeJson(doc, f);
    f.close();
}

GpsGpioConfig SystemModule::loadGpsGpio() const {
    GpsGpioConfig cfg;
    if (!LittleFS.exists(GPS_GPIO_FILE)) return cfg;
    File f = LittleFS.open(GPS_GPIO_FILE, "r");
    if (!f) return cfg;
    JsonDocument doc;
    if (deserializeJson(doc, f) == DeserializationError::Ok) {
        cfg.rxPin   = doc["rxPin"]   | (uint8_t)16;
        cfg.txPin   = doc["txPin"]   | (uint8_t)17;
        cfg.baud    = doc["baud"]    | (uint32_t)9600;
        cfg.uartNum = doc["uartNum"] | (uint8_t)1;
    }
    f.close();
    return cfg;
}

// ─────────────────────────────────────────────────────────────
String SystemModule::getStatusJson() const {
    JsonDocument doc;
    doc["ok"]         = true;
    doc["heap"]       = ESP.getFreeHeap();
    doc["uptime"]     = millis() / 1000;
    doc["cpuMhz"]     = ESP.getCpuFreqMHz();
    doc["cpuFreq"]    = ESP.getCpuFreqMHz();
    doc["chip"]       = ESP.getChipModel();
    doc["chipModel"]  = ESP.getChipModel();
    doc["firmware"]   = FIRMWARE_VERSION;
    doc["flashSize"]  = (uint32_t)(ESP.getFlashChipSize() / 1024);
    // MAC address
    uint8_t macBytes[6]; WiFi.macAddress(macBytes);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             macBytes[0], macBytes[1], macBytes[2],
             macBytes[3], macBytes[4], macBytes[5]);
    doc["mac"] = String(macStr);
    // GPS
    JsonObject gps = doc["gps"].to<JsonObject>();
    gps["connected"] = _gpsConnected;
    gps["lat"]   = _gpsInfo.lat;
    gps["lon"]   = _gpsInfo.lon;
    gps["speed"] = _gpsInfo.speed;
    gps["sats"]  = _gpsInfo.sats;
    gps["alt"]   = _gpsInfo.alt;
    gps["fix"]   = _gpsInfo.fix;
    gps["valid"] = _gpsInfo.valid;
    // LED
    JsonObject led = doc["led"].to<JsonObject>();
    led["mode"]   = (int)_ledCfg.mode;
    led["active"] = isLedActive();
    led["numLeds"]= _ledCfg.numLeds;
    String out; serializeJson(doc, out);
    return out;
}

String SystemModule::hardwareStatusJson() const {
    // Called by web_server to aggregate hardware detection
    char buf[80];
    snprintf(buf, sizeof(buf),
             "{\"gps\":%s,\"led\":%s}",
             _gpsConnected?"true":"false",
             _fastledInited?"true":"false");
    return String(buf);
}

// ─────────────────────────────────────────────────────────────
void SystemModule::setGhostLink(bool en) {
    _ghostLink = en; _saveConfig();
}
void SystemModule::setTimezone(const String& tz) {
    _timezone = tz; _saveConfig();
}

void SystemModule::_loadConfig() {
    if (!LittleFS.exists(SYS_CFG_FILE)) return;
    File f = LittleFS.open(SYS_CFG_FILE, "r");
    if (!f) return;
    JsonDocument doc;
    if (deserializeJson(doc, f) == DeserializationError::Ok) {
        _ghostLink = doc["ghostLink"] | false;
        _timezone  = doc["timezone"]  | (const char*)"IST";
    }
    f.close();
}

void SystemModule::_saveConfig() const {
    File f = LittleFS.open(SYS_CFG_FILE, "w");
    if (!f) return;
    JsonDocument doc;
    doc["ghostLink"] = _ghostLink;
    doc["timezone"]  = _timezone;
    serializeJson(doc, f);
    f.close();
}

// ─────────────────────────────────────────────────────────────
uint32_t SystemModule::addScheduleTask(SysScheduleTask& task) {
    static uint32_t nextId = 1;
    task.id = nextId++;
    _schedTasks.push_back(task);
    _saveScheduleTasks();
    return task.id;
}

bool SystemModule::deleteScheduleTask(uint32_t id) {
    for (auto it = _schedTasks.begin(); it != _schedTasks.end(); ++it) {
        if (it->id == id) { _schedTasks.erase(it); _saveScheduleTasks(); return true; }
    }
    return false;
}

bool SystemModule::toggleScheduleTask(uint32_t id, bool en) {
    for (auto& t : _schedTasks) {
        if (t.id == id) { t.enabled = en; _saveScheduleTasks(); return true; }
    }
    return false;
}

String SystemModule::scheduleTasksToJson() const {
    String out = "{\"tasks\":[";
    for (size_t i = 0; i < _schedTasks.size(); i++) {
        if (i) out += ',';
        const auto& t = _schedTasks[i];
        out += "{\"id\":" + String(t.id)
             + ",\"name\":\"" + t.name
             + "\",\"time\":\"" + t.time
             + "\",\"action\":\"" + t.action
             + "\",\"enabled\":" + (t.enabled?"true":"false") + "}";
    }
    out += "]}";
    return out;
}

void SystemModule::_loadScheduleTasks() {
    _schedTasks.clear();
    if (!LittleFS.exists(SYS_SCHED_FILE)) return;
    File f = LittleFS.open(SYS_SCHED_FILE, "r");
    if (!f) return;
    JsonDocument doc;
    if (deserializeJson(doc, f) == DeserializationError::Ok) {
        for (JsonObject o : doc["tasks"].as<JsonArray>()) {
            SysScheduleTask t;
            t.id      = o["id"]      | (uint32_t)0;
            t.name    = o["name"]    | "";
            t.time    = o["time"]    | "";
            t.action  = o["action"]  | "";
            t.enabled = o["enabled"] | true;
            _schedTasks.push_back(t);
        }
    }
    f.close();
}

void SystemModule::_saveScheduleTasks() const {
    File f = LittleFS.open(SYS_SCHED_FILE, "w");
    if (!f) return;
    f.print("{\"tasks\":[");
    bool first = true;
    for (const auto& t : _schedTasks) {
        if (!first) f.print(',');
        first = false;
        f.printf("{\"id\":%u,\"name\":\"%s\",\"time\":\"%s\","
                 "\"action\":\"%s\",\"enabled\":%s}",
                 t.id, t.name.c_str(), t.time.c_str(),
                 t.action.c_str(), t.enabled?"true":"false");
    }
    f.print("]}");
    f.close();
}

String SystemModule::gpioOverviewJson() const {
    // Return used pins for GPIO conflict detection
    JsonDocument doc;
    JsonArray pins = doc["used"].to<JsonArray>();
    // LED pin
    if (_ledCfg.mode != LedMode::OFF) {
        JsonObject o = pins.add<JsonObject>();
        o["pin"] = _ledCfg.dataPin; o["module"] = "LED";
    }
    // GPS pins
    if (_gpsConnected) {
        JsonObject o1 = pins.add<JsonObject>();
        o1["pin"] = _gpsCfg.rxPin; o1["module"] = "GPS-RX";
        JsonObject o2 = pins.add<JsonObject>();
        o2["pin"] = _gpsCfg.txPin; o2["module"] = "GPS-TX";
    }
    String out; serializeJson(doc, out);
    return out;
}
