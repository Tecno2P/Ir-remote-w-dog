#pragma once
// ============================================================
//  notification_manager.h  –  Batch 2: Notifications
//
//  Supported channels:
//    1. Telegram Bot API  (HTTPS)
//    2. WhatsApp via CallMeBot API (free, no business account needed)
//    3. Buzzer/LED alert  (on-device — no internet needed)
//
//  Config stored in LittleFS: /notify_config.json
//
//  API Endpoints (registered in web_server_batch2.cpp):
//    GET  /api/v1/notify/config       — read config
//    POST /api/v1/notify/config       — save config
//    POST /api/v1/notify/test         — send test message
//    GET  /api/v1/notify/queue        — pending messages count
// ============================================================
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <queue>
#include <functional>

#define NOTIFY_CFG_FILE     "/notify_config.json"
#define NOTIFY_QUEUE_MAX    10      // max pending messages
#define NOTIFY_TIMEOUT_MS   8000   // HTTP timeout per message
#define NOTIFY_RETRY_DELAY  30000  // ms between retry attempts
#define NOTIFY_TAG          "[NOTIFY]"

// ── Message struct ────────────────────────────────────────────
struct NotifyMessage {
    String text;
    bool   useTelegram  = true;
    bool   useWhatsApp  = false;
    uint8_t retries     = 0;
};

// ── Config struct ─────────────────────────────────────────────
struct NotifyConfig {
    // Telegram
    bool    telegramEnabled  = false;
    String  telegramToken;        // "123456:ABCdef..."
    String  telegramChatId;       // "123456789"

    // WhatsApp (CallMeBot)
    bool    whatsappEnabled  = false;
    String  whatsappPhone;        // "+923001234567"
    String  whatsappApiKey;       // from callmebot.com

    // On-device alerts
    bool    buzzerEnabled    = false;
    uint8_t buzzerPin        = 12;

    // Event toggles — which events trigger notifications
    bool    notifyRfidUnknown   = true;
    bool    notifyRfidKnown     = false;
    bool    notifyIrReceived    = false;
    bool    notifyScheduleFire  = false;
    bool    notifyMacroRun      = false;
    bool    notifyRuleFire      = true;
    bool    notifyBoot          = true;
    bool    notifyWifiChange    = true;
    bool    notifyOta           = true;
};

class NotificationManager {
public:
    NotificationManager();

    void begin();       // load config from LittleFS
    void loop();        // drain send queue (non-blocking)

    // ── Send functions ────────────────────────────────────────
    // Queue a message — returns immediately, sends in loop()
    void send(const String& text,
              bool telegram = true,
              bool whatsapp = false);

    // Convenience: sends on both channels if both enabled
    void notify(const String& text);

    // ── Typed event notifications ─────────────────────────────
    void onRfidScan    (const String& uid, const String& name, bool known);
    void onRuleFired   (const String& ruleName, const String& action);
    void onBoot        ();
    void onWifiChange  (bool connected, const String& ssid);
    void onOtaEvent    (const String& msg, bool success);
    void onScheduleFire(const String& entryName);
    void onMacroRun    (const String& macroName);

    // ── Config ────────────────────────────────────────────────
    bool           loadConfig();
    bool           saveConfig();
    NotifyConfig&  config()       { return _cfg; }
    const NotifyConfig& config() const { return _cfg; }
    String         configToJson() const;
    bool           configFromJson(const uint8_t* d, size_t l);

    // ── Status ────────────────────────────────────────────────
    size_t   queueSize()    const { return _queue.size(); }
    bool     isSending()    const { return _sending; }
    uint32_t sentTotal()    const { return _sentTotal; }
    uint32_t failedTotal()  const { return _failedTotal; }
    String   lastError()    const { return _lastError; }

    // Test: send a test message on all enabled channels
    bool sendTest(String& outError);

private:
    NotifyConfig  _cfg;
    std::queue<NotifyMessage> _queue;
    bool          _sending;
    uint32_t      _sentTotal;
    uint32_t      _failedTotal;
    String        _lastError;
    unsigned long _lastAttempt;

    bool _sendTelegram  (const String& text);
    bool _sendWhatsApp  (const String& text);
    void _buzzerBeep    (uint8_t times = 1);
    void _processQueue  ();
    String _urlEncode   (const String& s) const;
};

extern NotificationManager notifyMgr;
