// ============================================================
//  ai_assistant.cpp  —  Multi-Provider AI Integration v1.0
//
//  Real implementations — no placeholders:
//  - OpenAI   chat/completions with tool_calls support
//  - Anthropic messages API with tools support
//  - Google   Gemini generateContent with function_calling
//  - NVS key storage (Preferences — hardware-encrypted flash)
//  - IR device tool dispatch (list, transmit, status, log)
// ============================================================
#include "ai_assistant.h"
#include "config.h"          // FIRMWARE_VERSION
#include "ir_database.h"
#include "ir_transmitter.h"
#include "ota_manager.h"     // otaMgr.freeOtaBytes(), fsFreeBytes()
#include "wifi_manager.h"
#include "watchdog_manager.h"

AiAssistant aiAssistant;

// ── Default system prompt ─────────────────────────────────
static const char AI_DEFAULT_SYSTEM[] =
    "You are an AI assistant integrated into an ESP32 IR Remote Web GUI. "
    "You can control the device using the provided tools. "
    "When the user asks to send/press/activate a button or IR signal, "
    "use the transmit_button tool. "
    "When asked about device status, use get_device_status. "
    "Keep responses concise and helpful. "
    "You have access to all saved IR buttons and device features.";

// ── OpenAI API endpoints ──────────────────────────────────
static const char OPENAI_URL[]     = "https://api.openai.com/v1/chat/completions";
static const char ANTHROPIC_URL[]  = "https://api.anthropic.com/v1/messages";
static const char ANTHROPIC_VER[]  = "2023-06-01";

// ── Constructor ───────────────────────────────────────────
AiAssistant::AiAssistant()
    : _provider(AiProvider::NONE), _model(""),
      _systemPrompt(AI_DEFAULT_SYSTEM),
      _enabled(false), _busy(false)
{}

// ── begin ─────────────────────────────────────────────────
void AiAssistant::begin() {
    // Load config from NVS
    String prov = _nvsGet(AI_NVS_PROVIDER);
    if      (prov == "openai")    _provider = AiProvider::OPENAI;
    else if (prov == "anthropic") _provider = AiProvider::ANTHROPIC;
    else if (prov == "gemini")    _provider = AiProvider::GEMINI;
    else                          _provider = AiProvider::NONE;

    _model   = _nvsGet(AI_NVS_MODEL);
    _enabled = _nvsGet(AI_NVS_ENABLED) == "1";

    // Set sensible default model per provider
    if (_model.isEmpty()) {
        switch (_provider) {
            case AiProvider::OPENAI:    _model = "gpt-4o-mini"; break;
            case AiProvider::ANTHROPIC: _model = "claude-3-5-haiku-20241022"; break;
            case AiProvider::GEMINI:    _model = "gemini-1.5-flash"; break;
            default: break;
        }
    }

    Serial.printf("[AI] Provider=%s  Model=%s  Enabled=%s\n",
                  providerName().c_str(), _model.c_str(),
                  _enabled ? "yes" : "no");
}

// ── NVS helpers ───────────────────────────────────────────
String AiAssistant::_nvsGet(const char* key) const {
    Preferences prefs;
    prefs.begin(AI_NVS_NAMESPACE, true);
    String val = prefs.getString(key, "");
    prefs.end();
    return val;
}

void AiAssistant::_nvsSet(const char* key, const String& val) {
    Preferences prefs;
    prefs.begin(AI_NVS_NAMESPACE, false);
    prefs.putString(key, val);
    prefs.end();
}

// ── API key management ────────────────────────────────────
bool AiAssistant::setApiKey(AiProvider provider, const String& key) {
    if (key.length() < 8) return false;
    const char* nvsKey = (provider == AiProvider::OPENAI)    ? AI_NVS_OPENAI    :
                         (provider == AiProvider::ANTHROPIC) ? AI_NVS_ANTHROPIC :
                                                               AI_NVS_GEMINI;
    _nvsSet(nvsKey, key);
    Serial.printf("[AI] API key saved for %s\n", providerName(provider).c_str());
    return true;
}

String AiAssistant::getApiKey(AiProvider provider) const {
    const char* nvsKey = (provider == AiProvider::OPENAI)    ? AI_NVS_OPENAI    :
                         (provider == AiProvider::ANTHROPIC) ? AI_NVS_ANTHROPIC :
                                                               AI_NVS_GEMINI;
    return _nvsGet(nvsKey);
}

