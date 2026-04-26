// ============================================================
//  rule_manager.cpp  –  Batch 2: IF-THEN Automation Engine
// ============================================================
#include "rule_manager.h"
#include "audit_manager.h"
#include "scheduler.h"
#include <ctime>

RuleManager ruleMgr;

// ─────────────────────────────────────────────────────────────
RuleManager::RuleManager() : _nextId(1) {}

// ─────────────────────────────────────────────────────────────
void RuleManager::begin() {
    if (!LittleFS.exists(RULES_DIR)) {
        LittleFS.mkdir(RULES_DIR);
    }
    // Find max existing ID to set _nextId
    File dir = LittleFS.open(RULES_DIR);
    if (dir && dir.isDirectory()) {
        File f = dir.openNextFile();
        while (f) {
            if (!f.isDirectory()) {
                String name = String(f.name());
                name.replace(".json", "");
                uint32_t id = name.toInt();
                if (id >= _nextId) _nextId = id + 1;
            }
            f = dir.openNextFile();
        }
    }
    Serial.printf(RULE_TAG " Started — %u rules loaded, nextId=%u\n",
                  (unsigned)ruleCount(), _nextId);
}

// ─────────────────────────────────────────────────────────────
void RuleManager::loop() {
    if (_pending.empty()) return;
    unsigned long now = millis();
    for (auto it = _pending.begin(); it != _pending.end(); ) {
        if (now >= it->fireAt) {
            _executeAction(it->step, it->ruleId);
            it = _pending.erase(it);
        } else {
            ++it;
        }
    }
}

// ─────────────────────────────────────────────────────────────
//  Trigger events
// ─────────────────────────────────────────────────────────────
void RuleManager::triggerRfidScan(const String& uid,
                                   const String& cardName,
                                   bool known) {
    if (!known) {
        _fireTrigger(RuleTrigger::RFID_UNKNOWN, uid);
    }
    // Fire RFID_SCAN rules — check if triggerParam matches uid or cardName
    auto rules = listRules();
    for (auto& r : rules) {
        if (!r.enabled) continue;
        if (r.trigger != RuleTrigger::RFID_SCAN) continue;
        // triggerParam empty = fire on any RFID; otherwise match uid or name
        if (r.triggerParam.isEmpty()
            || r.triggerParam.equalsIgnoreCase(uid)
            || r.triggerParam.equalsIgnoreCase(cardName)) {
            _executeRule(r);
        }
    }
}

void RuleManager::triggerNfcScan(const String& uid, const String& tagName) {
    _fireTrigger(RuleTrigger::NFC_SCAN, uid);
}

void RuleManager::triggerIrReceived(uint32_t buttonId, const String& protocol) {
    _fireTrigger(RuleTrigger::IR_RECEIVED, String(buttonId));
}

void RuleManager::triggerWifiConnect(const String& ssid) {
    _fireTrigger(RuleTrigger::WIFI_CONNECT, ssid);
}

void RuleManager::triggerWifiDisconnect() {
    _fireTrigger(RuleTrigger::WIFI_DISCONNECT, "");
}

void RuleManager::triggerBoot() {
    _fireTrigger(RuleTrigger::BOOT, "");
}

void RuleManager::triggerManual(uint32_t ruleId) {
    RuleEntry rule;
    if (loadRule(ruleId, rule) && rule.enabled) {
        _executeRule(rule);
    }
}

// ─────────────────────────────────────────────────────────────
//  Internal fire — matches trigger type + param
// ─────────────────────────────────────────────────────────────
void RuleManager::_fireTrigger(RuleTrigger trigger, const String& param) {
    auto rules = listRules();
    for (auto& r : rules) {
        if (!r.enabled) continue;
        if (r.trigger != trigger) continue;
        if (!r.triggerParam.isEmpty() &&
            !r.triggerParam.equalsIgnoreCase(param)) continue;
        _executeRule(r);
    }
}

