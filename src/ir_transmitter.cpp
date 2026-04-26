// ============================================================
//  ir_transmitter.cpp  –  Multi-emitter IR transmit
// ============================================================
#include "ir_transmitter.h"
#include "ir_receiver.h"

IRTransmitter irTransmitter;

IRTransmitter::IRTransmitter() : _count(0) {
    for (uint8_t i = 0; i < IR_MAX_EMITTERS; ++i) {
        _senders[i] = nullptr;
        _pins[i]    = 255;
    }
}

IRTransmitter::~IRTransmitter() { destroyAll(); }

// ── begin ────────────────────────────────────────────────────
void IRTransmitter::begin(const IrPinConfig& pins) {
    destroyAll();
    _count = min((uint8_t)IR_MAX_EMITTERS, pins.emitCount);
    for (uint8_t i = 0; i < _count; ++i) {
        if (pins.emitEnabled[i]) createSender(i, pins.emitPin[i]);
    }
    Serial.printf(DEBUG_TAG " IR Transmitter: %d emitter(s) active\n",
                  activeCount());
    for (uint8_t i = 0; i < _count; ++i) {
        if (_senders[i])
            Serial.printf(DEBUG_TAG "   Emitter[%d] GPIO%d\n", i, _pins[i]);
    }
}

// ── reconfigure ──────────────────────────────────────────────
void IRTransmitter::reconfigure(const IrPinConfig& pins) {
    Serial.println(DEBUG_TAG " Reconfiguring emitters...");
    begin(pins);
}

// ── createSender / destroyAll ─────────────────────────────────
void IRTransmitter::createSender(uint8_t idx, uint8_t pin) {
    if (idx >= IR_MAX_EMITTERS) return;
    if (_senders[idx]) { delete _senders[idx]; _senders[idx] = nullptr; }

    PinStatus st = validateTxPin(pin);
    if (st != PinStatus::OK) {
        Serial.printf(DEBUG_TAG " Emitter[%d] GPIO%d rejected: %s\n",
                      idx, pin, pinStatusMsg(st));
        _pins[idx] = 255;
        return;
    }
    _senders[idx] = new IRsend(pin, false, true);
    _senders[idx]->begin();
    _pins[idx] = pin;
}

void IRTransmitter::destroyAll() {
    for (uint8_t i = 0; i < IR_MAX_EMITTERS; ++i) {
        if (_senders[i]) { delete _senders[i]; _senders[i] = nullptr; }
        _pins[i] = 255;
    }
    _count = 0;
}

// ── activeCount / emitterPin ──────────────────────────────────
uint8_t IRTransmitter::activeCount() const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < IR_MAX_EMITTERS; ++i)
        if (_senders[i]) ++n;
    return n;
}

uint8_t IRTransmitter::emitterPin(uint8_t idx) const {
    return (idx < IR_MAX_EMITTERS && _senders[idx]) ? _pins[idx] : 255;
}

// ── transmit (all enabled emitters) ──────────────────────────
bool IRTransmitter::transmit(const IRButton& btn) {
    if (!btn.isValid()) { Serial.println(DEBUG_TAG " TX: invalid button"); return false; }
    if (activeCount() == 0) { Serial.println(DEBUG_TAG " TX: no active emitters"); return false; }

    Serial.printf(DEBUG_TAG " TX %-10s 0x%llX  %db  reps=%d  count=%d  delay=%dms  emitters=%d\n",
                  protocolName(btn.protocol), (unsigned long long)btn.code,
                  btn.bits, btn.repeats, btn.repeatCount, btn.repeatDelay, activeCount());

    irReceiver.pause();
    // FIX: 20ms quiet-time before TX — was 5ms.
    // Some devices (especially older TVs/ACs) need 15-25ms
    // bus-quiet before accepting a new command.
    delay(20);

    bool ok = true;
    for (uint8_t i = 0; i < IR_MAX_EMITTERS; ++i) {
        if (_senders[i]) ok &= doTransmit(_senders[i], btn);
    }

    // Post-TX delay proportional to signal length
    uint32_t waitMs = 30;
    if (btn.protocol == IRProtocol::RAW && !btn.rawData.empty()) {
        uint32_t totalUs = 0;
        for (uint16_t v : btn.rawData) totalUs += v;
        waitMs = (totalUs / 1000u) + 20u;
        if (waitMs < 30u)  waitMs = 30u;
        if (waitMs > 500u) waitMs = 500u;
    }
    delay(waitMs);
    irReceiver.resume();  // always resume — even if doTransmit returned false
    return ok;
}

