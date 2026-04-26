// ============================================================
//  web_server_ai.cpp  —  AI Assistant API Routes v1.0
//
//  Endpoints:
//    GET  /api/v1/ai/status          — provider, model, key status
//    POST /api/v1/ai/chat            — send message, get reply
//    POST /api/v1/ai/config          — set provider, model, API key
//    POST /api/v1/ai/clear-history   — wipe conversation
//    GET  /api/v1/ai/history         — conversation history JSON
//    GET  /api/v1/ai/providers       — list all supported providers/models
//
//  All API keys stored in NVS (hardware-encrypted).
//  Keys are NEVER returned in GET responses — only hasKey boolean.
//  Chat runs in a FreeRTOS task so loop() is not blocked.
// ============================================================
#include "web_server.h"
#include "ai_assistant.h"
#include "auth_manager.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

// ── Local helpers ─────────────────────────────────────────
static void aiSend(AsyncWebServerRequest* req, int code, const String& json) {
    AsyncWebServerResponse* r = req->beginResponse(code, "application/json", json);
    r->addHeader("Access-Control-Allow-Origin", "*");
    req->send(r);
}

static void aiSendOk(AsyncWebServerRequest* req, const String& extra = "") {
    aiSend(req, 200, extra.isEmpty() ? "{\"ok\":true}" : extra);
}

