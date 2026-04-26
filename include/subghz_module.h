#pragma once
// ============================================================
//  subghz_module.h  –  CC1101 Sub-1GHz Real Implementation
// ============================================================
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <vector>

#define SUBGHZ_SAVE_FILE   "/subghz_signals.json"
#define SUBGHZ_GPIO_FILE   "/subghz_gpio.json"

struct SubGhzSignal {
    uint32_t id       = 0;
    String   name;
    float    freqMhz  = 433.92f;
    String   protocol;
    String   rawData;
    String   captured;
};

struct SubGhzGpioConfig {
    uint8_t gdo0   = 34;
    uint8_t gdo2   = 35;
    uint8_t cs     = 32;
    // HSPI bus to avoid conflict with SD card (VSPI: SCK=18,MOSI=23,MISO=19)
    uint8_t sck    = 14;   // HSPI SCK
    uint8_t mosi   = 13;   // HSPI MOSI
    uint8_t miso   = 12;   // HSPI MISO
    uint8_t spiBus = 1;    // 1=HSPI (SD uses VSPI)
};

class SubGhzModule {
public:
    void   begin();
    void   loop();
    void   reinit(const SubGhzGpioConfig& cfg);

    bool   isConnected()  const { return _hwConnected; }
    bool   isCapturing()  const { return _capturing; }
    bool   dataAvailable()const { return !_capBuffer.isEmpty(); }

    bool   startCapture(float freqMhz);
    void   stopCapture();
    String pollCaptured();

    bool   replaySignal(uint32_t id);

    uint32_t saveSignal(SubGhzSignal& sig);
    bool     deleteSignal(uint32_t id);
    bool     renameSignal(uint32_t id, const String& name);
    bool     getSignal(uint32_t id, SubGhzSignal& out) const;
    String   signalsToJson() const;

    void             saveGpioConfig(const SubGhzGpioConfig& cfg);
    SubGhzGpioConfig loadGpioConfig() const;

    String statusJson() const;

private:
    bool    _hwConnected = false;
    bool    _capturing   = false;
    float   _freqMhz     = 433.92f;
    String  _capBuffer;
    unsigned long _captureStart = 0;
    SubGhzGpioConfig _cfg;

    std::vector<SubGhzSignal> _signals;
    uint32_t _nextId = 1;

    void _loadSignals();
    void _saveSignals() const;

    // CC1101 SPI helpers (register-level, no external lib dependency)
    uint8_t  _spiRead(uint8_t addr);
    void     _spiWrite(uint8_t addr, uint8_t val);
    uint8_t  _spiReadStatus(uint8_t addr);
    void     _spiCommand(uint8_t cmd);
    void     _reset();
    bool     _detectCC1101();
    void     _setFrequency(float mhz);
    void     _setModeTx();
    void     _setModeRx();
    void     _setModeIdle();
    void     _spiByte(uint8_t b);

    // OOK/ASK raw capture via GDO0 pin
    void     _captureLoop();
    String   _rawToHex(const std::vector<uint16_t>& timings) const;

    std::vector<uint16_t> _captureTimings;
    bool     _lastGdo0   = false;
    unsigned long _lastEdge = 0;
};

extern SubGhzModule subGhzModule;
