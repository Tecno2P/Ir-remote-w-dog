#pragma once
// ============================================================
//  scheduler.h  –  Cron-style scheduled IR transmission
//
//  v2.0.0:
//    + Up to MAX_SCHEDULES entries
//    + Per-entry: days-of-week mask, hour, minute, buttonId
//    + Enable/disable toggle per entry
//    + NTP time sync (pool.ntp.org)
//    + Fires IR transmission at matching minute boundary
//    + Persisted in /schedules.json
//
//  v2.2.0 (Upgrade 3):
//    + repeatCount — how many times to fire IR on schedule trigger
//    + repeatDelay — ms between repeat fires (0 = use button default)
// ============================================================
#include <Arduino.h>
#include <vector>
#include <ArduinoJson.h>
#include "config.h"

// Days-of-week bitmask constants (bit 0 = Sunday)
#define SCHED_SUN  (1<<0)
#define SCHED_MON  (1<<1)
#define SCHED_TUE  (1<<2)
#define SCHED_WED  (1<<3)
#define SCHED_THU  (1<<4)
#define SCHED_FRI  (1<<5)
#define SCHED_SAT  (1<<6)
#define SCHED_DAILY   0x7F  // all days
#define SCHED_WEEKDAY 0x3E  // Mon-Fri
#define SCHED_WEEKEND 0x41  // Sat+Sun

struct ScheduleEntry {
    uint32_t id;
    String   name;          // Human-readable label
    uint32_t buttonId;      // IR button to fire
    uint8_t  hour;          // 0-23
    uint8_t  minute;        // 0-59
    uint8_t  daysMask;      // bitmask, see SCHED_* constants above
    bool     enabled;
    // v2.2.0
    uint8_t  repeatCount;   // how many times to fire (1 = single fire)
    uint16_t repeatDelay;   // ms between fires (0 = use button's own repeatDelay)

    ScheduleEntry()
        : id(0), buttonId(0), hour(22), minute(0),
          daysMask(SCHED_DAILY), enabled(true),
          repeatCount(1), repeatDelay(0) {}

    void toJson(JsonObject obj) const {
        obj["id"]          = id;
        obj["name"]        = name;
        obj["buttonId"]    = buttonId;
        obj["hour"]        = hour;
        obj["minute"]      = minute;
        obj["daysMask"]    = daysMask;
        obj["enabled"]     = enabled;
        obj["repeatCount"] = repeatCount;
        obj["repeatDelay"] = repeatDelay;
    }
    bool fromJson(JsonObjectConst obj) {
        id          = obj["id"]          | (uint32_t)0;
        name        = obj["name"]        | (const char*)"";
        buttonId    = obj["buttonId"]    | (uint32_t)0;
        hour        = obj["hour"]        | (uint8_t)22;
        minute      = obj["minute"]      | (uint8_t)0;
        daysMask    = obj["daysMask"]    | (uint8_t)SCHED_DAILY;
        enabled     = obj["enabled"]     | true;
        repeatCount = obj["repeatCount"] | (uint8_t)1;
        repeatDelay = obj["repeatDelay"] | (uint16_t)0;
        if (hour > 23)     hour = 0;
        if (minute > 59)   minute = 0;
        if (repeatCount < 1) repeatCount = 1;
        if (repeatCount > IR_MAX_REPEAT_COUNT) repeatCount = IR_MAX_REPEAT_COUNT;
        if (repeatDelay > IR_MAX_REPEAT_DELAY) repeatDelay = IR_MAX_REPEAT_DELAY;
        return buttonId > 0;
    }
};

class Scheduler {
public:
    Scheduler();
    void begin();                      // call after WiFi up; starts NTP
    void loop();                       // call from main loop()

    // NTP
    bool   ntpSynced()        const;
    String currentTimeStr()   const;   // "HH:MM:SS"
    String currentDateStr()   const;   // "YYYY-MM-DD"
    long   tzOffsetSec()      const { return _tzOffset; }
    long   dstOffsetSec()     const { return _dstOffset; }
    void   setTimezone(long tzSec, long dstSec);

    // CRUD
    uint32_t   addEntry(const ScheduleEntry& e);
    bool       updateEntry(const ScheduleEntry& e);
    bool       removeEntry(uint32_t id);
    bool       setEnabled(uint32_t id, bool en);
    size_t     size() const { return _entries.size(); }

    // Serialisation
    String     toJson() const;
    bool       loadFromFile();
    bool       saveToFile();

    const std::vector<ScheduleEntry>& entries() const { return _entries; }

    // Callback — set before begin()
    // v2.2.0: callback now receives full ScheduleEntry so caller can
    // honour repeatCount / repeatDelay at the schedule level.
    using FireCallback = std::function<void(const ScheduleEntry&)>;
    void onFire(FireCallback cb) { _fireCb = cb; }

    // Persist / restore timezone to /ntp_config.json
    bool saveTimezone();
    bool loadTimezone();

private:
    std::vector<ScheduleEntry> _entries;
    uint32_t   _nextId;
    long       _tzOffset;
    long       _dstOffset;
    bool       _ntpStarted;
    unsigned long _lastCheck;     // millis() of last scheduler tick
    unsigned long _lastNtpSync;   // millis() of last NTP sync
    int        _lastFiredMinute;  // tracks which minute we last fired
    FireCallback _fireCb;

    void _checkAndFire();
    void _startNtp();
};

extern Scheduler scheduler;