// ── Self-contained body buffer helpers (static, TU-local) ─
static String* _aiGetBuf(AsyncWebServerRequest* req) {
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
static void _aiFreeBuf(AsyncWebServerRequest* req) {
    if (req->_tempObject) {
        delete reinterpret_cast<String*>(req->_tempObject);
        req->_tempObject = nullptr;
    }
}

#define AI_POST(path, handler) \
    _server.on(path, HTTP_POST, \
        [](AsyncWebServerRequest* req){}, \
        nullptr, \
        [this](AsyncWebServerRequest* req, uint8_t* d, size_t l, size_t i, size_t t) { \
            if (_aiGetBuf(req)->length() + l > HTTP_MAX_BODY) { \
                _aiFreeBuf(req); \
                aiSend(req, 413, "{\"error\":\"Request too large\"}"); return; \
            } \
            _aiGetBuf(req)->concat((char*)d, l); \
            bool last = (t > 0) ? (i + l >= t) : (i == 0); \
            if (last) { \
                String* buf = _aiGetBuf(req); \
                handler(req, (uint8_t*)buf->c_str(), buf->length()); \
                _aiFreeBuf(req); \
            } \
        })

// ── setupAiRoutes ─────────────────────────────────────────
void WebUI::setupAiRoutes() {

    // ── GET /api/v1/ai/status ─────────────────────────────
    // Returns provider, model, hasKey booleans — NOT the actual keys.
    _server.on("/api/v1/ai/status", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            aiSend(req, 200, aiAssistant.statusJson());
        });

    // ── GET /api/v1/ai/providers ──────────────────────────
    // Returns all supported providers and their available models.
    _server.on("/api/v1/ai/providers", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            JsonDocument doc;

            JsonArray providers = doc["providers"].to<JsonArray>();

            // OpenAI
            JsonObject oai = providers.add<JsonObject>();
            oai["id"]      = "openai";
            oai["name"]    = "OpenAI";
            oai["keyHint"] = "sk-...";
            oai["keyUrl"]  = "https://platform.openai.com/api-keys";
            JsonArray oaiModels = oai["models"].to<JsonArray>();
            for (const char* m : {"gpt-4o", "gpt-4o-mini", "gpt-4-turbo", "gpt-3.5-turbo"})
                oaiModels.add(m);

            // Anthropic
            JsonObject anth = providers.add<JsonObject>();
            anth["id"]      = "anthropic";
            anth["name"]    = "Anthropic (Claude)";
            anth["keyHint"] = "sk-ant-...";
            anth["keyUrl"]  = "https://console.anthropic.com/settings/keys";
            JsonArray anthModels = anth["models"].to<JsonArray>();
            for (const char* m : {
                "claude-3-5-sonnet-20241022",
                "claude-3-5-haiku-20241022",
                "claude-3-opus-20240229",
                "claude-3-haiku-20240307"}) anthModels.add(m);

            // Google Gemini
            JsonObject gem = providers.add<JsonObject>();
            gem["id"]      = "gemini";
            gem["name"]    = "Google Gemini";
            gem["keyHint"] = "AIza...";
            gem["keyUrl"]  = "https://aistudio.google.com/app/apikey";
            JsonArray gemModels = gem["models"].to<JsonArray>();
            for (const char* m : {
                "gemini-1.5-flash",
                "gemini-1.5-flash-8b",
                "gemini-1.5-pro",
                "gemini-2.0-flash-exp"}) gemModels.add(m);

            // Mistral
            JsonObject mist = providers.add<JsonObject>();
            mist["id"]      = "mistral";
            mist["name"]    = "Mistral AI";
            mist["keyHint"] = "...";
            mist["keyUrl"]  = "https://console.mistral.ai/api-keys";
            mist["note"]    = "Use OpenAI-compatible mode — set provider=openai, model=mistral-large-latest, and point to Mistral API";

            String out; serializeJson(doc, out);
            aiSend(req, 200, out);
        });

    // ── POST /api/v1/ai/config ────────────────────────────
    // Set provider, model, API key, enable/disable.
    // Body: { "provider":"openai"|"anthropic"|"gemini",
    //         "model":"gpt-4o-mini",
    //         "apiKey":"sk-...",
    //         "enabled": true,
    //         "systemPrompt": "..." }
    AI_POST("/api/v1/ai/config",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument body;
            if (deserializeJson(body, d, l) != DeserializationError::Ok) {
                aiSend(req, 400, "{\"error\":\"Invalid JSON\"}"); return;
            }

            // Provider
            if (body["provider"].is<const char*>()) {
                String p = body["provider"].as<String>();
                AiProvider prov = (p == "openai")    ? AiProvider::OPENAI    :
                                  (p == "anthropic") ? AiProvider::ANTHROPIC :
                                  (p == "gemini")    ? AiProvider::GEMINI    :
                                                       AiProvider::NONE;
                aiAssistant.setProvider(prov);
            }

            // Model
            if (body["model"].is<const char*>()) {
                String m = body["model"].as<String>();
                m.trim();
                if (m.length() > 0) aiAssistant.setModel(m);
            }

            // API key — stored in NVS, never echoed back
            if (body["apiKey"].is<const char*>()) {
                String key = body["apiKey"].as<String>();
                key.trim();
                if (key.length() > 8) {
                    // Determine which provider this key belongs to
                    AiProvider keyProv = aiAssistant.getProvider();
                    // Allow explicit provider override for key
                    if (body["keyProvider"].is<const char*>()) {
                        String kp = body["keyProvider"].as<String>();
                        if      (kp == "openai")    keyProv = AiProvider::OPENAI;
                        else if (kp == "anthropic") keyProv = AiProvider::ANTHROPIC;
                        else if (kp == "gemini")    keyProv = AiProvider::GEMINI;
                    }
                    aiAssistant.setApiKey(keyProv, key);
                }
            }

            // Enable/disable
            if (body["enabled"].is<bool>())
                aiAssistant.setEnabled(body["enabled"].as<bool>());

            // System prompt override
            if (body["systemPrompt"].is<const char*>()) {
                String sp = body["systemPrompt"].as<String>();
                sp.trim();
                aiAssistant.setSystemPrompt(sp);
            }

            // Return status (without keys)
            aiSend(req, 200,
                String("{\"ok\":true,\"status\":") + aiAssistant.statusJson() + "}");
        }));

    // ── POST /api/v1/ai/chat ──────────────────────────────
    // Send a message to the AI. Response may include tool calls
    // that directly control the IR device.
    // Body: { "message": "Turn on the TV" }
    // Response: { "ok": true, "reply": "...", "provider": "OpenAI" }
    AI_POST("/api/v1/ai/chat",
        ([this](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            if (!authMgr.checkAuth(req)) return;
            JsonDocument body;
            if (deserializeJson(body, d, l) != DeserializationError::Ok) {
                aiSend(req, 400, "{\"error\":\"Invalid JSON\"}"); return;
            }

            String message = body["message"] | "";
            message.trim();
            if (message.isEmpty()) {
                aiSend(req, 400, "{\"error\":\"Empty message\"}"); return;
            }
            if (message.length() > 2000) message = message.substring(0, 2000);

            if (!aiAssistant.isEnabled()) {
                aiSend(req, 503, "{\"error\":\"AI is disabled\",\"hint\":\"Enable in Settings → AI Assistant\"}");
                return;
            }

            // IMPORTANT: chatBlocking() makes an HTTPS request (up to 30s).
            // Calling it directly in an AsyncWebServer handler would block the
            // TCP task and cause WDT reset. Instead we run it in a FreeRTOS task
            // and send the HTTP response from that task once complete.
            // AsyncWebServer request pointers remain valid until req->send() fires.
            struct AiChatReq {
                AsyncWebServerRequest* req;
                String message;
            };
            auto* chatReq = new AiChatReq{req, message};

            BaseType_t created = xTaskCreate(
                [](void* param) {
                    auto* cr = static_cast<AiChatReq*>(param);
                    String reply = aiAssistant.chatBlocking(cr->message);

                    bool ok = !reply.startsWith("Error:") &&
                              !reply.startsWith("No AI") &&
                              !reply.startsWith("WiFi") &&
                              !reply.startsWith("AI is") &&
                              !reply.startsWith("AI Assistant");

                    JsonDocument resp;
                    resp["ok"]       = ok;
                    resp["provider"] = aiAssistant.providerName();
                    resp["model"]    = aiAssistant.getModel();
                    if (ok) { resp["reply"] = reply; }
                    else    { resp["error"] = reply; resp["reply"] = ""; }

                    String out; serializeJson(resp, out);

                    AsyncWebServerResponse* r = cr->req->beginResponse(
                        ok ? 200 : 503, "application/json", out);
                    r->addHeader("Access-Control-Allow-Origin", "*");
                    cr->req->send(r);

                    delete cr;
                    vTaskDelete(NULL);
                },
                "ai_chat_http",
                12288,        // 12 KB stack: WiFiClientSecure(~8KB)+JSON+ArduinoJson buffers
                chatReq,
                1,
                NULL
            );

            if (created != pdPASS) {
                delete chatReq;
                aiSend(req, 503, "{\"error\":\"Failed to create AI task — low heap\"}");
            }
            // Response sent from task — do NOT call req->send() here
        }));

    // ── GET /api/v1/ai/history ────────────────────────────
    _server.on("/api/v1/ai/history", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            aiSend(req, 200,
                String("{\"history\":") + aiAssistant.historyJson() + "}");
        });

    // ── POST /api/v1/ai/clear-history ─────────────────────
    _server.on("/api/v1/ai/clear-history", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!authMgr.checkAuth(req)) return;
            aiAssistant.clearHistory();
            aiSendOk(req, "{\"ok\":true,\"note\":\"Conversation history cleared\"}");
        });

    Serial.println(DEBUG_TAG " AI routes registered (/api/v1/ai/*)");
}
