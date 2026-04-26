// ============================================================
//  nrf24_module.cpp  –  NRF24L01 Real Hardware Implementation
// ============================================================
#include "nrf24_module.h"

#define NRF24_TAG "[NRF24]"

Nrf24Module nrf24Module;

// ─────────────────────────────────────────────────────────────
void Nrf24Module::begin() {
    _cfg = loadGpioConfig();
    memset(_scanChannels, 0, sizeof(_scanChannels));

    // Create SPI bus
    if (_spi) { delete _spi; _spi = nullptr; }
    if (_radio) { delete _radio; _radio = nullptr; }

    _spi = (_cfg.spiBus == 1)
        ? new SPIClass(HSPI)
        : new SPIClass(VSPI);
    // DO NOT pass CSN as SS to SPI.begin — RF24 manages CSN via digitalWrite.
    // Passing CSN here causes the SPI hardware to also drive it, creating conflicts.
    _spi->begin(_cfg.sck, _cfg.miso, _cfg.mosi, _cfg.csn);
    _spi->setFrequency(10000000);  // 10 MHz — NRF24L01 max SPI clock

    // CE and CSN must be OUTPUT before RF24::begin()
    pinMode(_cfg.ce,  OUTPUT); digitalWrite(_cfg.ce,  LOW);
    pinMode(_cfg.csn, OUTPUT); digitalWrite(_cfg.csn, HIGH);

    _radio = new RF24(_cfg.ce, _cfg.csn);

    bool beginOk = _radio->begin(_spi);
    Serial.printf(NRF24_TAG " RF24::begin=%s CE=%u CSN=%u SCK=%u MOSI=%u MISO=%u bus=%s\n",
                  beginOk?"OK":"FAIL", _cfg.ce, _cfg.csn,
                  _cfg.sck, _cfg.mosi, _cfg.miso,
                  _cfg.spiBus==1?"HSPI":"VSPI");

    if (beginOk) {
        _hwConnected = _radio->isChipConnected();
        Serial.printf(NRF24_TAG " isChipConnected=%s\n",
                      _hwConnected?"YES":"NO (SPI OK but no NRF24 response)");
    } else {
        _hwConnected = false;
        Serial.println(NRF24_TAG " RF24::begin failed — check CE/CSN/SPI wiring");
    }

    if (_hwConnected) {
        _applyConfig();
        Serial.printf(NRF24_TAG " Connected — CE=%u CSN=%u ch=%u\n",
                      _cfg.ce, _cfg.csn, _channel);
    } else {
        Serial.println(NRF24_TAG " Not detected — verify: CE/CSN not swapped, 3.3V power, 100uF cap on VCC");
    }
}

// ─────────────────────────────────────────────────────────────
void Nrf24Module::reinit(const Nrf24GpioConfig& cfg) {
    saveGpioConfig(cfg);
    _cfg = cfg;
    if (_radio) { _radio->powerDown(); delete _radio; _radio = nullptr; }
    if (_spi)   { _spi->end();         delete _spi;   _spi   = nullptr; }
    begin();
}

// ─────────────────────────────────────────────────────────────
void Nrf24Module::_applyConfig() {
    if (!_radio || !_hwConnected) return;
    _radio->setPALevel(_toRF24Pa(_paLevel));
    _radio->setDataRate(_toRF24Rate(_dataRate));
    _radio->setChannel(_channel);
    _radio->setAutoAck(false);
    _radio->setPayloadSize(32);
}