// ── transmitOn (single emitter) ───────────────────────────────
bool IRTransmitter::transmitOn(uint8_t idx, const IRButton& btn) {
    if (idx >= IR_MAX_EMITTERS || !_senders[idx]) {
        Serial.printf(DEBUG_TAG " transmitOn: emitter[%d] not active\n", idx);
        return false;
    }
    if (!btn.isValid()) return false;

    irReceiver.pause();
    delay(20);  // FIX: 20ms quiet-time (was 5ms)
    bool ok = doTransmit(_senders[idx], btn);
    delay(30);
    irReceiver.resume();  // always resume regardless of doTransmit result
    return ok;
}

// ── transmitRaw ──────────────────────────────────────────────
bool IRTransmitter::transmitRaw(const uint16_t* data, size_t len, uint16_t freqKHz) {
    if (!data || len < 4 || activeCount() == 0) return false;
    irReceiver.pause();
    delay(5);
    for (uint8_t i = 0; i < IR_MAX_EMITTERS; ++i)
        if (_senders[i]) _senders[i]->sendRaw(data, static_cast<uint16_t>(len), freqKHz);
    delay(30);
    irReceiver.resume();
    return true;
}

// ── doTransmit ───────────────────────────────────────────────
// Simple protocols → dedicated IRsend::sendXxx().
// Complex AC + RAW → sendRaw() with captured timing array.
//
// FIX: Protocol-aware minimum repeats.
// btn.repeats  = IRremoteESP8266 library repeat frames (e.g. NEC repeat frame)
// btn.repeatCount = our outer loop — how many full signals to send
//
// Root cause of "3-4 presses needed":
//   IR_SEND_REPEATS=0 means no NEC repeat frame is appended. Real remotes
//   always send at least 1 repeat frame while the button is held.
//   Fix: use protocol-aware minimum repeat frame count.
bool IRTransmitter::doTransmit(IRsend* s, const IRButton& btn) {
    uint8_t  total = (btn.repeatCount >= 1) ? btn.repeatCount : 1;
    uint16_t delMs = (btn.repeatDelay > 0) ? btn.repeatDelay : IR_DEFAULT_REPEAT_DELAY;

    // Protocol-aware minimum repeat frame count.
    // The library `repeats` param is the number of EXTRA frames after the first.
    // For most protocols 1 extra frame = much better reliability.
    // User-stored value wins if explicitly set > 0.
    uint8_t reps = btn.repeats;
    auto minRep = [&](uint8_t minVal) -> uint8_t {
        return (reps >= minVal) ? reps : minVal;
    };

    for (uint8_t rep = 0; rep < total; ++rep) {
        switch (btn.protocol) {
            case IRProtocol::NEC:
            case IRProtocol::NEC_LIKE:
                // NEC: 1 extra repeat frame = reliable single-press response
                s->sendNEC(btn.code, btn.bits, minRep(1)); break;
            case IRProtocol::SONY:
                // SONY spec requires signal sent 3x minimum
                s->sendSony(btn.code, btn.bits, minRep(2)); break;
            case IRProtocol::SAMSUNG:
                s->sendSAMSUNG(btn.code, btn.bits, minRep(1)); break;
            case IRProtocol::SAMSUNG36:
                s->sendSamsung36(btn.code, btn.bits, minRep(1)); break;
            case IRProtocol::LG:
                s->sendLG(btn.code, btn.bits, minRep(1)); break;
            case IRProtocol::PANASONIC: {
                uint16_t addr = (uint16_t)((btn.code >> 32) & 0xFFFF);
                uint32_t data = (uint32_t)(btn.code & 0xFFFFFFFF);
                s->sendPanasonic(addr, data,
                    btn.bits == 0 ? kPanasonicBits : btn.bits, btn.repeats);
                break; }
            case IRProtocol::RC5:
                s->sendRC5(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::RC6:
                s->sendRC6(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::JVC:
                s->sendJVC(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::DISH:
                s->sendDISH(btn.code, btn.bits, btn.repeats < 3 ? 3 : btn.repeats); break;
            case IRProtocol::SHARP: {
                uint8_t addr = (btn.code >> 8) & 0x1F;
                uint8_t cmd  = btn.code & 0xFF;
                s->sendSharp(addr, cmd, btn.bits, btn.repeats); break; }
            case IRProtocol::DENON:
                s->sendDenon(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::MITSUBISHI:
                s->sendMitsubishi(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::MITSUBISHI2:
                s->sendMitsubishi2(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::SANYO:
                s->sendSanyoLC7461(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::AIWA_RC_T501:
            case IRProtocol::AIWA_RC_T501_2:
                s->sendAiwaRCT501(btn.code, btn.repeats); break;
            case IRProtocol::NIKAI:
                s->sendNikai(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::RCMM:
                s->sendRCMM(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::LEGOPF:
                s->sendLegoPf(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::PIONEER:
                s->sendPioneer(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::EPSON:
                s->sendEpson(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::SYMPHONY:
                s->sendSymphony(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::BOSE:
                s->sendBose(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::METZ:
                s->sendMetz(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::DOSHISHA:
                s->sendDoshisha(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::GORENJE:
                s->sendGorenje(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::INAX:
                s->sendInax(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::LUTRON:
                s->sendLutron(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::ELITESCREENS:
                s->sendElitescreens(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::MILESTAG2:
                s->sendMilestag2(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::XMP:
                s->sendXmp(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::TRUMA:
                s->sendTruma(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::WOWWEE:
                s->sendWowwee(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::TECO:
                s->sendTeco(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::GOODWEATHER:
                s->sendGoodweather(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::MIDEA:
                s->sendMidea(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::MIDEA24:
                s->sendMidea24(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::COOLIX:
                s->sendCOOLIX(btn.code, btn.bits, btn.repeats < 2 ? 2 : btn.repeats); break;
            case IRProtocol::COOLIX48:
                s->sendCoolix48(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::GICABLE:
                s->sendGICable(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::MAGIQUEST:
                s->sendMagiQuest(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::LASERTAG:
                s->sendLasertag(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::ARRIS:
                s->sendArris(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::MULTIBRACKETS:
                s->sendMultibrackets(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::ZEPEAL:
                s->sendZepeal(btn.code, btn.bits, btn.repeats); break;
            case IRProtocol::MWM:
                // sendMWM() requires a byte array (not a 64-bit code).
                // MWM captured as code falls through to sendRaw if rawData available,
                // otherwise encode the code as a 3-byte array from btn.code.
                if (!btn.rawData.empty()) {
                    s->sendRaw(btn.rawData.data(),
                               static_cast<uint16_t>(btn.rawData.size()),
                               btn.freqKHz);
                } else {
                    // Reconstruct byte array from stored 64-bit code (up to 3 bytes)
                    uint8_t mwmData[3];
                    mwmData[0] = (btn.code >> 16) & 0xFF;
                    mwmData[1] = (btn.code >>  8) & 0xFF;
                    mwmData[2] =  btn.code        & 0xFF;
                    uint16_t nb = (btn.bits > 0) ? (btn.bits + 7) / 8 : 3;
                    if (nb > 3) nb = 3;
                    s->sendMWM(mwmData, nb, btn.repeats);
                }
                break;

            // Complex AC + explicit RAW → sendRaw()
            case IRProtocol::RAW:
            default:
                if (btn.rawData.empty()) return false;
                s->sendRaw(btn.rawData.data(),
                           static_cast<uint16_t>(btn.rawData.size()),
                           btn.freqKHz);
                break;
        }
        if (rep + 1 < total && delMs > 0) delay(delMs);
    }
    return true;
}

// ── activePins ───────────────────────────────────────────────
// Returns GPIO numbers of all active emitter slots.
// Used by /api/v1/ir/pwm-info to report emitter config to the UI.
std::vector<uint8_t> IRTransmitter::activePins() const {
    std::vector<uint8_t> result;
    for (uint8_t i = 0; i < IR_MAX_EMITTERS; ++i) {
        if (_senders[i] && _pins[i] != 255)
            result.push_back(_pins[i]);
    }
    return result;
}