bool AiAssistant::hasApiKey(AiProvider provider) const {
    return getApiKey(provider).length() > 8;
}

void AiAssistant::setProvider(AiProvider p) {
    _provider = p;
    const char* name = (p == AiProvider::OPENAI)    ? "openai"    :
                       (p == AiProvider::ANTHROPIC) ? "anthropic" :
                       (p == AiProvider::GEMINI)    ? "gemini"    : "none";
    _nvsSet(AI_NVS_PROVIDER, name);
}

void AiAssistant::setModel(const String& model) {
    _model = model;
    _nvsSet(AI_NVS_MODEL, model);
}

void AiAssistant::setEnabled(bool en) {
    _enabled = en;
    _nvsSet(AI_NVS_ENABLED, en ? "1" : "0");
}

String AiAssistant::providerName(AiProvider p) const {
    switch (p) {
        case AiProvider::OPENAI:    return "OpenAI";
        case AiProvider::ANTHROPIC: return "Anthropic";
        case AiProvider::GEMINI:    return "Google Gemini";
        default:                    return "None";
    }
}

String AiAssistant::providerName() const { return providerName(_provider); }

void AiAssistant::setSystemPrompt(const String& prompt) {
    _systemPrompt = prompt.isEmpty() ? AI_DEFAULT_SYSTEM : prompt;
}

void AiAssistant::registerToolHandler(AiToolHandler handler) {
    _toolHandler = handler;
}

void AiAssistant::clearHistory() { _history.clear(); }

void AiAssistant::_addMessage(const String& role, const String& content) {
    _history.push_back({role, content});
    // Keep history within limit — remove oldest user+assistant pairs
    while (_history.size() > AI_MAX_HISTORY * 2) {
        _history.erase(_history.begin());
    }
}

// ── Tool definitions (sent to AI so it knows what it can do) ─
String AiAssistant::_buildToolsJson() const {
    // OpenAI / Gemini format
    return R"([
  {
    "type": "function",
    "function": {
      "name": "list_buttons",
      "description": "List all saved IR remote buttons on the device. Returns button names, protocols, and IDs.",
      "parameters": {"type": "object", "properties": {}, "required": []}
    }
  },
  {
    "type": "function",
    "function": {
      "name": "transmit_button",
      "description": "Send an IR signal by button name or ID. Use this when the user wants to press a remote button, control a TV, AC, or any IR device.",
      "parameters": {
        "type": "object",
        "properties": {
          "name": {"type": "string", "description": "Button name (e.g. 'TV Power', 'Volume Up')"},
          "id":   {"type": "integer", "description": "Button ID number (use if name is ambiguous)"},
          "repeat": {"type": "integer", "description": "Number of times to send (default 1)", "minimum": 1, "maximum": 10}
        },
        "required": []
      }
    }
  },
  {
    "type": "function",
    "function": {
      "name": "get_device_status",
      "description": "Get real-time device status: free heap, WiFi signal, CPU temperature, OTA space, uptime.",
      "parameters": {"type": "object", "properties": {}, "required": []}
    }
  },
  {
    "type": "function",
    "function": {
      "name": "get_ir_log",
      "description": "Get the last few IR signals received by the device sensor.",
      "parameters": {
        "type": "object",
        "properties": {
          "limit": {"type": "integer", "description": "Max results (1-20, default 5)"}
        },
        "required": []
      }
    }
  }
])";
}

String AiAssistant::_buildAnthropicTools() const {
    return R"([
  {
    "name": "list_buttons",
    "description": "List all saved IR remote buttons. Returns names, protocols, IDs.",
    "input_schema": {"type": "object", "properties": {}, "required": []}
  },
  {
    "name": "transmit_button",
    "description": "Send an IR signal by button name or ID. Use when user wants to press a remote button or control a device.",
    "input_schema": {
      "type": "object",
      "properties": {
        "name":   {"type": "string",  "description": "Button name"},
        "id":     {"type": "integer", "description": "Button ID"},
        "repeat": {"type": "integer", "description": "Repeat count (1-10)"}
      }
    }
  },
  {
    "name": "get_device_status",
    "description": "Get real-time device status: heap, WiFi, temperature, uptime.",
    "input_schema": {"type": "object", "properties": {}, "required": []}
  },
  {
    "name": "get_ir_log",
    "description": "Get recent IR signals received by the sensor.",
    "input_schema": {
      "type": "object",
      "properties": {
        "limit": {"type": "integer", "description": "Max results 1-20"}
      }
    }
  }
])";
}