// ─────────────────────────────────────────────────────────────
void Nrf24Module::loop() {
    if (!_hwConnected || !_radio) return;

    // ── Channel scanner ──────────────────────────────────────
    if (_scanning && (millis() - _scanTimer) > 5) {
        _scanTimer = millis();
        _radio->stopListening();
        for (uint8_t ch = 0; ch < NRF24_CHANNELS; ch++) {
            _radio->setChannel(ch);
            _radio->startListening();
            delayMicroseconds(128);
            _radio->stopListening();
            if (_radio->testCarrier()) {
                if (_scanChannels[ch] < 255) _scanChannels[ch]++;
            } else {
                if (_scanChannels[ch] > 0) _scanChannels[ch]--;
            }
        }
        // Restore channel
        _radio->setChannel(_channel);
        _radio->startListening();
    }

    // ── Sniffer ──────────────────────────────────────────────
    // Auto-stop sniffer after 5 min to prevent _sniffPending unbounded growth
    if (_sniffing && (millis() - _sniffTimer) > 300000UL) {
        stopSniff();
        Serial.println(NRF24_TAG " Sniffer auto-stopped after 5 min timeout");
    }
    if (_sniffing) {
        if (_radio->available()) {
            uint8_t buf[32] = {};
            _radio->read(buf, 32);
            char hex[65];
            for (int i = 0; i < 32; i++)
                snprintf(hex + i*2, 3, "%02X", buf[i]);
            hex[64] = '\0';

            Nrf24Packet pkt;
            pkt.channel   = _channel;
            pkt.timestamp = millis();
            pkt.data      = String(hex);

            char jsonbuf[100];
            snprintf(jsonbuf, sizeof(jsonbuf),
                     "{\"ch\":%u,\"data\":\"%s\"}", _channel, hex);
            _sniffPending += String(jsonbuf) + "\n";

            if (_capReplay && _replayBuf.size() < 256)
                _replayBuf.push_back(pkt);
        }
    }
}

// ─────────────────────────────────────────────────────────────
void Nrf24Module::setChannel(uint8_t ch) {
    _channel = ch;
    if (_radio && _hwConnected) {
        _radio->setChannel(ch);
        Serial.printf(NRF24_TAG " Channel set to %u\n", ch);
    }
}

void Nrf24Module::setDataRate(Nrf24DataRate r) {
    _dataRate = r;
    if (_radio && _hwConnected)
        _radio->setDataRate(_toRF24Rate(r));
}

// ─────────────────────────────────────────────────────────────
void Nrf24Module::startScan() {
    if (!_hwConnected) return;
    memset(_scanChannels, 0, sizeof(_scanChannels));
    _scanning  = true;
    _scanTimer = millis();
    _radio->stopListening();
    Serial.println(NRF24_TAG " Channel scan started");
}

void Nrf24Module::stopScan() {
    _scanning = false;
    if (_radio && _hwConnected) {
        _radio->setChannel(_channel);
        _radio->startListening();
    }
    Serial.println(NRF24_TAG " Channel scan stopped");
}

// ─────────────────────────────────────────────────────────────
void Nrf24Module::startSniff() {
    if (!_hwConnected) return;
    _sniffPending = "";
    _sniffing    = true;
    _sniffTimer  = millis();
    _radio->setChannel(_channel);
    _radio->startListening();
    Serial.println(NRF24_TAG " Sniffer started");
}

void Nrf24Module::stopSniff() {
    _sniffing = false;
    if (_radio && _hwConnected) _radio->stopListening();
    Serial.println(NRF24_TAG " Sniffer stopped");
}

String Nrf24Module::pollSniffPacket() {
    String r = _sniffPending;
    _sniffPending = "";
    return r;
}

// ─────────────────────────────────────────────────────────────
void Nrf24Module::startReplayCapture() {
    _capReplay = true;
    _replayBuf.clear();
    Serial.println(NRF24_TAG " Replay capture started");
}

void Nrf24Module::stopReplayCapture() {
    _capReplay = false;
    Serial.printf(NRF24_TAG " Replay stopped, %u packets\n",
                  (unsigned)_replayBuf.size());
}

bool Nrf24Module::replayPackets() {
    if (!_hwConnected || _replayBuf.empty()) return false;
    const uint8_t addr[6] = "1Node";
    _radio->openWritingPipe(addr);
    _radio->stopListening();
    for (const auto& pkt : _replayBuf) {
        _radio->setChannel(pkt.channel);
        uint8_t buf[32] = {};
        // Parse hex data back to bytes
        for (int i = 0; i < 32 && (i*2+1) < (int)pkt.data.length(); i++) {
            char h[3] = {pkt.data[i*2], pkt.data[i*2+1], '\0'};
            buf[i] = (uint8_t)strtol(h, nullptr, 16);
        }
        _radio->write(buf, 32);
        delay(2);
        yield();  // feed watchdog during replay
    }
    _radio->setChannel(_channel);
    _radio->startListening();
    Serial.printf(NRF24_TAG " Replayed %u packets\n",
                  (unsigned)_replayBuf.size());
    return true;
}

