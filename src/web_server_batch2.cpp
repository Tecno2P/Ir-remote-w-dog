// ============================================================
//  web_server_batch2.cpp  –  Batch 2 Routes
//
//  1. Rule/Automation Engine  (/api/v1/rules/*)
//  2. Notification Config     (/api/v1/notify/*)
// ============================================================
#include "web_server.h"
#include "rule_manager.h"
#include "notification_manager.h"
#include "audit_manager.h"
#include <ArduinoJson.h>

// ── Shared helper ─────────────────────────────────────────────
static void sendJsonB2(AsyncWebServerRequest* req, int code, const String& json) {
    AsyncWebServerResponse* r = req->beginResponse(code, "application/json", json);
    r->addHeader("Access-Control-Allow-Origin", "*");
    r->addHeader("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    r->addHeader("Access-Control-Allow-Headers", "Content-Type");
    req->send(r);
}

static String* _getB2Buf(AsyncWebServerRequest* req) {
    if (!req->_tempObject) {
        req->_tempObject = new String();
        req->onDisconnect([req]() {
            if (req->_tempObject) {
                delete reinterpret_cast<String*>(req->_tempObject);
                req->_tempObject = nullptr;
            }
        });
    }
    return reinterpret_cast<String*>(req->_tempObject);
}
static void _freeB2Buf(AsyncWebServerRequest* req) {
    if (req->_tempObject) {
        delete reinterpret_cast<String*>(req->_tempObject);
        req->_tempObject = nullptr;
    }
}

#define B2_POST(path, handler) \
    _server.on(path, HTTP_POST, \
        [](AsyncWebServerRequest* req){}, \
        nullptr, \
        [this](AsyncWebServerRequest* req, uint8_t* d, size_t l, size_t i, size_t t) { \
            if (_getB2Buf(req)->length() + l > HTTP_MAX_BODY) { \
                _freeB2Buf(req); \
                sendJsonB2(req, 413, "{\"error\":\"Request too large\"}"); return; \
            } \
            _getB2Buf(req)->concat((char*)d, l); \
            bool last = (t > 0) ? (i + l >= t) : (i == 0); \
            if (last) { \
                String* buf = _getB2Buf(req); \
                handler(req, (uint8_t*)buf->c_str(), buf->length()); \
                _freeB2Buf(req); \
            } \
        })

// ─────────────────────────────────────────────────────────────
// ══ SECTION 1: RULE ENGINE ROUTES ════════════════════════════
// ─────────────────────────────────────────────────────────────
void WebUI::setupRuleRoutes() {
    // GET  /api/v1/rules          — list all rules
    _server.on("/api/v1/rules", HTTP_GET,
        [this](AsyncWebServerRequest* req) {
            auditMgr.logApi("/api/v1/rules", "GET");
            sendJsonB2(req, 200, ruleMgr.allRulesToJson());
        });

    // POST /api/v1/rules          — create new rule
    B2_POST("/api/v1/rules",
        [this](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            handleRuleCreate(req, d, l); });

    // POST /api/v1/rules/update   — update existing rule
    B2_POST("/api/v1/rules/update",
        [this](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            handleRuleUpdate(req, d, l); });

    // GET  /api/v1/rules/delete?id=X
    _server.on("/api/v1/rules/delete", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleRuleDelete(req); });

    // GET  /api/v1/rules/toggle?id=X&enabled=true
    _server.on("/api/v1/rules/toggle", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleRuleToggle(req); });

    // GET  /api/v1/rules/fire?id=X  — manually trigger a rule
    _server.on("/api/v1/rules/fire", HTTP_GET,
        [this](AsyncWebServerRequest* req) { handleRuleFire(req); });

    // GET  /api/v1/rules/triggers  — list available trigger types
    _server.on("/api/v1/rules/triggers", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            const char* json =
                "{\"triggers\":["
                "\"RFID_SCAN\",\"RFID_UNKNOWN\",\"NFC_SCAN\","
                "\"IR_RECEIVED\",\"WIFI_CONNECT\",\"WIFI_DISCONNECT\","
                "\"BOOT\",\"MANUAL\""
                "]}";
            AsyncWebServerResponse* r = req->beginResponse(200, "application/json", json);
            r->addHeader("Access-Control-Allow-Origin", "*");
            req->send(r);
        });

    // GET  /api/v1/rules/actions   — list available action types
    _server.on("/api/v1/rules/actions", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            const char* json =
                "{\"actions\":["
                "\"IR_TRANSMIT\",\"MACRO_RUN\","
                "\"NOTIFY\",\"NOTIFY_TELEGRAM\",\"NOTIFY_WHATSAPP\","
                "\"BUZZER\",\"LOG\""
                "]}";
            AsyncWebServerResponse* r = req->beginResponse(200, "application/json", json);
            r->addHeader("Access-Control-Allow-Origin", "*");
            req->send(r);
        });

    Serial.println("[WEB] Rule Engine routes registered");
}