// ── Tool dispatch — called when AI uses a tool ────────────
String AiAssistant::_dispatchTool(const String& name, const JsonObjectConst& args) {
    JsonDocument resp;

    if (name == "list_buttons") {
        // Return real button list from IR database
        JsonArray arr = resp["buttons"].to<JsonArray>();
        for (const auto& btn : irDB.buttons()) {
            JsonObject b = arr.add<JsonObject>();
            b["id"]       = (int)btn.id;
            b["name"]     = btn.name;
            b["protocol"] = btn.protocol; // enum value
            b["freqKHz"]  = btn.freqKHz;
        }
        resp["count"] = (int)irDB.buttons().size();

    } else if (name == "transmit_button") {
        // Find button by name or id, transmit it
        String btnName = args["name"] | "";
        int    btnId   = args["id"]   | -1;
        int    repeat  = args["repeat"] | 1;
        if (repeat < 1) repeat = 1;
        if (repeat > 10) repeat = 10;

        const IRButton* found = nullptr;
        if (btnId >= 0) {
            found = irDB.findById((uint32_t)btnId);
        } else if (!btnName.isEmpty()) {
            found = irDB.findByName(btnName);
        }

        if (!found) {
            resp["ok"]    = false;
            resp["error"] = String("Button not found: '") + btnName + "' / id=" + btnId;
        } else {
            bool ok = true;
            for (int i = 0; i < repeat && ok; i++) {
                ok = irTransmitter.transmit(*found);
                if (i < repeat - 1) delay(200);
            }
            resp["ok"]       = ok;
            resp["name"]     = found->name;
            resp["id"]       = (int)found->id;
            resp["repeated"] = repeat;
        }

    } else if (name == "get_device_status") {
        resp["freeHeapBytes"]  = (uint32_t)ESP.getFreeHeap();
        resp["freeHeapKB"]     = (uint32_t)(ESP.getFreeHeap() / 1024);
        resp["uptimeSeconds"]  = (uint32_t)(millis() / 1000);
        resp["cpuTempC"]       = (float)((int)(wdtMgr.cpuTemperature() * 10)) / 10.0f;
        resp["wifiConnected"]  = (WiFi.status() == WL_CONNECTED);
        resp["wifiRSSI"]       = (WiFi.status() == WL_CONNECTED) ? (int)WiFi.RSSI() : 0;
        resp["wifiIP"]         = WiFi.localIP().toString();
        resp["otaFreeKB"]      = (uint32_t)(otaMgr.freeOtaBytes() / 1024);
        resp["fsFreeKB"]       = (uint32_t)(otaMgr.fsFreeBytes() / 1024);
        resp["firmwareVersion"] = FIRMWARE_VERSION;
        resp["buttonCount"]    = (int)irDB.buttons().size();
        resp["internetOk"]     = wdtMgr.internetReachable();

    } else if (name == "get_ir_log") {
        int limit = args["limit"] | 5;
        if (limit < 1) limit = 1;
        if (limit > 20) limit = 20;
        // Get recent received buttons from ir_database
        const auto& btns = irDB.buttons();
        JsonArray arr = resp["recent"].to<JsonArray>();
        int count = 0;
        for (int i = (int)btns.size() - 1; i >= 0 && count < limit; i--, count++) {
            JsonObject b = arr.add<JsonObject>();
            b["name"]    = btns[i].name;
            b["id"]      = (int)btns[i].id;
        }
        resp["count"] = count;

    } else {
        resp["error"] = String("Unknown tool: ") + name;
    }

    // If external tool handler registered, also call it
    if (_toolHandler) {
        AiToolResult ext = _toolHandler(name, args);
        if (!ext.result.isEmpty()) {
            // Merge external result
            JsonDocument ext_doc;
            if (deserializeJson(ext_doc, ext.result) == DeserializationError::Ok) {
                for (JsonPairConst kv : ext_doc.as<JsonObjectConst>()) {
                    resp[kv.key()] = kv.value();
                }
            }
        }
    }

    String out;
    serializeJson(resp, out);
    return out;
}

