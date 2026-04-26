// ============================================================
//  notification_manager.cpp  –  Batch 2: Notifications
//  Telegram Bot API + WhatsApp CallMeBot + Buzzer
// ============================================================
#include "notification_manager.h"
#include "wifi_manager.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

NotificationManager notifyMgr;

// ─────────────────────────────────────────────────────────────
NotificationManager::NotificationManager()
    : _sending(false), _sentTotal(0), _failedTotal(0), _lastAttempt(0) {}

// ─────────────────────────────────────────────────────────────
void NotificationManager::begin() {
    loadConfig();
    Serial.printf(NOTIFY_TAG " Started — Telegram:%s  WhatsApp:%s  Buzzer:%s\n",
                  _cfg.telegramEnabled ? "ON" : "OFF",
                  _cfg.whatsappEnabled ? "ON" : "OFF",
                  _cfg.buzzerEnabled   ? "ON" : "OFF");
    if (_cfg.buzzerEnabled) {
        pinMode(_cfg.buzzerPin, OUTPUT);
        digitalWrite(_cfg.buzzerPin, LOW);
    }
}

// ─────────────────────────────────────────────────────────────
void NotificationManager::loop() {
    // Only process queue when WiFi STA is connected
    if (!wifiMgr.staConnected()) return;
    // Rate limit — don't hammer the APIs
    if (millis() - _lastAttempt < 2000) return;
    if (_queue.empty()) return;
    _processQueue();
}

// ─────────────────────────────────────────────────────────────
//  Public API
// ─────────────────────────────────────────────────────────────
void NotificationManager::send(const String& text, bool telegram, bool whatsapp) {
    if (_queue.size() >= NOTIFY_QUEUE_MAX) {
        Serial.println(NOTIFY_TAG " Queue full — dropping message");
        return;
    }
    NotifyMessage msg;
    msg.text         = text;
    msg.useTelegram  = telegram && _cfg.telegramEnabled;
    msg.useWhatsApp  = whatsapp && _cfg.whatsappEnabled;
    if (!msg.useTelegram && !msg.useWhatsApp) return; // nothing to send
    _queue.push(msg);
    Serial.printf(NOTIFY_TAG " Queued: \"%s\"\n", text.c_str());
}

void NotificationManager::notify(const String& text) {
    send(text, _cfg.telegramEnabled, _cfg.whatsappEnabled);
    if (_cfg.buzzerEnabled) _buzzerBeep(1);
}

// ─────────────────────────────────────────────────────────────
//  Typed event notifications
// ─────────────────────────────────────────────────────────────
void NotificationManager::onRfidScan(const String& uid,
                                      const String& name,
                                      bool known) {
    if (known  && !_cfg.notifyRfidKnown)   return;
    if (!known && !_cfg.notifyRfidUnknown) return;

    String msg = known
        ? String("✅ RFID: ") + name + " (UID: " + uid + ")"
        : String("⚠️ Unknown RFID card scanned! UID: ") + uid;

    notify(msg);
    if (!known && _cfg.buzzerEnabled) _buzzerBeep(3); // 3 beeps for unknown
}

void NotificationManager::onRuleFired(const String& ruleName,
                                       const String& action) {
    if (!_cfg.notifyRuleFire) return;
    notify(String("🤖 Rule fired: ") + ruleName + " → " + action);
}

void NotificationManager::onBoot() {
    if (!_cfg.notifyBoot) return;
    notify(String("🚀 Device booted — Firmware v") + FIRMWARE_VERSION);
}

void NotificationManager::onWifiChange(bool connected, const String& ssid) {
    if (!_cfg.notifyWifiChange) return;
    String msg = connected
        ? String("📶 WiFi connected: ") + ssid
        : String("❌ WiFi disconnected from: ") + ssid;
    notify(msg);
}

void NotificationManager::onOtaEvent(const String& msg, bool success) {
    if (!_cfg.notifyOta) return;
    notify((success ? String("✅ OTA: ") : String("❌ OTA failed: ")) + msg);
}

void NotificationManager::onScheduleFire(const String& entryName) {
    if (!_cfg.notifyScheduleFire) return;
    notify(String("⏰ Schedule fired: ") + entryName);
}

void NotificationManager::onMacroRun(const String& macroName) {
    if (!_cfg.notifyMacroRun) return;
    notify(String("▶️ Macro running: ") + macroName);
}

