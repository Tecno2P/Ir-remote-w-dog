#pragma once
// ============================================================
//  rule_manager.h  –  Batch 2: IF-THEN Automation Engine
//
//  Ek rule = ek trigger condition + ek ya zyada actions
//
//  TRIGGERS (IF):
//    RFID_SCAN      — koi specific card scan ho
//    RFID_UNKNOWN   — unknown card scan ho
//    NFC_SCAN       — NFC tag detect ho
//    IR_RECEIVED    — specific IR signal receive ho
//    SCHEDULE       — time-based (uses existing scheduler)
//    WIFI_CONNECT   — WiFi connect ho
//    WIFI_DISCONNECT— WiFi disconnect ho
//    BOOT           — device start ho
//    MANUAL         — web UI / REST API se manually fire karo
//
//  ACTIONS (THEN):
//    IR_TRANSMIT    — IR signal bhejo
//    MACRO_RUN      — macro chalao
//    NOTIFY         — Telegram/WhatsApp notification bhejo
//    NOTIFY_TELEGRAM— sirf Telegram
//    NOTIFY_WHATSAPP— sirf WhatsApp
//    BUZZER         — buzzer beep karo
//    LOG            — sirf audit log mein likhoo
//
//  Example Rule:
//    IF RFID_UNKNOWN → THEN NOTIFY + BUZZER
//    IF RFID_SCAN (card="Admin") → THEN IR_TRANSMIT (btn=5) + NOTIFY
//
//  Storage: /rules/<id>.json
//  API: /api/v1/rules/*
// ============================================================
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <vector>
#include <functional>

#define RULES_DIR           "/rules"
#define MAX_PENDING_ACTIONS   64   // FIX: cap deferred action queue
#define RULES_MAX           32
#define RULE_NAME_MAX       40
#define RULE_TAG            "[RULE]"

// ── Trigger types ─────────────────────────────────────────────
enum class RuleTrigger : uint8_t {
    RFID_SCAN       = 0,
    RFID_UNKNOWN    = 1,
    NFC_SCAN        = 2,
    IR_RECEIVED     = 3,
    WIFI_CONNECT    = 4,
    WIFI_DISCONNECT = 5,
    BOOT            = 6,
    MANUAL          = 7,
    UNKNOWN         = 255
};

// ── Action types ──────────────────────────────────────────────
enum class RuleAction : uint8_t {
    IR_TRANSMIT      = 0,
    MACRO_RUN        = 1,
    NOTIFY           = 2,   // both channels
    NOTIFY_TELEGRAM  = 3,
    NOTIFY_WHATSAPP  = 4,
    BUZZER           = 5,
    LOG              = 6,
    UNKNOWN          = 255
};

// ── Single action in a rule ───────────────────────────────────
struct RuleActionStep {
    RuleAction action;
    String     param1;    // IR: buttonId / Macro: name / Notify: message
    String     param2;    // extra param if needed
    uint32_t   delayMs;   // delay before this action

    RuleActionStep() : action(RuleAction::LOG), delayMs(0) {}
};

// ── Full rule definition ──────────────────────────────────────
struct RuleEntry {
    uint32_t    id;
    String      name;
    bool        enabled;
    RuleTrigger trigger;
    String      triggerParam;   // e.g. RFID UID, IR button id, etc.
    std::vector<RuleActionStep> actions;
    uint32_t    firedCount;     // how many times this rule fired
    String      lastFiredAt;    // timestamp string

    RuleEntry()
        : id(0), enabled(true),
          trigger(RuleTrigger::MANUAL),
          firedCount(0) {}

    // JSON serialisation
    void    toJson(JsonObject obj) const;
    bool    fromJson(JsonObjectConst obj);
    String  toJsonString() const;
};

// ── Callbacks for actions ─────────────────────────────────────
using RuleIrCallback     = std::function<void(uint32_t buttonId)>;
using RuleMacroCallback  = std::function<void(const String& name)>;
using RuleNotifyCallback = std::function<void(const String& msg,
                                               bool telegram,
                                               bool whatsapp)>;
using RuleBuzzerCallback = std::function<void(uint8_t times)>;

class RuleManager {
public:
    RuleManager();

    void begin();   // create /rules dir, load all rules
    void loop();    // execute pending deferred actions

    // ── Trigger events — called from main modules ─────────────
    void triggerRfidScan    (const String& uid, const String& cardName, bool known);
    void triggerNfcScan     (const String& uid, const String& tagName);
    void triggerIrReceived  (uint32_t buttonId, const String& protocol);
    void triggerWifiConnect (const String& ssid);
    void triggerWifiDisconnect();
    void triggerBoot        ();
    void triggerManual      (uint32_t ruleId);   // from REST API

    // ── CRUD ──────────────────────────────────────────────────
    uint32_t    addRule    (RuleEntry& entry);
    bool        updateRule (const RuleEntry& entry);
    bool        deleteRule (uint32_t id);
    bool        setEnabled (uint32_t id, bool en);

    // List all rules (loads from LittleFS)
    std::vector<RuleEntry> listRules() const;

    // Load single rule by id
    bool loadRule(uint32_t id, RuleEntry& out) const;

    size_t ruleCount() const;

    // JSON for all rules
    String allRulesToJson() const;

    // ── Register action callbacks ─────────────────────────────
    void onIrTransmit (RuleIrCallback     cb) { _irCb     = cb; }
    void onMacroRun   (RuleMacroCallback  cb) { _macroCb  = cb; }
    void onNotify     (RuleNotifyCallback cb) { _notifyCb = cb; }
    void onBuzzer     (RuleBuzzerCallback cb) { _buzzerCb = cb; }

private:
    uint32_t          _nextId;
    RuleIrCallback    _irCb;
    RuleMacroCallback _macroCb;
    RuleNotifyCallback _notifyCb;
    RuleBuzzerCallback _buzzerCb;

    // Deferred execution (non-blocking delays between actions)
    struct DeferredAction {
        RuleActionStep step;
        unsigned long  fireAt;   // millis()
        uint32_t       ruleId;
    };
    std::vector<DeferredAction> _pending;

    void _fireTrigger   (RuleTrigger trigger, const String& param);
    void _executeRule   (RuleEntry& rule);
    void _executeAction (const RuleActionStep& step, uint32_t ruleId);
    void _saveRule      (const RuleEntry& rule) const;
    bool _loadRule      (uint32_t id, RuleEntry& out) const;
    void _deleteRuleFile(uint32_t id) const;
    String _rulePath    (uint32_t id) const;
    String _triggerToStr(RuleTrigger t) const;
    String _actionToStr (RuleAction  a) const;
    RuleTrigger _strToTrigger(const String& s) const;
    RuleAction  _strToAction (const String& s) const;
    String _buildTimeStr() const;
};

extern RuleManager ruleMgr;