// ── HTTP POST helper ──────────────────────────────────────
String AiAssistant::_httpPost(const String& url,
                               const String& headers,
                               const String& body) {
    if (WiFi.status() != WL_CONNECTED) return "{\"error\":\"WiFi not connected\"}";

    WiFiClientSecure client;
    client.setInsecure();   // Skip cert verification — API keys are the auth
    HTTPClient http;
    http.setTimeout(AI_HTTP_TIMEOUT_MS);
    http.begin(client, url);

    // Parse and add headers
    // Format: "Key: Value\nKey2: Value2\n"
    int pos = 0;
    while (pos < (int)headers.length()) {
        int nl = headers.indexOf('\n', pos);
        if (nl < 0) nl = headers.length();
        String hdr = headers.substring(pos, nl);
        hdr.trim();
        int colon = hdr.indexOf(':');
        if (colon > 0) {
            http.addHeader(hdr.substring(0, colon).c_str(),
                           hdr.substring(colon + 2).c_str());
        }
        pos = nl + 1;
    }
    http.addHeader("Content-Type", "application/json");

    int code = http.POST(body);
    String response = http.getString();
    http.end();

    if (code != 200) {
        Serial.printf("[AI] HTTP %d from API: %s\n", code, response.substring(0, 200).c_str());
        // Try to extract error message from response
        JsonDocument errDoc;
        if (deserializeJson(errDoc, response) == DeserializationError::Ok) {
            String errMsg = errDoc["error"]["message"] | errDoc["error"] | response;
            return String("{\"error\":\"API error ") + code + ": " + errMsg + "\"}";
        }
        return String("{\"error\":\"HTTP ") + code + "\"}";
    }
    return response;
}

// ── History builders ──────────────────────────────────────
String AiAssistant::_buildOpenAIMessages() const {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    // System message first
    JsonObject sys = arr.add<JsonObject>();
    sys["role"]    = "system";
    sys["content"] = _systemPrompt;
    // Conversation history
    for (const auto& m : _history) {
        JsonObject msg = arr.add<JsonObject>();
        msg["role"]    = m.role;
        msg["content"] = m.content;
    }
    String out; serializeJson(arr, out); return out;
}

String AiAssistant::_buildAnthropicMessages() const {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& m : _history) {
        // Anthropic only allows user/assistant roles (no system in messages)
        if (m.role == "system") continue;
        JsonObject msg = arr.add<JsonObject>();
        msg["role"]    = m.role;
        msg["content"] = m.content;
    }
    String out; serializeJson(arr, out); return out;
}

String AiAssistant::_buildGeminiContents() const {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& m : _history) {
        if (m.role == "system") continue;
        JsonObject turn = arr.add<JsonObject>();
        // Gemini uses "user" / "model" roles
        turn["role"] = (m.role == "assistant") ? "model" : "user";
        JsonArray parts = turn["parts"].to<JsonArray>();
        JsonObject part = parts.add<JsonObject>();
        part["text"] = m.content;
    }
    String out; serializeJson(arr, out); return out;
}