// ─────────────────────────────────────────────────────────────
//  Config
// ─────────────────────────────────────────────────────────────
bool NotificationManager::loadConfig() {
    if (!LittleFS.exists(NOTIFY_CFG_FILE)) return false;
    File f = LittleFS.open(NOTIFY_CFG_FILE, "r");
    if (!f) return false;
    JsonDocument doc;
    if (deserializeJson(doc, f)) { f.close(); return false; }
    f.close();

    _cfg.telegramEnabled     = doc["telegramEnabled"]    | false;
    _cfg.telegramToken       = doc["telegramToken"]      | (const char*)"";
    _cfg.telegramChatId      = doc["telegramChatId"]     | (const char*)"";
    _cfg.whatsappEnabled     = doc["whatsappEnabled"]    | false;
    _cfg.whatsappPhone       = doc["whatsappPhone"]      | (const char*)"";
    _cfg.whatsappApiKey      = doc["whatsappApiKey"]     | (const char*)"";
    _cfg.buzzerEnabled       = doc["buzzerEnabled"]      | false;
    _cfg.buzzerPin           = doc["buzzerPin"]          | (uint8_t)12;
    _cfg.notifyRfidUnknown   = doc["notifyRfidUnknown"]  | true;
    _cfg.notifyRfidKnown     = doc["notifyRfidKnown"]    | false;
    _cfg.notifyIrReceived    = doc["notifyIrReceived"]   | false;
    _cfg.notifyScheduleFire  = doc["notifyScheduleFire"] | false;
    _cfg.notifyMacroRun      = doc["notifyMacroRun"]     | false;
    _cfg.notifyRuleFire      = doc["notifyRuleFire"]     | true;
    _cfg.notifyBoot          = doc["notifyBoot"]         | true;
    _cfg.notifyWifiChange    = doc["notifyWifiChange"]   | true;
    _cfg.notifyOta           = doc["notifyOta"]          | true;
    return true;
}

bool NotificationManager::saveConfig() {
    File f = LittleFS.open(NOTIFY_CFG_FILE, "w");
    if (!f) return false;
    JsonDocument doc;
    doc["telegramEnabled"]   = _cfg.telegramEnabled;
    doc["telegramToken"]     = _cfg.telegramToken;
    doc["telegramChatId"]    = _cfg.telegramChatId;
    doc["whatsappEnabled"]   = _cfg.whatsappEnabled;
    doc["whatsappPhone"]     = _cfg.whatsappPhone;
    doc["whatsappApiKey"]    = _cfg.whatsappApiKey;
    doc["buzzerEnabled"]     = _cfg.buzzerEnabled;
    doc["buzzerPin"]         = _cfg.buzzerPin;
    doc["notifyRfidUnknown"] = _cfg.notifyRfidUnknown;
    doc["notifyRfidKnown"]   = _cfg.notifyRfidKnown;
    doc["notifyIrReceived"]  = _cfg.notifyIrReceived;
    doc["notifyScheduleFire"]= _cfg.notifyScheduleFire;
    doc["notifyMacroRun"]    = _cfg.notifyMacroRun;
    doc["notifyRuleFire"]    = _cfg.notifyRuleFire;
    doc["notifyBoot"]        = _cfg.notifyBoot;
    doc["notifyWifiChange"]  = _cfg.notifyWifiChange;
    doc["notifyOta"]         = _cfg.notifyOta;
    serializeJson(doc, f);
    f.close();
    return true;
}

String NotificationManager::configToJson() const {
    JsonDocument doc;
    doc["telegramEnabled"]   = _cfg.telegramEnabled;
    doc["telegramToken"]     = _cfg.telegramToken;
    doc["telegramChatId"]    = _cfg.telegramChatId;
    doc["whatsappEnabled"]   = _cfg.whatsappEnabled;
    doc["whatsappPhone"]     = _cfg.whatsappPhone;
    doc["whatsappApiKey"]    = _cfg.whatsappApiKey;
    doc["buzzerEnabled"]     = _cfg.buzzerEnabled;
    doc["buzzerPin"]         = _cfg.buzzerPin;
    doc["notifyRfidUnknown"] = _cfg.notifyRfidUnknown;
    doc["notifyRfidKnown"]   = _cfg.notifyRfidKnown;
    doc["notifyIrReceived"]  = _cfg.notifyIrReceived;
    doc["notifyScheduleFire"]= _cfg.notifyScheduleFire;
    doc["notifyMacroRun"]    = _cfg.notifyMacroRun;
    doc["notifyRuleFire"]    = _cfg.notifyRuleFire;
    doc["notifyBoot"]        = _cfg.notifyBoot;
    doc["notifyWifiChange"]  = _cfg.notifyWifiChange;
    doc["notifyOta"]         = _cfg.notifyOta;
    String out; serializeJson(doc, out);
    return out;
}