// ─────────────────────────────────────────────────────────────
// POST /api/v1/rules
// Body: {
//   "name": "Unknown RFID Alert",
//   "trigger": "RFID_UNKNOWN",
//   "triggerParam": "",
//   "actions": [
//     {"action": "NOTIFY", "param1": "Unknown card scanned!", "delayMs": 0},
//     {"action": "BUZZER", "param1": "3", "delayMs": 0}
//   ]
// }
// ─────────────────────────────────────────────────────────────
void WebUI::handleRuleCreate(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    JsonDocument body;
    if (deserializeJson(body, d, l) != DeserializationError::Ok) {
        sendJsonB2(req, 400, "{\"error\":\"Invalid JSON\"}"); return;
    }

    RuleEntry rule;
    if (!rule.fromJson(body.as<JsonObjectConst>())) {
        sendJsonB2(req, 400, "{\"error\":\"Missing required fields (name, trigger, actions)\"}");
        return;
    }
    rule.id = 0;  // force new ID assignment

    uint32_t newId = ruleMgr.addRule(rule);
    if (!newId) {
        sendJsonB2(req, 507, "{\"error\":\"Max rules reached\"}"); return;
    }

    auditMgr.log(AuditSource::RULE, "RULE_CREATED",
                 String("Rule: ") + rule.name + " (id=" + newId + ")");

    sendJsonB2(req, 200,
        "{\"ok\":true,\"id\":" + String(newId) + ",\"name\":\"" + rule.name + "\"}");
}

// ─────────────────────────────────────────────────────────────
// POST /api/v1/rules/update
// Body: same as create but must include "id"
// ─────────────────────────────────────────────────────────────
void WebUI::handleRuleUpdate(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    JsonDocument body;
    if (deserializeJson(body, d, l) != DeserializationError::Ok) {
        sendJsonB2(req, 400, "{\"error\":\"Invalid JSON\"}"); return;
    }
    RuleEntry rule;
    if (!rule.fromJson(body.as<JsonObjectConst>()) || rule.id == 0) {
        sendJsonB2(req, 400, "{\"error\":\"Missing id\"}"); return;
    }
    if (!ruleMgr.updateRule(rule)) {
        sendJsonB2(req, 404, "{\"error\":\"Rule not found\"}"); return;
    }
    auditMgr.log(AuditSource::RULE, "RULE_UPDATED",
                 String("Rule: ") + rule.name + " (id=" + rule.id + ")");
    sendJsonB2(req, 200, "{\"ok\":true}");
}

// ─────────────────────────────────────────────────────────────
// GET /api/v1/rules/delete?id=X
// ─────────────────────────────────────────────────────────────
void WebUI::handleRuleDelete(AsyncWebServerRequest* req) {
    if (!req->hasParam("id")) {
        sendJsonB2(req, 400, "{\"error\":\"Missing id\"}"); return;
    }
    uint32_t id = req->getParam("id")->value().toInt();
    if (!ruleMgr.deleteRule(id)) {
        sendJsonB2(req, 404, "{\"error\":\"Rule not found\"}"); return;
    }
    auditMgr.log(AuditSource::RULE, "RULE_DELETED", String("id=") + id);
    sendJsonB2(req, 200, "{\"ok\":true}");
}