// ─────────────────────────────────────────────────────────────
void Nrf24Module::sendRcCommand(char dir, uint8_t speed) {
    if (!_hwConnected || !_radio) return;
    const uint8_t addr[6] = "RCCAR";
    _radio->openWritingPipe(addr);
    _radio->stopListening();
    uint8_t payload[4] = {(uint8_t)dir, speed, 0, 0};
    _radio->write(payload, 4);
    _radio->startListening();
    Serial.printf(NRF24_TAG " RC dir=%c speed=%u\n", dir, speed);
}

// ─────────────────────────────────────────────────────────────
String Nrf24Module::statusJson() const {
    const char* rateStr =
        (_dataRate == Nrf24DataRate::RATE_250K) ? "250K" :
        (_dataRate == Nrf24DataRate::RATE_2M)   ? "2M"   : "1M";
    const char* paStr =
        (_paLevel == Nrf24PaLevel::PA_MIN)  ? "MIN"  :
        (_paLevel == Nrf24PaLevel::PA_LOW)  ? "LOW"  :
        (_paLevel == Nrf24PaLevel::PA_MAX)  ? "MAX"  : "HIGH";
    char buf[180];
    snprintf(buf, sizeof(buf),
             "{\"connected\":%s,\"channel\":%u,\"dataRate\":\"%s\","
             "\"paLevel\":\"%s\",\"scanning\":%s,\"sniffing\":%s,"
             "\"replayPkts\":%u}",
             _hwConnected?"true":"false", _channel, rateStr, paStr,
             _scanning?"true":"false", _sniffing?"true":"false",
             (unsigned)_replayBuf.size());
    return String(buf);
}

// ─────────────────────────────────────────────────────────────
void Nrf24Module::saveGpioConfig(const Nrf24GpioConfig& cfg) {
    File f = LittleFS.open(NRF24_GPIO_FILE, "w");
    if (!f) return;
    JsonDocument doc;
    doc["moduleType"] = (int)cfg.moduleType;
    doc["ce"]   = cfg.ce;   doc["csn"]  = cfg.csn;
    doc["sck"]  = cfg.sck;  doc["mosi"] = cfg.mosi;
    doc["miso"] = cfg.miso; doc["irq"]  = cfg.irq;
    doc["spiBus"]  = cfg.spiBus;
    doc["dataRate"]= (int)cfg.dataRate;
    doc["paLevel"] = (int)cfg.paLevel;
    serializeJson(doc, f);
    f.close();
}

Nrf24GpioConfig Nrf24Module::loadGpioConfig() const {
    Nrf24GpioConfig cfg;
    if (!LittleFS.exists(NRF24_GPIO_FILE)) return cfg;
    File f = LittleFS.open(NRF24_GPIO_FILE, "r");
    if (!f) return cfg;
    JsonDocument doc;
    if (deserializeJson(doc, f) == DeserializationError::Ok) {
        cfg.moduleType = (Nrf24Module_t)(doc["moduleType"] | 0);
        cfg.ce   = doc["ce"]   | (uint8_t)16;
        cfg.csn  = doc["csn"]  | (uint8_t)17;
        cfg.sck  = doc["sck"]  | (uint8_t)18;
        cfg.mosi = doc["mosi"] | (uint8_t)23;
        cfg.miso = doc["miso"] | (uint8_t)19;
        cfg.irq  = doc["irq"]  | (uint8_t)0;
        cfg.spiBus   = doc["spiBus"]   | (uint8_t)0;
        cfg.dataRate = (Nrf24DataRate)(doc["dataRate"] | 1);
        cfg.paLevel  = (Nrf24PaLevel)(doc["paLevel"]   | 2);
    }
    f.close();
    return cfg;
}

// ─────────────────────────────────────────────────────────────
rf24_datarate_e Nrf24Module::_toRF24Rate(Nrf24DataRate r) const {
    switch (r) {
        case Nrf24DataRate::RATE_250K: return RF24_250KBPS;
        case Nrf24DataRate::RATE_2M:   return RF24_2MBPS;
        default:                        return RF24_1MBPS;
    }
}

rf24_pa_dbm_e Nrf24Module::_toRF24Pa(Nrf24PaLevel p) const {
    switch (p) {
        case Nrf24PaLevel::PA_MIN:  return RF24_PA_MIN;
        case Nrf24PaLevel::PA_LOW:  return RF24_PA_LOW;
        case Nrf24PaLevel::PA_MAX:  return RF24_PA_MAX;
        default:                     return RF24_PA_HIGH;
    }
}