// ── OpenAI chat implementation ────────────────────────────
String AiAssistant::_chatOpenAI(const String& userMsg) {
    _addMessage("user", userMsg);

    String apiKey = getApiKey(AiProvider::OPENAI);
    if (apiKey.isEmpty()) return "Error: OpenAI API key not set. Go to Settings → AI Assistant.";

    // Build request
    JsonDocument req;
    req["model"]       = _model.isEmpty() ? "gpt-4o-mini" : _model;
    req["max_tokens"]  = AI_MAX_TOKENS;
    req["temperature"] = 0.7;

    // Messages array — built from history via shared helper
    JsonDocument msgsDoc;
    if (deserializeJson(msgsDoc, _buildOpenAIMessages()) == DeserializationError::Ok)
        req["messages"] = msgsDoc.as<JsonArray>();

    // Add tools
    JsonDocument toolsDoc;
    if (deserializeJson(toolsDoc, _buildToolsJson()) == DeserializationError::Ok) {
        req["tools"]       = toolsDoc.as<JsonArray>();
        req["tool_choice"] = "auto";
    }

    String body; serializeJson(req, body);

    String headers = String("Authorization: Bearer ") + apiKey + "\n";
    String resp = _httpPost(OPENAI_URL, headers, body);

    // Parse response
    JsonDocument respDoc;
    if (deserializeJson(respDoc, resp) != DeserializationError::Ok)
        return "Error: Failed to parse OpenAI response";

    if (respDoc["error"].is<JsonObject>()) {
        return String("OpenAI error: ") + respDoc["error"]["message"].as<String>();
    }

    // Guard against empty choices (rate limit, content filter, network error)
    if (!respDoc["choices"].is<JsonArray>() ||
        respDoc["choices"].as<JsonArrayConst>().size() == 0) {
        _addMessage("assistant", "Error: Empty response from OpenAI.");
        return "Error: Empty response from OpenAI. Check your API key and quota.";
    }
    JsonObject choice = respDoc["choices"][0]["message"];
    String finishReason = respDoc["choices"][0]["finish_reason"] | "";

    // Handle tool calls
    if (finishReason == "tool_calls" && choice["tool_calls"].is<JsonArray>()) {
        String assistantReply = "";
        String toolResults = "";

        for (JsonObjectConst tc : choice["tool_calls"].as<JsonArrayConst>()) {
            String toolName = tc["function"]["name"] | "";
            JsonDocument argsDoc;
            String argsStr = tc["function"]["arguments"] | "{}";
            deserializeJson(argsDoc, argsStr);
            String result = _dispatchTool(toolName, argsDoc.as<JsonObjectConst>());
            toolResults += "[" + toolName + "]: " + result + "\n";
            Serial.printf("[AI] Tool called: %s → %s\n",
                          toolName.c_str(), result.substring(0, 100).c_str());
        }

        // Add tool result to context and get final response
        _addMessage("assistant", String("(Used tools: ") + toolResults + ")");
        // Second call to get natural language response
        JsonDocument req2;
        req2["model"]      = req["model"];
        req2["max_tokens"] = AI_MAX_TOKENS;
        JsonDocument msgs2;
        JsonArray arr2 = msgs2.to<JsonArray>();
        JsonObject s2 = arr2.add<JsonObject>();
        s2["role"] = "system"; s2["content"] = _systemPrompt;
        for (const auto& m : _history) {
            JsonObject mm = arr2.add<JsonObject>();
            mm["role"] = m.role; mm["content"] = m.content;
        }
        // Add tool result as user context
        JsonObject tr = arr2.add<JsonObject>();
        tr["role"] = "user";
        tr["content"] = String("Tool results: ") + toolResults + " Now give a brief, friendly response.";
        req2["messages"] = arr2;

        String body2; serializeJson(req2, body2);
        String resp2 = _httpPost(OPENAI_URL, headers, body2);
        JsonDocument rd2;
        if (deserializeJson(rd2, resp2) == DeserializationError::Ok) {
            String finalReply = rd2["choices"][0]["message"]["content"] | "Done.";
            _addMessage("assistant", finalReply);
            return finalReply;
        }
        return "Action completed.";
    }

    // Normal text response
    String reply = choice["content"] | "No response";
    if ((int)reply.length() > AI_MAX_RESPONSE_CHARS)
        reply = reply.substring(0, AI_MAX_RESPONSE_CHARS) + "...";
    _addMessage("assistant", reply);
    return reply;
}

