#pragma once
// ============================================================
//  watchdog_manager.h  –  Ultra Pro Watchdog v3.0
//  All values are real ESP32 system API readings — zero fake data.
// ============================================================
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <esp_idf_version.h>
#include <vector>

#define WDT_CFG_FILE              "/wdt_config.json"
#define WDT_CRASH_FILE            "/crash_log.json"
#define WDT_BOOT_CTR_FILE         "/wdt_boot_ctr.json"
#define WDT_TAG                   "[WDT]"

#define WDT_HW_TIMEOUT_S          30
#define WDT_LOOP_MAX_MS           8000UL
#define WDT_LOOP_REBOOT_MS        30000UL
#define WDT_HEAP_MIN_BYTES        20000UL
#define WDT_HEAP_CRITICAL_BYTES   12000UL
#define WDT_MAX_CRASHES           20
#define WDT_PING_INTERVAL_MS      60000UL
#define WDT_HEAP_CHECK_MS         30000UL
#define WDT_TEMP_CHECK_MS         20000UL
#define WDT_TEMP_THROTTLE_C       80.0f
#define WDT_TEMP_REBOOT_C         95.0f
#define WDT_MAX_BOOT_FAILURES     5
#define WDT_PING_URL              "http://connectivitycheck.gstatic.com/generate_204"
#define WDT_PING_TIMEOUT_MS       5000

enum class WdtPerfMode { NORMAL, POWER_SAVE, TURBO };

struct WdtModuleHealth {
    String        name;
    unsigned long lastFeedMs;
    uint32_t      timeoutMs;
    bool          stalled;
    uint32_t      stallCount;
    bool isStalled() const {
        if (timeoutMs == 0) return false;
        return (millis() - lastFeedMs) > timeoutMs;
    }
};

struct WdtCrashEntry {
    uint32_t id;
    uint32_t timestamp;
    String   timeStr;
    String   reason;
    uint32_t heapAtCrash;
    uint32_t uptimeSec;
    float    tempAtCrash;
};

struct WdtBootState {
    uint8_t  failCount;
    bool     safeMode;
    uint32_t lastCleanBoot;
};

class WatchdogManager {
public:
    WatchdogManager();

    void    begin();
    void    loop();

    void    hwFeed();
    bool    hwEnabled()            const { return _hwEnabled; }

    uint8_t registerModule(const String& name, uint32_t timeoutMs = 30000);
    void    feedModule(uint8_t idx);
    bool    isModuleStalled(uint8_t idx) const;

    bool     isHeapCritical()      const { return ESP.getFreeHeap() < WDT_HEAP_CRITICAL_BYTES; }
    bool     isHeapLow()           const { return ESP.getFreeHeap() < _heapThreshold; }
    uint32_t minHeapSeen()         const { return _minHeapSeen; }
    void     tryMemoryCleanup();

    bool     internetReachable()   const { return _internetOk; }
    float    cpuTemperature()      const;
    bool     isOverheating()       const { return cpuTemperature() > WDT_TEMP_THROTTLE_C; }

    bool     isSafeMode()          const { return _bootState.safeMode; }
    uint8_t  bootFailCount()       const { return _bootState.failCount; }
    void     markBootSuccess();

    void         setPerfMode(WdtPerfMode mode);
    WdtPerfMode  getPerfMode()     const { return _perfMode; }
    const char*  perfModeStr()     const;

    String  statusJson()           const;
    String  crashLogJson()         const;

    void    setHwEnabled(bool en);
    void    setHeapThreshold(uint32_t bytes);
    bool    loadConfig();
    bool    saveConfig();

private:
    bool          _hwEnabled;
    bool          _hwStarted;
    bool          _beginDone;
    uint32_t      _heapThreshold;
    uint32_t      _minHeapSeen;
    unsigned long _lastLoopMs;
    unsigned long _lastHeapCheck;
    unsigned long _lastPingCheck;
    unsigned long _lastTempCheck;
    bool          _internetOk;
    bool          _wifiWasConnected;
    WdtBootState  _bootState;
    WdtPerfMode   _perfMode;
    uint8_t       _heapCriticalCount;

    std::vector<WdtModuleHealth> _modules;

    void   _startHw();
    void   _logCrashOnBoot();
    void   _saveCrash(const WdtCrashEntry& entry);
    void   _checkConnectivity();
    void   _checkTemperature();
    void   _checkHeap(uint32_t freeHeap, unsigned long now);
    void   _checkModules(unsigned long now);
    void   _checkLoopStall(unsigned long now);
    void   _loadBootState();
    void   _saveBootState();
    void   _applyCpuFreq(uint32_t mhz);
    String _resetReasonStr(esp_reset_reason_t reason) const;
    String _buildTimeStr() const;
};

extern WatchdogManager wdtMgr;
