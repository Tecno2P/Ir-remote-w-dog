#pragma once
// ============================================================
//  ai_assistant.h  —  Multi-Provider AI Integration v1.0
//
//  Supported providers:
//    - OpenAI   (GPT-4o, GPT-4o-mini, GPT-3.5-turbo)
//    - Anthropic (Claude-3-5-haiku, claude-3-5-sonnet)
//    - Google    (Gemini-1.5-flash, Gemini-1.5-pro)
//
//  API keys stored in NVS (encrypted flash partition).
//  Never stored in LittleFS plaintext.
//
//  AI Tools (function calling) — AI can control the device:
//    - list_buttons      : List all saved IR buttons
//    - transmit_button   : Send IR signal by name or ID
//    - get_device_status : Real-time heap, WiFi, OTA info
//    - list_schedules    : List all scheduled actions
//    - get_ir_log        : Recent received IR signals
// ============================================================
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <vector>
#include <functional>

// ── NVS namespace for API keys ─────────────────────────────
#define AI_NVS_NAMESPACE   "ai_keys"
#define AI_NVS_OPENAI      "openai"
#define AI_NVS_ANTHROPIC   "anthropic"
#define AI_NVS_GEMINI      "gemini"
#define AI_NVS_PROVIDER    "provider"
#define AI_NVS_MODEL       "model"
#define AI_NVS_ENABLED     "enabled"

// ── HTTP timeouts ─────────────────────────────────────────
#define AI_HTTP_TIMEOUT_MS    30000
#define AI_MAX_TOKENS         1024
#define AI_MAX_HISTORY        10    // max conversation turns kept in RAM
#define AI_MAX_RESPONSE_CHARS 4096  // truncate huge responses

// ── Provider enum ─────────────────────────────────────────
enum class AiProvider { OPENAI, ANTHROPIC, GEMINI, NONE };

// ── Conversation message ──────────────────────────────────
struct AiMessage {
    String role;     // "user" | "assistant" | "system"
    String content;
};

// ── Tool call result (AI → device action) ────────────────
struct AiToolResult {
    String toolName;
    String result;   // JSON string returned to AI
    bool   ok;
};

// ── Callback types ────────────────────────────────────────
using AiToolHandler = std::function<AiToolResult(const String& toolName,
                                                  const JsonObjectConst& args)>;
using AiResponseCallback = std::function<void(bool ok,
                                              const String& reply,
                                              const String& error)>;

class AiAssistant {
public:
    AiAssistant();

    // ── Lifecycle ─────────────────────────────────────────
    void begin();

    // ── Config (stored in NVS) ────────────────────────────
    bool        setApiKey(AiProvider provider, const String& key);
    String      getApiKey(AiProvider provider) const;
    bool        hasApiKey(AiProvider provider) const;
    void        setProvider(AiProvider p);
    AiProvider  getProvider()  const { return _provider; }
    void        setModel(const String& model);
    String      getModel()     const { return _model; }
    void        setEnabled(bool en);
    bool        isEnabled()    const { return _enabled; }
    String      providerName() const;
    String      providerName(AiProvider p) const;

    // ── Conversation ──────────────────────────────────────
    // Send a user message; calls back with AI reply (async-safe, runs in task)
    void chat(const String& userMessage, AiResponseCallback cb);

    // Clear conversation history
    void clearHistory();

    // Get history as JSON array
    String historyJson() const;

    // ── System prompt ─────────────────────────────────────
    void setSystemPrompt(const String& prompt);

    // ── Tool registration ─────────────────────────────────
    void registerToolHandler(AiToolHandler handler);

    // ── Status JSON ───────────────────────────────────────
    String statusJson() const;

    // ── Blocking chat (call from task, not loop()) ────────
    // Returns reply string or error. Used internally.
    String chatBlocking(const String& userMessage);

private:
    AiProvider   _provider;
    String       _model;
    String       _systemPrompt;
    bool         _enabled;
    bool         _busy;

    std::vector<AiMessage>  _history;
    AiToolHandler           _toolHandler;

    // ── Provider implementations ──────────────────────────
    String _chatOpenAI    (const String& userMsg);
    String _chatAnthropic (const String& userMsg);
    String _chatGemini    (const String& userMsg);

    // ── Tool calling helpers ──────────────────────────────
    String _buildToolsJson()       const;   // OpenAI/Gemini format
    String _buildAnthropicTools()  const;   // Anthropic format
    String _dispatchTool(const String& name, const JsonObjectConst& args);

    // ── HTTP helpers ──────────────────────────────────────
    String _httpPost(const String& url,
                     const String& headers,
                     const String& body);

    // ── NVS helpers ───────────────────────────────────────
    String  _nvsGet(const char* key) const;
    void    _nvsSet(const char* key, const String& val);

    // ── Conversation helpers ──────────────────────────────
    void    _addMessage(const String& role, const String& content);
    String  _buildOpenAIMessages()    const;
    String  _buildAnthropicMessages() const;
    String  _buildGeminiContents()    const;
};

extern AiAssistant aiAssistant;