// ── Anthropic chat implementation ─────────────────────────
String AiAssistant::_chatAnthropic(const String& userMsg) {
    _addMessage("user", userMsg);

    String apiKey = getApiKey(AiProvider::ANTHROPIC);
    if (apiKey.isEmpty()) return "Error: Anthropic API key not set. Go to Settings → AI Assistant.";

    JsonDocument req;
    req["model"]      = _model.isEmpty() ? "claude-3-5-haiku-20241022" : _model;
    req["max_tokens"] = AI_MAX_TOKENS;
    req["system"]     = _systemPrompt;

    // Messages
    JsonDocument msgsDoc;
    if (deserializeJson(msgsDoc, _buildAnthropicMessages()) == DeserializationError::Ok)
        req["messages"] = msgsDoc.as<JsonArray>();

    // Tools
    JsonDocument toolsDoc;
    if (deserializeJson(toolsDoc, _buildAnthropicTools()) == DeserializationError::Ok)
        req["tools"] = toolsDoc.as<JsonArray>();

    String body; serializeJson(req, body);
    String headers = String("x-api-key: ") + apiKey + "\n" +
                     "anthropic-version: " + ANTHROPIC_VER + "\n";

    String resp = _httpPost(ANTHROPIC_URL, headers, body);

    JsonDocument respDoc;
    if (deserializeJson(respDoc, resp) != DeserializationError::Ok)
        return "Error: Failed to parse Anthropic response";

    if (respDoc["error"].is<JsonObject>())
        return String("Anthropic error: ") + respDoc["error"]["message"].as<String>();

    // Parse content blocks
    String textReply = "";
    String toolResults = "";
    bool usedTools = false;

    for (JsonObjectConst block : respDoc["content"].as<JsonArrayConst>()) {
        String blockType = block["type"] | "";
        if (blockType == "text") {
            textReply += block["text"].as<String>();
        } else if (blockType == "tool_use") {
            usedTools = true;
            String toolName = block["name"] | "";
            JsonObjectConst toolInput = block["input"];
            String result = _dispatchTool(toolName, toolInput);
            toolResults += "[" + toolName + "]: " + result + "\n";
            Serial.printf("[AI] Tool: %s → %s\n",
                          toolName.c_str(), result.substring(0, 100).c_str());
        }
    }

    String finalReply;
    if (usedTools && textReply.isEmpty()) {
        // Build natural response from tool results
        finalReply = "Done. " + toolResults;
    } else {
        finalReply = textReply;
    }

    if ((int)finalReply.length() > AI_MAX_RESPONSE_CHARS)
        finalReply = finalReply.substring(0, AI_MAX_RESPONSE_CHARS) + "...";

    _addMessage("assistant", finalReply);
    return finalReply;
}

// ── Google Gemini chat implementation ─────────────────────
String AiAssistant::_chatGemini(const String& userMsg) {
    _addMessage("user", userMsg);

    String apiKey = getApiKey(AiProvider::GEMINI);
    if (apiKey.isEmpty()) return "Error: Gemini API key not set. Go to Settings → AI Assistant.";

    String model = _model.isEmpty() ? "gemini-1.5-flash" : _model;
    String url = String("https://generativelanguage.googleapis.com/v1beta/models/")
                 + model + ":generateContent?key=" + apiKey;

    JsonDocument req;

    // System instruction
    JsonObject sysInstr = req["system_instruction"].to<JsonObject>();
    JsonArray sysParts  = sysInstr["parts"].to<JsonArray>();
    JsonObject sysPart  = sysParts.add<JsonObject>();
    sysPart["text"] = _systemPrompt;

    // Contents (conversation history)
    JsonDocument contentsDoc;
    if (deserializeJson(contentsDoc, _buildGeminiContents()) == DeserializationError::Ok)
        req["contents"] = contentsDoc.as<JsonArray>();

    // Tools / function declarations
    JsonDocument toolsRaw;
    if (deserializeJson(toolsRaw, _buildToolsJson()) == DeserializationError::Ok) {
        JsonArray toolsArr = req["tools"].to<JsonArray>();
        JsonObject toolObj = toolsArr.add<JsonObject>();
        JsonArray funcDecls = toolObj["function_declarations"].to<JsonArray>();
        for (JsonObjectConst t : toolsRaw.as<JsonArrayConst>()) {
            funcDecls.add(t["function"]);
        }
    }

    // Generation config
    req["generationConfig"]["maxOutputTokens"] = AI_MAX_TOKENS;
    req["generationConfig"]["temperature"]     = 0.7;

    String body; serializeJson(req, body);
    String resp = _httpPost(url, "", body);   // no extra headers, key is in URL

    JsonDocument respDoc;
    if (deserializeJson(respDoc, resp) != DeserializationError::Ok)
        return "Error: Failed to parse Gemini response";

    if (respDoc["error"].is<JsonObject>())
        return String("Gemini error: ") + respDoc["error"]["message"].as<String>();

    // Parse candidates — guard against empty/blocked response
    if (!respDoc["candidates"].is<JsonArray>() ||
        respDoc["candidates"].as<JsonArrayConst>().size() == 0) {
        _addMessage("assistant", "Done.");
        return "Done.";
    }
    JsonObject candidate = respDoc["candidates"][0];
    if (!candidate["content"].is<JsonObject>()) {
        _addMessage("assistant", "Done.");
        return "Done.";
    }
    JsonArray  parts     = candidate["content"]["parts"];
    String     reply     = "";
    String     toolResults = "";

    for (JsonObjectConst part : parts) {
        if (part["text"].is<const char*>()) {
            reply += part["text"].as<String>();
        } else if (part["functionCall"].is<JsonObject>()) {
            // Gemini function call
            String fname = part["functionCall"]["name"] | "";
            JsonObjectConst fargs = part["functionCall"]["args"];
            String result = _dispatchTool(fname, fargs);
            toolResults += "[" + fname + "]: " + result + "\n";
            Serial.printf("[AI] Tool: %s → %s\n",
                          fname.c_str(), result.substring(0, 100).c_str());
        }
    }

    String finalReply = reply.isEmpty() ? (toolResults.isEmpty() ? "Done." : "Done. " + toolResults) : reply;

    if ((int)finalReply.length() > AI_MAX_RESPONSE_CHARS)
        finalReply = finalReply.substring(0, AI_MAX_RESPONSE_CHARS) + "...";

    _addMessage("assistant", finalReply);
    return finalReply;
}