// ─────────────────────────────────────────────────────────────
//  Execute all actions of a rule (with delays)
// ─────────────────────────────────────────────────────────────
void RuleManager::_executeRule(RuleEntry& rule) {
    Serial.printf(RULE_TAG " Firing rule #%u: %s\n", rule.id, rule.name.c_str());

    // Update stats
    rule.firedCount++;
    rule.lastFiredAt = _buildTimeStr();
    _saveRule(rule);

    // Audit
    auditMgr.log(AuditSource::RULE, "RULE_FIRED",
                 String("Rule: ") + rule.name + " (id=" + rule.id + ")");

    unsigned long offset = 0;
    for (const auto& step : rule.actions) {
        offset += step.delayMs;
        if (offset == 0) {
            // Execute immediately
            _executeAction(step, rule.id);
        } else {
            // Schedule deferred
            DeferredAction da;
            da.step   = step;
            da.fireAt = millis() + offset;
            da.ruleId = rule.id;
            // FIX: cap pending queue to avoid heap exhaustion under rapid fire
            if (_pending.size() >= MAX_PENDING_ACTIONS) {
                Serial.printf(RULE_TAG " WARNING: pending queue full (%u), dropping oldest\n",
                              (unsigned)_pending.size());
                _pending.erase(_pending.begin());
            }
            _pending.push_back(da);
        }
    }
}

// ─────────────────────────────────────────────────────────────
//  Execute a single action
// ─────────────────────────────────────────────────────────────
void RuleManager::_executeAction(const RuleActionStep& step, uint32_t ruleId) {
    Serial.printf(RULE_TAG " Action: %s param1=%s\n",
                  _actionToStr(step.action).c_str(), step.param1.c_str());

    switch (step.action) {
        case RuleAction::IR_TRANSMIT:
            if (_irCb) _irCb(step.param1.toInt());
            break;

        case RuleAction::MACRO_RUN:
            if (_macroCb) _macroCb(step.param1);
            break;

        case RuleAction::NOTIFY:
            if (_notifyCb) _notifyCb(step.param1, true, true);
            break;

        case RuleAction::NOTIFY_TELEGRAM:
            if (_notifyCb) _notifyCb(step.param1, true, false);
            break;

        case RuleAction::NOTIFY_WHATSAPP:
            if (_notifyCb) _notifyCb(step.param1, false, true);
            break;

        case RuleAction::BUZZER:
            if (_buzzerCb) {
                uint8_t times = step.param1.toInt();
                if (times < 1) times = 1;
                if (times > 5) times = 5;
                _buzzerCb(times);
            }
            break;

        case RuleAction::LOG:
            auditMgr.log(AuditSource::RULE, "RULE_LOG",
                         String("Rule #") + ruleId + ": " + step.param1);
            break;

        default:
            break;
    }
}

// ─────────────────────────────────────────────────────────────
//  CRUD
// ─────────────────────────────────────────────────────────────
uint32_t RuleManager::addRule(RuleEntry& entry) {
    if (ruleCount() >= RULES_MAX) {
        Serial.println(RULE_TAG " Max rules reached");
        return 0;
    }
    entry.id = _nextId++;
    _saveRule(entry);
    Serial.printf(RULE_TAG " Added rule #%u: %s\n", entry.id, entry.name.c_str());
    return entry.id;
}

bool RuleManager::updateRule(const RuleEntry& entry) {
    if (!LittleFS.exists(_rulePath(entry.id))) return false;
    _saveRule(entry);
    return true;
}

bool RuleManager::deleteRule(uint32_t id) {
    String path = _rulePath(id);
    if (!LittleFS.exists(path)) return false;
    LittleFS.remove(path);
    Serial.printf(RULE_TAG " Deleted rule #%u\n", id);
    return true;
}

void RuleManager::_deleteRuleFile(uint32_t id) const {
    String path = _rulePath(id);
    if (LittleFS.exists(path)) {
        LittleFS.remove(path);
        Serial.printf(RULE_TAG " _deleteRuleFile: removed %s\n", path.c_str());
    }
}