// ─────────────────────────────────────────────────────────────
// GET /api/v1/rules/toggle?id=X&enabled=true
// ─────────────────────────────────────────────────────────────
void WebUI::handleRuleToggle(AsyncWebServerRequest* req) {
    if (!req->hasParam("id")) {
        sendJsonB2(req, 400, "{\"error\":\"Missing id\"}"); return;
    }
    uint32_t id = req->getParam("id")->value().toInt();
    bool en = true;
    if (req->hasParam("enabled")) {
        en = req->getParam("enabled")->value() != "false";
    }
    if (!ruleMgr.setEnabled(id, en)) {
        sendJsonB2(req, 404, "{\"error\":\"Rule not found\"}"); return;
    }
    sendJsonB2(req, 200,
        "{\"ok\":true,\"id\":" + String(id) +
        ",\"enabled\":" + (en ? "true" : "false") + "}");
}

// ─────────────────────────────────────────────────────────────
// GET /api/v1/rules/fire?id=X
// ─────────────────────────────────────────────────────────────
void WebUI::handleRuleFire(AsyncWebServerRequest* req) {
    if (!req->hasParam("id")) {
        sendJsonB2(req, 400, "{\"error\":\"Missing id\"}"); return;
    }
    uint32_t id = req->getParam("id")->value().toInt();
    ruleMgr.triggerManual(id);
    auditMgr.log(AuditSource::RULE, "RULE_MANUAL_FIRE", String("id=") + id);
    sendJsonB2(req, 200, "{\"ok\":true,\"fired\":" + String(id) + "}");
}

// ─────────────────────────────────────────────────────────────
// ══ SECTION 2: NOTIFICATION ROUTES ═══════════════════════════
// ─────────────────────────────────────────────────────────────
void WebUI::setupNotifyRoutes() {
    // GET  /api/v1/notify/config  — read notification config
    _server.on("/api/v1/notify/config", HTTP_GET,
        [this](AsyncWebServerRequest* req) {
            auditMgr.logApi("/api/v1/notify/config", "GET");
            sendJsonB2(req, 200, notifyMgr.configToJson());
        });

    // POST /api/v1/notify/config  — save config
    B2_POST("/api/v1/notify/config",
        [this](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            handleNotifyConfigSave(req, d, l); });

    // POST /api/v1/notify/test    — send test message
    B2_POST("/api/v1/notify/test",
        [this](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            handleNotifyTest(req, d, l); });

    // GET  /api/v1/notify/queue   — queue status
    _server.on("/api/v1/notify/queue", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            String json = "{\"queued\":" + String(notifyMgr.queueSize())
                        + ",\"sending\":" + (notifyMgr.isSending() ? "true" : "false")
                        + ",\"sentTotal\":" + notifyMgr.sentTotal()
                        + ",\"failedTotal\":" + notifyMgr.failedTotal()
                        + ",\"lastError\":\"" + notifyMgr.lastError() + "\"}";
            AsyncWebServerResponse* r = req->beginResponse(200, "application/json", json);
            r->addHeader("Access-Control-Allow-Origin", "*");
            req->send(r);
        });

    Serial.println("[WEB] Notification routes registered");
}

// ─────────────────────────────────────────────────────────────
// POST /api/v1/notify/config
// Body: { "telegramEnabled": true, "telegramToken": "...", ... }
// ─────────────────────────────────────────────────────────────
void WebUI::handleNotifyConfigSave(AsyncWebServerRequest* req,
                                    uint8_t* d, size_t l) {
    if (!notifyMgr.configFromJson(d, l)) {
        sendJsonB2(req, 400, "{\"error\":\"Invalid JSON\"}"); return;
    }
    if (!notifyMgr.saveConfig()) {
        sendJsonB2(req, 500, "{\"error\":\"Failed to save config\"}"); return;
    }
    auditMgr.log(AuditSource::SYSTEM, "NOTIFY_CONFIG_SAVED", "Notification config updated");
    sendJsonB2(req, 200, "{\"ok\":true}");
}

// ─────────────────────────────────────────────────────────────
// POST /api/v1/notify/test
// Body: {} or {"message": "custom test msg"}
// ─────────────────────────────────────────────────────────────
void WebUI::handleNotifyTest(AsyncWebServerRequest* req, uint8_t* d, size_t l) {
    String err;
    bool ok = notifyMgr.sendTest(err);
    if (!ok) {
        sendJsonB2(req, 500,
            "{\"ok\":false,\"error\":\"" + err + "\","
            "\"hint\":\"Make sure WiFi STA is connected and credentials are correct\"}");
        return;
    }
    sendJsonB2(req, 200, "{\"ok\":true,\"message\":\"Test notification sent\"}");
}