bool NotificationManager::configFromJson(const uint8_t* d, size_t l) {
    JsonDocument doc;
    if (deserializeJson(doc, d, l) != DeserializationError::Ok) return false;
    if (doc["telegramEnabled"].is<bool>())    _cfg.telegramEnabled   = doc["telegramEnabled"].as<bool>();
    if (doc["telegramToken"].is<const char*>())   _cfg.telegramToken = doc["telegramToken"].as<String>();
    if (doc["telegramChatId"].is<const char*>())  _cfg.telegramChatId= doc["telegramChatId"].as<String>();
    if (doc["whatsappEnabled"].is<bool>())    _cfg.whatsappEnabled   = doc["whatsappEnabled"].as<bool>();
    if (doc["whatsappPhone"].is<const char*>())   _cfg.whatsappPhone = doc["whatsappPhone"].as<String>();
    if (doc["whatsappApiKey"].is<const char*>())  _cfg.whatsappApiKey= doc["whatsappApiKey"].as<String>();
    if (doc["buzzerEnabled"].is<bool>())      _cfg.buzzerEnabled     = doc["buzzerEnabled"].as<bool>();
    if (doc["buzzerPin"].is<uint8_t>())       _cfg.buzzerPin         = doc["buzzerPin"].as<uint8_t>();
    if (doc["notifyRfidUnknown"].is<bool>())  _cfg.notifyRfidUnknown = doc["notifyRfidUnknown"].as<bool>();
    if (doc["notifyRfidKnown"].is<bool>())    _cfg.notifyRfidKnown   = doc["notifyRfidKnown"].as<bool>();
    if (doc["notifyIrReceived"].is<bool>())   _cfg.notifyIrReceived  = doc["notifyIrReceived"].as<bool>();
    if (doc["notifyScheduleFire"].is<bool>()) _cfg.notifyScheduleFire= doc["notifyScheduleFire"].as<bool>();
    if (doc["notifyMacroRun"].is<bool>())     _cfg.notifyMacroRun    = doc["notifyMacroRun"].as<bool>();
    if (doc["notifyRuleFire"].is<bool>())     _cfg.notifyRuleFire    = doc["notifyRuleFire"].as<bool>();
    if (doc["notifyBoot"].is<bool>())         _cfg.notifyBoot        = doc["notifyBoot"].as<bool>();
    if (doc["notifyWifiChange"].is<bool>())   _cfg.notifyWifiChange  = doc["notifyWifiChange"].as<bool>();
    if (doc["notifyOta"].is<bool>())          _cfg.notifyOta         = doc["notifyOta"].as<bool>();
    return true;
}

bool NotificationManager::sendTest(String& outError) {
    String msg = String("🔔 Test from IR-Remote v") + FIRMWARE_VERSION;
    bool ok = false;
    if (_cfg.telegramEnabled) {
        if (_sendTelegram(msg)) ok = true;
        else outError += "Telegram failed. ";
    }
    if (_cfg.whatsappEnabled) {
        if (_sendWhatsApp(msg)) ok = true;
        else outError += "WhatsApp failed. ";
    }
    return ok;
}