bool RuleManager::setEnabled(uint32_t id, bool en) {
    RuleEntry rule;
    if (!_loadRule(id, rule)) return false;
    rule.enabled = en;
    _saveRule(rule);
    return true;
}

// ─────────────────────────────────────────────────────────────
//  List / Load
// ─────────────────────────────────────────────────────────────
std::vector<RuleEntry> RuleManager::listRules() const {
    std::vector<RuleEntry> result;
    File dir = LittleFS.open(RULES_DIR);
    if (!dir || !dir.isDirectory()) return result;
    File f = dir.openNextFile();
    while (f) {
        if (!f.isDirectory()) {
            String name = String(f.name());
            if (name.endsWith(".json")) {
                name.replace(".json", "");
                uint32_t id = name.toInt();
                RuleEntry rule;
                if (_loadRule(id, rule)) result.push_back(rule);
            }
        }
        f = dir.openNextFile();
    }
    return result;
}

bool RuleManager::loadRule(uint32_t id, RuleEntry& out) const {
    return _loadRule(id, out);
}

size_t RuleManager::ruleCount() const {
    size_t count = 0;
    File dir = LittleFS.open(RULES_DIR);
    if (!dir || !dir.isDirectory()) return 0;
    File f = dir.openNextFile();
    while (f) {
        if (!f.isDirectory()) count++;
        f = dir.openNextFile();
    }
    return count;
}

String RuleManager::allRulesToJson() const {
    auto rules = listRules();
    String out = "{\"count\":";
    out += rules.size();
    out += ",\"rules\":[";
    for (size_t i = 0; i < rules.size(); ++i) {
        if (i) out += ",";
        out += rules[i].toJsonString();
    }
    out += "]}";
    return out;
}

// ─────────────────────────────────────────────────────────────
//  Persistence helpers
// ─────────────────────────────────────────────────────────────
void RuleManager::_saveRule(const RuleEntry& rule) const {
    File f = LittleFS.open(_rulePath(rule.id), "w");
    if (!f) return;
    f.print(rule.toJsonString());
    f.close();
}

bool RuleManager::_loadRule(uint32_t id, RuleEntry& out) const {
    String path = _rulePath(id);
    if (!LittleFS.exists(path)) return false;
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    JsonDocument doc;
    bool ok = (deserializeJson(doc, f) == DeserializationError::Ok);
    f.close();
    if (!ok) return false;
    return out.fromJson(doc.as<JsonObjectConst>());
}

String RuleManager::_rulePath(uint32_t id) const {
    return String(RULES_DIR) + "/" + id + ".json";
}

// ─────────────────────────────────────────────────────────────
//  RuleEntry JSON serialisation
// ─────────────────────────────────────────────────────────────
void RuleEntry::toJson(JsonObject obj) const {
    obj["id"]           = id;
    obj["name"]         = name;
    obj["enabled"]      = enabled;
    obj["triggerParam"] = triggerParam;
    obj["firedCount"]   = firedCount;
    obj["lastFiredAt"]  = lastFiredAt;
    // trigger string
    const char* tstr = "MANUAL";
    switch (trigger) {
        case RuleTrigger::RFID_SCAN:       tstr = "RFID_SCAN";       break;
        case RuleTrigger::RFID_UNKNOWN:    tstr = "RFID_UNKNOWN";    break;
        case RuleTrigger::NFC_SCAN:        tstr = "NFC_SCAN";        break;
        case RuleTrigger::IR_RECEIVED:     tstr = "IR_RECEIVED";     break;
        case RuleTrigger::WIFI_CONNECT:    tstr = "WIFI_CONNECT";    break;
        case RuleTrigger::WIFI_DISCONNECT: tstr = "WIFI_DISCONNECT"; break;
        case RuleTrigger::BOOT:            tstr = "BOOT";            break;
        default:                           tstr = "MANUAL";          break;
    }
    obj["trigger"] = tstr;
    JsonArray acts = obj["actions"].to<JsonArray>();
    for (const auto& s : actions) {
        JsonObject a = acts.add<JsonObject>();
        a["delayMs"] = s.delayMs;
        a["param1"]  = s.param1;
        a["param2"]  = s.param2;
        const char* astr = "LOG";
        switch (s.action) {
            case RuleAction::IR_TRANSMIT:     astr = "IR_TRANSMIT";     break;
            case RuleAction::MACRO_RUN:       astr = "MACRO_RUN";       break;
            case RuleAction::NOTIFY:          astr = "NOTIFY";          break;
            case RuleAction::NOTIFY_TELEGRAM: astr = "NOTIFY_TELEGRAM"; break;
            case RuleAction::NOTIFY_WHATSAPP: astr = "NOTIFY_WHATSAPP"; break;
            case RuleAction::BUZZER:          astr = "BUZZER";          break;
            default:                          astr = "LOG";             break;
        }
        a["action"] = astr;
    }
}

