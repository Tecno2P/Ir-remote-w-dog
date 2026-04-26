// ============================================================
//  scheduler.cpp  –  NTP + cron-style IR scheduler
// ============================================================
#include "scheduler.h"
#include <WiFi.h>
#include <LittleFS.h>
#include <ctime>

Scheduler scheduler;

Scheduler::Scheduler()
    : _nextId(1), _tzOffset(NTP_TIMEZONE_OFFSET), _dstOffset(NTP_DST_OFFSET),
      _ntpStarted(false), _lastCheck(0), _lastNtpSync(0),
      _lastFiredMinute(-1)
{}

// ── NTP ───────────────────────────────────────────────────────
void Scheduler::_startNtp() {
    configTime(_tzOffset, _dstOffset, NTP_SERVER1, NTP_SERVER2);
    Serial.printf(DEBUG_TAG " NTP sync started (tz=%ld dst=%ld)\n",
                  _tzOffset, _dstOffset);
    _ntpStarted   = true;
    _lastNtpSync  = millis();
}

void Scheduler::setTimezone(long tzSec, long dstSec) {
    _tzOffset  = tzSec;
    _dstOffset = dstSec;
    if (_ntpStarted) {
        configTime(_tzOffset, _dstOffset, NTP_SERVER1, NTP_SERVER2);
        Serial.printf(DEBUG_TAG " Timezone updated tz=%ld dst=%ld\n",
                      _tzOffset, _dstOffset);
    }
    saveTimezone();  // persist so it survives reboot
}

bool Scheduler::saveTimezone() {
    File f = LittleFS.open("/ntp_config.json", "w");
    if (!f) { Serial.println(DEBUG_TAG " ERROR: Cannot write ntp_config.json"); return false; }
    JsonDocument doc;
    doc["tzOffset"]  = _tzOffset;
    doc["dstOffset"] = _dstOffset;
    size_t w = serializeJson(doc, f);
    f.close();
    return w > 0;
}

bool Scheduler::loadTimezone() {
    if (!LittleFS.exists("/ntp_config.json")) return false;
    File f = LittleFS.open("/ntp_config.json", "r");
    if (!f) return false;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err != DeserializationError::Ok) return false;
    _tzOffset  = doc["tzOffset"]  | (long)NTP_TIMEZONE_OFFSET;
    _dstOffset = doc["dstOffset"] | (long)NTP_DST_OFFSET;
    Serial.printf(DEBUG_TAG " Timezone loaded tz=%ld dst=%ld\n", _tzOffset, _dstOffset);
    return true;
}

bool Scheduler::ntpSynced() const {
    time_t now = time(nullptr);
    return now > 1700000000UL;   // past Nov 2023 = definitely synced
}

String Scheduler::currentTimeStr() const {
    time_t now = time(nullptr);
    struct tm tmbuf;
    struct tm* t = localtime_r(&now, &tmbuf);
    char buf[12];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);
    return String(buf);
}

String Scheduler::currentDateStr() const {
    time_t now = time(nullptr);
    struct tm tmbuf;
    struct tm* t = localtime_r(&now, &tmbuf);
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t->tm_year+1900, t->tm_mon+1, t->tm_mday);
    return String(buf);
}

// ── Lifecycle ─────────────────────────────────────────────────
void Scheduler::begin() {
    loadFromFile();
    loadTimezone();  // restore persisted tz/dst offsets before NTP starts
    // from wifi_manager or loop() when STA comes online.
    // We still call configTime() here so the timezone is set even if NTP
    // resolves later via SNTP background polling.
    if (WiFi.status() == WL_CONNECTED) {
        _startNtp();
    } else {
        // Pre-configure timezone so SNTP daemon picks it up when WiFi connects
        configTime(_tzOffset, _dstOffset, NTP_SERVER1, NTP_SERVER2);
        Serial.println(DEBUG_TAG " NTP: scheduled for when STA connects");
    }
}