// ─────────────────────────────────────────────────────────────
//  Internal queue processing
// ─────────────────────────────────────────────────────────────
void NotificationManager::_processQueue() {
    if (_queue.empty()) return;
    _lastAttempt = millis();
    _sending = true;

    NotifyMessage& msg = _queue.front();
    bool ok = true;

    if (msg.useTelegram && _cfg.telegramEnabled) {
        if (!_sendTelegram(msg.text)) {
            ok = false;
            _lastError = "Telegram send failed";
        }
    }
    if (msg.useWhatsApp && _cfg.whatsappEnabled) {
        if (!_sendWhatsApp(msg.text)) {
            ok = false;
            _lastError = "WhatsApp send failed";
        }
    }

    if (ok) {
        _sentTotal++;
        _queue.pop();
        Serial.printf(NOTIFY_TAG " Sent OK (total=%u)\n", _sentTotal);
    } else {
        _failedTotal++;
        msg.retries++;
        if (msg.retries >= 3) {
            Serial.printf(NOTIFY_TAG " Dropping message after 3 failures: %s\n",
                          msg.text.c_str());
            _queue.pop();
        } else {
            Serial.printf(NOTIFY_TAG " Retry %u/3 scheduled\n", msg.retries);
            _lastAttempt = millis() + NOTIFY_RETRY_DELAY; // back off
        }
    }
    _sending = false;
}

// ─────────────────────────────────────────────────────────────
//  Telegram Bot API
//  URL: https://api.telegram.org/bot<TOKEN>/sendMessage
//       ?chat_id=<CHAT_ID>&text=<TEXT>
// ─────────────────────────────────────────────────────────────
bool NotificationManager::_sendTelegram(const String& text) {
    if (_cfg.telegramToken.isEmpty() || _cfg.telegramChatId.isEmpty()) {
        _lastError = "Telegram token/chatId not configured";
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();   // skip cert verification (simpler, acceptable for IoT)
    client.setTimeout(NOTIFY_TIMEOUT_MS / 1000);

    HTTPClient http;
    String url = String("https://api.telegram.org/bot")
               + _cfg.telegramToken
               + "/sendMessage?chat_id="
               + _cfg.telegramChatId
               + "&text="
               + _urlEncode(text)
               + "&parse_mode=HTML";

    http.begin(client, url);
    http.setTimeout(NOTIFY_TIMEOUT_MS);
    int code = http.GET();
    http.end();

    if (code == 200) {
        Serial.println(NOTIFY_TAG " Telegram: sent OK");
        return true;
    }
    Serial.printf(NOTIFY_TAG " Telegram: HTTP %d\n", code);
    _lastError = String("Telegram HTTP ") + code;
    return false;
}

// ─────────────────────────────────────────────────────────────
//  WhatsApp via CallMeBot API (free)
//  Setup: https://www.callmebot.com/blog/free-api-whatsapp-messages/
//  URL: https://api.callmebot.com/whatsapp.php
//       ?phone=<PHONE>&text=<TEXT>&apikey=<KEY>
// ─────────────────────────────────────────────────────────────
bool NotificationManager::_sendWhatsApp(const String& text) {
    if (_cfg.whatsappPhone.isEmpty() || _cfg.whatsappApiKey.isEmpty()) {
        _lastError = "WhatsApp phone/apiKey not configured";
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(NOTIFY_TIMEOUT_MS / 1000);

    HTTPClient http;
    String url = String("https://api.callmebot.com/whatsapp.php?phone=")
               + _urlEncode(_cfg.whatsappPhone)
               + "&text="
               + _urlEncode(text)
               + "&apikey="
               + _cfg.whatsappApiKey;

    http.begin(client, url);
    http.setTimeout(NOTIFY_TIMEOUT_MS);
    int code = http.GET();
    http.end();

    if (code == 200) {
        Serial.println(NOTIFY_TAG " WhatsApp: sent OK");
        return true;
    }
    Serial.printf(NOTIFY_TAG " WhatsApp: HTTP %d\n", code);
    _lastError = String("WhatsApp HTTP ") + code;
    return false;
}

// ─────────────────────────────────────────────────────────────
//  Buzzer
// ─────────────────────────────────────────────────────────────
void NotificationManager::_buzzerBeep(uint8_t times) {
    if (!_cfg.buzzerEnabled) return;
    for (uint8_t i = 0; i < times; i++) {
        digitalWrite(_cfg.buzzerPin, HIGH);
        delay(150);
        digitalWrite(_cfg.buzzerPin, LOW);
        if (i + 1 < times) delay(100);
    }
}

// ─────────────────────────────────────────────────────────────
//  URL Encode helper
// ─────────────────────────────────────────────────────────────
String NotificationManager::_urlEncode(const String& s) const {
    String encoded;
    encoded.reserve(s.length() * 3);
    for (char c : s) {
        if (isAlphaNumeric(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else if (c == ' ') {
            encoded += '+';
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
            encoded += buf;
        }
    }
    return encoded;
}