String RuleEntry::toJsonString() const {
    // Build manually to avoid circular dependency with ruleMgr methods
    String out = "{";
    out += "\"id\":" + String(id) + ",";
    out += "\"name\":\"" + name + "\",";
    out += "\"enabled\":" + String(enabled ? "true" : "false") + ",";
    // Trigger
    const char* tstr = "MANUAL";
    switch (trigger) {
        case RuleTrigger::RFID_SCAN:       tstr = "RFID_SCAN";       break;
        case RuleTrigger::RFID_UNKNOWN:    tstr = "RFID_UNKNOWN";    break;
        case RuleTrigger::NFC_SCAN:        tstr = "NFC_SCAN";        break;
        case RuleTrigger::IR_RECEIVED:     tstr = "IR_RECEIVED";     break;
        case RuleTrigger::WIFI_CONNECT:    tstr = "WIFI_CONNECT";    break;
        case RuleTrigger::WIFI_DISCONNECT: tstr = "WIFI_DISCONNECT"; break;
        case RuleTrigger::BOOT:            tstr = "BOOT";            break;
        case RuleTrigger::MANUAL:          tstr = "MANUAL";          break;
        default:                           tstr = "MANUAL";          break;
    }
    out += "\"trigger\":\"" + String(tstr) + "\",";
    out += "\"triggerParam\":\"" + triggerParam + "\",";
    out += "\"firedCount\":" + String(firedCount) + ",";
    out += "\"lastFiredAt\":\"" + lastFiredAt + "\",";
    out += "\"actions\":[";
    for (size_t i = 0; i < actions.size(); i++) {
        if (i) out += ",";
        const char* astr = "LOG";
        switch (actions[i].action) {
            case RuleAction::IR_TRANSMIT:     astr = "IR_TRANSMIT";     break;
            case RuleAction::MACRO_RUN:       astr = "MACRO_RUN";       break;
            case RuleAction::NOTIFY:          astr = "NOTIFY";          break;
            case RuleAction::NOTIFY_TELEGRAM: astr = "NOTIFY_TELEGRAM"; break;
            case RuleAction::NOTIFY_WHATSAPP: astr = "NOTIFY_WHATSAPP"; break;
            case RuleAction::BUZZER:          astr = "BUZZER";          break;
            case RuleAction::LOG:             astr = "LOG";             break;
            default:                          astr = "LOG";             break;
        }
        String p1 = actions[i].param1; p1.replace("\"","\\\"");
        String p2 = actions[i].param2; p2.replace("\"","\\\"");
        out += "{\"action\":\"" + String(astr) + "\","
             + "\"param1\":\"" + p1 + "\","
             + "\"param2\":\"" + p2 + "\","
             + "\"delayMs\":" + String(actions[i].delayMs) + "}";
    }
    out += "]}";
    return out;
}