void Scheduler::loop() {
    unsigned long now = millis();

    // Start NTP as soon as STA connects (if not already started)
    if (!_ntpStarted && WiFi.status() == WL_CONNECTED) {
        _startNtp();
    }

    // Re-sync NTP periodically (only when connected)
    if (_ntpStarted && WiFi.status() == WL_CONNECTED &&
        (now - _lastNtpSync) >= NTP_SYNC_INTERVAL_MS) {
        _startNtp();
    }

    if ((now - _lastCheck) >= SCHEDULER_CHECK_INTERVAL_MS) {
        _lastCheck = now;
        if (ntpSynced()) _checkAndFire();
    }
}

void Scheduler::_checkAndFire() {
    time_t t = time(nullptr);
    struct tm tmbuf;
    struct tm* tm = localtime_r(&t, &tmbuf);
    int curMinute = tm->tm_hour * 60 + tm->tm_min;
    int dotw      = tm->tm_wday; // 0=Sun

    // Fire at most once per minute window
    if (curMinute == _lastFiredMinute) return;

    for (const auto& e : _entries) {
        if (!e.enabled) continue;
        if (e.hour != (uint8_t)tm->tm_hour) continue;
        if (e.minute != (uint8_t)tm->tm_min) continue;
        if (!(e.daysMask & (1 << dotw))) continue;

        Serial.printf(DEBUG_TAG " Scheduler FIRE: entry %u '%s' btn=%u rpt=%u dly=%u\n",
                      e.id, e.name.c_str(), e.buttonId,
                      e.repeatCount, e.repeatDelay);
        if (_fireCb) _fireCb(e);
    }

    _lastFiredMinute = curMinute;
}

// ── CRUD ──────────────────────────────────────────────────────
uint32_t Scheduler::addEntry(const ScheduleEntry& e) {
    if (_entries.size() >= MAX_SCHEDULES) return 0;
    ScheduleEntry copy = e;
    copy.id = _nextId++;
    _entries.push_back(copy);
    saveToFile();
    return copy.id;
}

bool Scheduler::updateEntry(const ScheduleEntry& e) {
    for (auto& x : _entries) {
        if (x.id == e.id) { x = e; saveToFile(); return true; }
    }
    return false;
}

bool Scheduler::removeEntry(uint32_t id) {
    for (auto it = _entries.begin(); it != _entries.end(); ++it) {
        if (it->id == id) { _entries.erase(it); saveToFile(); return true; }
    }
    return false;
}

bool Scheduler::setEnabled(uint32_t id, bool en) {
    for (auto& e : _entries) {
        if (e.id == id) { e.enabled = en; saveToFile(); return true; }
    }
    return false;
}

// ── Serialisation ─────────────────────────────────────────────
String Scheduler::toJson() const {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& e : _entries) {
        JsonObject o = arr.add<JsonObject>();
        e.toJson(o);
    }
    String out; serializeJson(doc, out);
    return out;
}

bool Scheduler::loadFromFile() {
    if (!LittleFS.exists(SCHEDULES_FILE)) return false;
    File f = LittleFS.open(SCHEDULES_FILE, "r");
    if (!f) return false;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err != DeserializationError::Ok || !doc.is<JsonArrayConst>()) return false;

    _entries.clear();
    _nextId = 1;
    for (JsonObjectConst o : doc.as<JsonArrayConst>()) {
        ScheduleEntry e;
        if (e.fromJson(o)) {
            _entries.push_back(e);
            if (e.id >= _nextId) _nextId = e.id + 1;
        }
    }
    Serial.printf(DEBUG_TAG " Schedules loaded: %u\n", (unsigned)_entries.size());
    return true;
}

bool Scheduler::saveToFile() {
    File f = LittleFS.open(SCHEDULES_FILE, "w");
    if (!f) { Serial.println(DEBUG_TAG " ERROR: Cannot write schedules."); return false; }
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& e : _entries) {
        JsonObject o = arr.add<JsonObject>();
        e.toJson(o);
    }
    size_t w = serializeJson(doc, f); f.close();
    return w > 0;
}
