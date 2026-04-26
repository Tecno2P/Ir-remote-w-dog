#pragma once
// ============================================================
//  nrf24_module.h  –  NRF24L01 2.4GHz — Real HW Implementation
// ============================================================
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SPI.h>
#include <RF24.h>
#include <vector>

#define NRF24_GPIO_FILE  "/nrf24_gpio.json"
#define NRF24_CHANNELS   125

enum class Nrf24Module_t { NRF24L01, NRF24L01_PA_LNA, NRF24L01_SMA };
enum class Nrf24DataRate  { RATE_250K, RATE_1M, RATE_2M };
enum class Nrf24PaLevel   { PA_MIN, PA_LOW, PA_HIGH, PA_MAX };

struct Nrf24GpioConfig {
    Nrf24Module_t moduleType = Nrf24Module_t::NRF24L01;
    uint8_t  ce      = 16;
    uint8_t  csn     = 17;
    // HSPI bus by default to avoid conflict with SD card (VSPI: SCK=18,MOSI=23,MISO=19)
    uint8_t  sck     = 14;   // HSPI SCK  (was 18 = conflicts with SD)
    uint8_t  mosi    = 13;   // HSPI MOSI (was 23 = conflicts with SD)
    uint8_t  miso    = 12;   // HSPI MISO (was 19 = conflicts with SD)
    uint8_t  irq     = 0;     // 0 = disabled
    uint8_t  spiBus  = 1;     // 1=HSPI (SD uses VSPI — do NOT use 0 here)
    Nrf24DataRate dataRate = Nrf24DataRate::RATE_1M;
    Nrf24PaLevel  paLevel  = Nrf24PaLevel::PA_HIGH;
};

struct Nrf24Packet {
    uint8_t  channel   = 0;
    uint32_t timestamp = 0;
    String   data;
};

class Nrf24Module {
public:
    void   begin();
    void   loop();
    void   reinit(const Nrf24GpioConfig& cfg);

    bool   isConnected()  const { return _hwConnected; }
    bool   isScanning()   const { return _scanning; }
    bool   isSniffing()   const { return _sniffing; }
    bool   isCapturingReplay() const { return _capReplay; }

    // Channel scanner
    void   startScan();
    void   stopScan();
    const uint8_t* scanData() const { return _scanChannels; }

    // Sniffer
    void   startSniff();
    void   stopSniff();
    String pollSniffPacket();

    // Replay
    void   startReplayCapture();
    void   stopReplayCapture();
    size_t replayPacketCount() const { return _replayBuf.size(); }
    bool   replayPackets();

    // RC command
    void   sendRcCommand(char dir, uint8_t speed);

    // Runtime config
    void   setChannel(uint8_t ch);
    void   setDataRate(Nrf24DataRate r);

    // GPIO config persistence
    void            saveGpioConfig(const Nrf24GpioConfig& cfg);
    Nrf24GpioConfig loadGpioConfig() const;

    // Status JSON
    String statusJson() const;

private:
    RF24*        _radio       = nullptr;
    bool         _hwConnected = false;
    bool         _scanning    = false;
    bool         _sniffing    = false;
    bool         _capReplay   = false;
    uint8_t      _channel     = 76;
    Nrf24DataRate _dataRate   = Nrf24DataRate::RATE_1M;
    Nrf24PaLevel  _paLevel    = Nrf24PaLevel::PA_HIGH;
    Nrf24GpioConfig _cfg;

    uint8_t  _scanChannels[NRF24_CHANNELS] = {};
    unsigned long _scanTimer   = 0;
    unsigned long _sniffTimer  = 0;
    String   _sniffPending;
    std::vector<Nrf24Packet> _replayBuf;

    SPIClass* _spi = nullptr;

    rf24_datarate_e   _toRF24Rate(Nrf24DataRate r) const;
    rf24_pa_dbm_e     _toRF24Pa(Nrf24PaLevel p) const;
    void _applyConfig();
};

extern Nrf24Module nrf24Module;