bool RuleEntry::fromJson(JsonObjectConst obj) {
    id           = obj["id"]           | (uint32_t)0;
    name         = obj["name"]         | (const char*)"";
    enabled      = obj["enabled"]      | true;
    triggerParam = obj["triggerParam"] | (const char*)"";
    firedCount   = obj["firedCount"]   | (uint32_t)0;
    lastFiredAt  = obj["lastFiredAt"]  | (const char*)"";

    // Parse trigger
    String ts = obj["trigger"] | (const char*)"MANUAL";
    ts.toUpperCase();
    if      (ts == "RFID_SCAN")       trigger = RuleTrigger::RFID_SCAN;
    else if (ts == "RFID_UNKNOWN")    trigger = RuleTrigger::RFID_UNKNOWN;
    else if (ts == "NFC_SCAN")        trigger = RuleTrigger::NFC_SCAN;
    else if (ts == "IR_RECEIVED")     trigger = RuleTrigger::IR_RECEIVED;
    else if (ts == "WIFI_CONNECT")    trigger = RuleTrigger::WIFI_CONNECT;
    else if (ts == "WIFI_DISCONNECT") trigger = RuleTrigger::WIFI_DISCONNECT;
    else if (ts == "BOOT")            trigger = RuleTrigger::BOOT;
    else                              trigger = RuleTrigger::MANUAL;

    // Parse actions
    actions.clear();
    for (JsonObjectConst a : obj["actions"].as<JsonArrayConst>()) {
        RuleActionStep step;
        step.delayMs = a["delayMs"] | (uint32_t)0;
        step.param1  = a["param1"] | (const char*)"";
        step.param2  = a["param2"] | (const char*)"";
        String as    = a["action"] | (const char*)"LOG";
        as.toUpperCase();
        if      (as == "IR_TRANSMIT")     step.action = RuleAction::IR_TRANSMIT;
        else if (as == "MACRO_RUN")       step.action = RuleAction::MACRO_RUN;
        else if (as == "NOTIFY")          step.action = RuleAction::NOTIFY;
        else if (as == "NOTIFY_TELEGRAM") step.action = RuleAction::NOTIFY_TELEGRAM;
        else if (as == "NOTIFY_WHATSAPP") step.action = RuleAction::NOTIFY_WHATSAPP;
        else if (as == "BUZZER")          step.action = RuleAction::BUZZER;
        else                              step.action = RuleAction::LOG;
        actions.push_back(step);
    }
    return id > 0 && !name.isEmpty();
}

// ─────────────────────────────────────────────────────────────
//  String conversion helpers
// ─────────────────────────────────────────────────────────────
String RuleManager::_triggerToStr(RuleTrigger t) const {
    switch (t) {
        case RuleTrigger::RFID_SCAN:       return "RFID_SCAN";
        case RuleTrigger::RFID_UNKNOWN:    return "RFID_UNKNOWN";
        case RuleTrigger::NFC_SCAN:        return "NFC_SCAN";
        case RuleTrigger::IR_RECEIVED:     return "IR_RECEIVED";
        case RuleTrigger::WIFI_CONNECT:    return "WIFI_CONNECT";
        case RuleTrigger::WIFI_DISCONNECT: return "WIFI_DISCONNECT";
        case RuleTrigger::BOOT:            return "BOOT";
        case RuleTrigger::MANUAL:          return "MANUAL";
        default:                           return "MANUAL";
    }
}

String RuleManager::_actionToStr(RuleAction a) const {
    switch (a) {
        case RuleAction::IR_TRANSMIT:     return "IR_TRANSMIT";
        case RuleAction::MACRO_RUN:       return "MACRO_RUN";
        case RuleAction::NOTIFY:          return "NOTIFY";
        case RuleAction::NOTIFY_TELEGRAM: return "NOTIFY_TELEGRAM";
        case RuleAction::NOTIFY_WHATSAPP: return "NOTIFY_WHATSAPP";
        case RuleAction::BUZZER:          return "BUZZER";
        case RuleAction::LOG:             return "LOG";
        default:                          return "LOG";
    }
}

String RuleManager::_buildTimeStr() const {
    time_t now; time(&now);
    if (now > 1000000000UL) {
        struct tm t; localtime_r(&now, &t);
        char buf[24];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                 t.tm_year+1900, t.tm_mon+1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
        return String(buf);
    }
    return String("uptime:") + (millis()/1000) + "s";
}
