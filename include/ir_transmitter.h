#pragma once
// ============================================================
//  ir_transmitter.h  –  Multi-emitter IR transmit wrapper
//
//  v1.2.0: Supports up to IR_MAX_EMITTERS (4) independent
//  IRsend instances on separate GPIOs.  Each emitter can be
//  enabled/disabled and has its own GPIO assignment.
//  IRsend instances are heap-allocated — pins can be changed
//  at runtime without rebooting.
// ============================================================
#include <Arduino.h>
#include <vector>
#include <IRsend.h>
#include "ir_button.h"
#include "config.h"
#include "gpio_config.h"

class IRTransmitter {
public:
    IRTransmitter();
    ~IRTransmitter();

    // Initialise emitters from IrPinConfig.
    void begin(const IrPinConfig& pins);

    // Reconfigure emitters at runtime (no reboot needed).
    // Destroys and recreates all IRsend instances.
    void reconfigure(const IrPinConfig& pins);

    // Transmit on ALL enabled emitters simultaneously.
    bool transmit(const IRButton& btn);

    // Transmit on a specific emitter index only (0-based).
    bool transmitOn(uint8_t emitterIdx, const IRButton& btn);

    // Raw transmit on all enabled emitters.
    bool transmitRaw(const uint16_t* data, size_t len,
                     uint16_t freqKHz = IR_DEFAULT_FREQ_KHZ);

    // Number of currently active (enabled) emitters.
    uint8_t activeCount() const;
    // Returns vector of active GPIO pin numbers (one per active emitter slot).
    // Used by /api/v1/ir/pwm-info to report emitter configuration to UI.
    std::vector<uint8_t> activePins() const;

    // Active GPIO for emitter at index i (255 = disabled/invalid).
    uint8_t emitterPin(uint8_t idx) const;

private:
    IRsend*  _senders[IR_MAX_EMITTERS];  // heap-allocated, nullptr = inactive
    uint8_t  _pins   [IR_MAX_EMITTERS];  // active GPIO per slot
    uint8_t  _count;                     // number of configured slots

    void destroyAll();
    void createSender(uint8_t idx, uint8_t pin);

    bool doTransmit(IRsend* s, const IRButton& btn);
    // All protocol send logic is in doTransmit() — no per-protocol helpers needed.
};

extern IRTransmitter irTransmitter;
