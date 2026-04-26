#pragma once
// ============================================================
//  system_module.h  –  System Module
//  GPS (TinyGPSPlus) + LED (FastLED) + Real Implementation
// ============================================================
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <HardwareSerial.h>
#include <TinyGPSPlus.h>
#include <FastLED.h>
#include <vector>

#define SYS_CFG_FILE    "/sys_config.json"
#define SYS_SCHED_FILE  "/sys_schedules.json"
#define GPS_GPIO_FILE   "/gps_gpio.json"
#define LED_CFG_FILE    "/led_config.json"
#define MAX_LEDS        64

enum class LedMode { OFF, SOLID, RAINBOW, RAVE, BLINK, PULSE };
enum class LedType { WS2812B, WS2811, SK6812 };

struct LedConfig {
    LedType type     = LedType::WS2812B;
    LedMode mode     = LedMode::OFF;
    uint8_t dataPin  = 2;
    uint8_t numLeds  = 8;
    uint8_t r        = 255;
    uint8_t g        = 0;
    uint8_t b        = 128;
    uint8_t brightness = 128;
};

struct GpsGpioConfig {
    uint8_t  rxPin  = 16;
    uint8_t  txPin  = 17;
    uint32_t baud   = 9600;
    uint8_t  uartNum = 1;   // UART1 or UART2
};

struct GpsInfo {
    double  lat   = 0.0;
    double  lon   = 0.0;
    float   speed = 0.0f;
    uint8_t sats  = 0;
    float   alt   = 0.0f;
    String  fix   = "No Fix";
    bool    valid = false;
};

struct SysScheduleTask {
    uint32_t id      = 0;
    String   name;
    String   time;
    String   action;
    bool     enabled = true;
};

class SystemModule {
public:
    void  begin();
    void  loop();

    // Status JSON
    String getStatusJson() const;

    // GPS
    GpsInfo        getGpsInfo() const { return _gpsInfo; }
    bool           isGpsConnected() const { return _gpsConnected; }
    void           saveGpsGpio(const GpsGpioConfig& cfg);
    GpsGpioConfig  loadGpsGpio() const;
    void           reinitGps(const GpsGpioConfig& cfg);

    // LED
    void      setLedMode(const LedConfig& cfg);
    void      saveLedConfig(const LedConfig& cfg);
    LedConfig loadLedConfig() const;
    bool      isLedActive() const { return _ledCfg.mode != LedMode::OFF; }
    void      ledTick();

    // GhostLink
    void    setGhostLink(bool en);
    bool    isGhostLinkEnabled() const { return _ghostLink; }

    // Timezone
    void    setTimezone(const String& tz);
    String  getTimezone() const { return _timezone; }

    // Schedule tasks
    uint32_t addScheduleTask(SysScheduleTask& task);
    bool     deleteScheduleTask(uint32_t id);
    bool     toggleScheduleTask(uint32_t id, bool en);
    String   scheduleTasksToJson() const;

    // GPIO overview
    String  gpioOverviewJson() const;

    // Hardware status
    String hardwareStatusJson() const;

private:
    bool    _ghostLink = false;
    String  _timezone  = "IST";
    bool    _gpsConnected = false;
    GpsInfo _gpsInfo;
    LedConfig _ledCfg;
    GpsGpioConfig _gpsCfg;

    std::vector<SysScheduleTask> _schedTasks;
    unsigned long _ledTimer  = 0;
    unsigned long _gpsTimer  = 0;
    uint8_t       _ledHue    = 0;
    uint8_t       _ledBlink  = 0;

    // FastLED
    CRGB _leds[MAX_LEDS];
    bool _fastledInited = false;
    void _initFastLED();

    // GPS UART
    HardwareSerial* _gpsSerial = nullptr;
    TinyGPSPlus     _gps;
    void _initGps();

    void _loadConfig();
    void _saveConfig() const;
    void _loadScheduleTasks();
    void _saveScheduleTasks() const;
    void _applyLed();
};

extern SystemModule sysModule;
