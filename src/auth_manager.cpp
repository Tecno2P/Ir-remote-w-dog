// ============================================================
//  auth_manager.cpp  –  Batch 3: Web Panel Authentication
// ============================================================
#include "auth_manager.h"
#include "audit_manager.h"

AuthManager authMgr;

AuthManager::AuthManager()
    : _enabled(false), _firstLogin(true),
      _username("admin"), _failCount(0), _lockedUntil(0) {
    _passwordHash = _hashPassword("irremote123");
}

void AuthManager::begin() {
    loadConfig();
    Serial.printf(AUTH_TAG " Auth: %s  firstLogin=%s\n",
                  _enabled ? "ENABLED" : "disabled",
                  _firstLogin ? "yes" : "no");
}

void AuthManager::loop() { _cleanExpired(); }

// ── Login ─────────────────────────────────────────────────────
String AuthManager::login(const String& username,
                           const String& password,
                           const String& clientHint) {
    if (_lockedUntil > 0 && millis() < _lockedUntil) {
        auditMgr.log(AuditSource::AUTH, "LOGIN_LOCKED",
                     String("From ") + clientHint, false);
        return "";
    }
    if (!username.equalsIgnoreCase(_username) ||
        _hashPassword(password) != _passwordHash) {
        _failCount++;
        if (_failCount >= AUTH_MAX_FAILURES) {
            _lockedUntil = millis() + AUTH_LOCKOUT_MS;
            _failCount   = 0;
            auditMgr.log(AuditSource::AUTH, "LOGIN_LOCKOUT",
                         String("From ") + clientHint, false);
        } else {
            auditMgr.log(AuditSource::AUTH, "LOGIN_FAILED",
                         String("Attempt ") + _failCount + " from " + clientHint, false);
        }
        return "";
    }
    _failCount = 0; _lockedUntil = 0;
    if (_sessions.size() >= AUTH_MAX_SESSIONS)
        _sessions.erase(_sessions.begin());

    AuthSession s;
    s.token     = _generateToken();
    s.createdAt = millis();
    s.clientHint= clientHint;
    _sessions.push_back(s);
    auditMgr.log(AuditSource::AUTH, "LOGIN_OK",
                 String("User: ") + username + " from " + clientHint);
    return s.token;
}

bool AuthManager::logout(const String& token) {
    for (auto it = _sessions.begin(); it != _sessions.end(); ++it) {
        if (it->token == token) {
            _sessions.erase(it);
            auditMgr.log(AuditSource::AUTH, "LOGOUT", "");
            return true;
        }
    }
    return false;
}

bool AuthManager::validateToken(const String& token) {
    if (token.isEmpty()) return false;
    _cleanExpired();
    for (const auto& s : _sessions)
        if (s.token == token) return true;
    return false;
}

// ── Middleware ────────────────────────────────────────────────
String AuthManager::extractBearer(AsyncWebServerRequest* req) {
    if (!req->hasHeader("Authorization")) return "";
    String h = req->getHeader("Authorization")->value();
    if (h.startsWith("Bearer ")) return h.substring(7);
    return "";
}

bool AuthManager::checkAuth(AsyncWebServerRequest* req) {
    if (!_enabled) return true;
    if (validateToken(extractBearer(req))) return true;
    AsyncWebServerResponse* r = req->beginResponse(401,
        "application/json",
        "{\"error\":\"Unauthorized\","
        "\"hint\":\"POST /api/v1/auth/login\"}");
    r->addHeader("Access-Control-Allow-Origin", "*");
    r->addHeader("WWW-Authenticate", "Bearer realm=\\\"IR-Remote\\\"");
    req->send(r);
    return false;
}

// ── Password change ───────────────────────────────────────────
bool AuthManager::changePassword(const String& oldPass,
                                  const String& newPass) {
    if (_hashPassword(oldPass) != _passwordHash) return false;
    if (newPass.length() < 6) return false;
    _passwordHash = _hashPassword(newPass);
    _firstLogin   = false;
    saveConfig();
    clearSessions();
    auditMgr.log(AuditSource::AUTH, "PASSWORD_CHANGED", "");
    return true;
}

// ── Config ────────────────────────────────────────────────────
bool AuthManager::loadConfig() {
    if (!LittleFS.exists(AUTH_CFG_FILE)) return false;
    File f = LittleFS.open(AUTH_CFG_FILE, "r");
    if (!f) return false;
    JsonDocument doc;
    if (deserializeJson(doc, f)) { f.close(); return false; }
    f.close();
    _enabled      = doc["enabled"]    | false;
    _firstLogin   = doc["firstLogin"] | true;
    _username     = doc["username"]   | (const char*)"admin";
    _passwordHash = doc["passHash"]   | _hashPassword("irremote123");
    return true;
}

bool AuthManager::saveConfig() {
    File f = LittleFS.open(AUTH_CFG_FILE, "w");
    if (!f) return false;
    JsonDocument doc;
    doc["enabled"]    = _enabled;
    doc["firstLogin"] = _firstLogin;
    doc["username"]   = _username;
    doc["passHash"]   = _passwordHash;
    serializeJson(doc, f); f.close();
    return true;
}

void AuthManager::setAuthEnabled(bool en) {
    _enabled = en; saveConfig();
    if (!en) clearSessions();
}

void AuthManager::clearSessions() { _sessions.clear(); }

String AuthManager::statusJson() const {
    bool locked = _lockedUntil > 0 && millis() < _lockedUntil;
    return String("{\"enabled\":") + (_enabled?"true":"false")
         + ",\"firstLogin\":"      + (_firstLogin?"true":"false")
         + ",\"sessions\":"        + _sessions.size()
         + ",\"locked\":"          + (locked?"true":"false") + "}";
}

String AuthManager::configJson() const {
    return String("{\"enabled\":") + (_enabled?"true":"false")
         + ",\"username\":\""     + _username + "\""
         + ",\"firstLogin\":"     + (_firstLogin?"true":"false") + "}";
}

// ── Internals ─────────────────────────────────────────────────
void AuthManager::_cleanExpired() {
    unsigned long now = millis();
    _sessions.erase(
        std::remove_if(_sessions.begin(), _sessions.end(),
            [&](const AuthSession& s){
                return (now - s.createdAt) >= AUTH_TOKEN_TTL_MS;
            }),
        _sessions.end());
}

String AuthManager::_hashPassword(const String& pass) const {
    uint32_t h = 5381;
    for (char c : pass) h = ((h << 5) + h) + (uint8_t)c;
    h ^= (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF);
    char buf[9]; snprintf(buf, sizeof(buf), "%08X", h);
    return String(buf);
}

String AuthManager::_generateToken() const {
    char buf[AUTH_TOKEN_LEN + 1];
    for (int i = 0; i < AUTH_TOKEN_LEN; i += 8)
        snprintf(buf + i, 9, "%08X", esp_random());
    buf[AUTH_TOKEN_LEN] = '\0';
    return String(buf);
}