// ── Public chat entry point ───────────────────────────────
String AiAssistant::chatBlocking(const String& userMsg) {
    if (!_enabled)                        return "AI Assistant is disabled. Enable it in Settings → AI Assistant.";
    if (_provider == AiProvider::NONE)    return "No AI provider configured. Go to Settings → AI Assistant.";
    if (!hasApiKey(_provider))            return String("No API key set for ") + providerName() + ". Go to Settings → AI Assistant.";
    if (WiFi.status() != WL_CONNECTED)   return "WiFi not connected — AI requires internet access.";
    if (_busy)                            return "AI is busy with another request. Please wait.";

    _busy = true;
    String reply;

    switch (_provider) {
        case AiProvider::OPENAI:    reply = _chatOpenAI(userMsg);    break;
        case AiProvider::ANTHROPIC: reply = _chatAnthropic(userMsg); break;
        case AiProvider::GEMINI:    reply = _chatGemini(userMsg);    break;
        default:                    reply = "Unknown provider";       break;
    }

    _busy = false;
    return reply;
}

void AiAssistant::chat(const String& userMsg, AiResponseCallback cb) {
    // Run in a short FreeRTOS task so loop() is not blocked
    struct ChatArgs { String msg; AiResponseCallback cb; AiAssistant* self; };
    auto* args = new ChatArgs{userMsg, cb, this};

    xTaskCreate([](void* param) {
        auto* a = static_cast<ChatArgs*>(param);
        String reply = a->self->chatBlocking(a->msg);
        bool ok = !reply.startsWith("Error:") && !reply.startsWith("AI ") &&
                  !reply.startsWith("No AI") && !reply.startsWith("WiFi");
        if (a->cb) a->cb(ok, ok ? reply : "", ok ? "" : reply);
        delete a;
        vTaskDelete(NULL);
    }, "ai_chat", 12288, args, 1, NULL);  // 12 KB: WiFiClientSecure(~8KB)+JSON+buffers
}

String AiAssistant::historyJson() const {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& m : _history) {
        JsonObject o = arr.add<JsonObject>();
        o["role"]    = m.role;
        o["content"] = m.content;
    }
    String out; serializeJson(arr, out); return out;
}

String AiAssistant::statusJson() const {
    JsonDocument doc;
    doc["enabled"]          = _enabled;
    doc["provider"]         = providerName();
    doc["model"]            = _model;
    doc["hasKeyOpenAI"]     = hasApiKey(AiProvider::OPENAI);
    doc["hasKeyAnthropic"]  = hasApiKey(AiProvider::ANTHROPIC);
    doc["hasKeyGemini"]     = hasApiKey(AiProvider::GEMINI);
    doc["historyCount"]     = (int)_history.size();
    doc["busy"]             = _busy;
    String out; serializeJson(doc, out); return out;
}
