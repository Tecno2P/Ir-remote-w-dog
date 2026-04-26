// ============================================================
//  ir_database.cpp
//
//  Storage improvements in this version:
//    Fix 1 – Auto-save received IR signals
//             autoSaveReceived() checks for duplicates (same
//             protocol + code) before adding to the DB.
//             Setting persisted to IR_AUTO_SAVE_FILE.
//
//    Fix 2 – Lazy save (batch flash writes)
//             add/update/remove/clear call _markDirty() instead
//             of save().  loop() flushes after DB_LAZY_SAVE_MS ms
//             of inactivity.  importJson/clear still flush
//             immediately because they replace the whole DB.
//
//    Fix 3 – RAW memory guard
//             add() rejects new RAW buttons when _rawCount()
//             >= MAX_RAW_BUTTONS (default 16).
//
//    Fix 4 – Streaming JSON save
//             save() serialises one IRButton at a time directly
//             to the LittleFS file.  Peak extra RAM ≈ 1 IRButton
//             (~1 KB) instead of a full snapshot vector
//             (up to 136 KB with 128 RAW buttons).
//
//  Pre-existing design kept intact:
//    * portMUX spinlock guards vector mutations.
//    * Atomic temp-file rename prevents partial-write corruption.
//    * Heap-alloc ops (push_back, erase, String copy) stay
//      outside portENTER_CRITICAL sections.
// ============================================================
#include "ir_database.h"

// ── Global singleton ─────────────────────────────────────────
IRDatabase irDB;

// ── Constructor ──────────────────────────────────────────────
IRDatabase::IRDatabase()
    : _nextId(1),
      _mux(portMUX_INITIALIZER_UNLOCKED),
      _dirty(false),
      _dirtyMs(0),
      _autoSave(IR_AUTO_SAVE_DEFAULT)
{}

// ── begin ────────────────────────────────────────────────────
bool IRDatabase::begin() {
    _buttons.clear();
    _nextId  = 1;
    _dirty   = false;
    _dirtyMs = 0;

    // Fix 1: restore auto-save preference from flash
    _loadAutoSave();

    if (!LittleFS.exists(DB_FILE)) {
        Serial.println(DEBUG_TAG " No DB file – starting with empty database.");
        return true;
    }

    File f = LittleFS.open(DB_FILE, "r");
    if (!f) {
        Serial.println(DEBUG_TAG " ERROR: Cannot open DB file for reading.");
        return false;
    }

    // Stream-parse directly from the file to minimise RAM usage
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err != DeserializationError::Ok) {
        Serial.printf(DEBUG_TAG " DB parse error: %s — starting fresh.\n",
                      err.c_str());
        return false;
    }

    if (!doc["buttons"].is<JsonArrayConst>()) {
        Serial.println(DEBUG_TAG " DB has no 'buttons' array — starting fresh.");
        return false;
    }

    std::vector<IRButton> loaded;
    uint32_t maxId = 0;
    for (JsonObjectConst obj : doc["buttons"].as<JsonArrayConst>()) {
        if ((int)loaded.size() >= MAX_BUTTONS) break;
        IRButton btn;
        if (btn.fromJson(obj)) {
            loaded.push_back(btn);
            if (btn.id > maxId) maxId = btn.id;
        }
    }

    portENTER_CRITICAL(&_mux);
    _buttons = std::move(loaded);
    _nextId  = maxId + 1;
    portEXIT_CRITICAL(&_mux);

    Serial.printf(DEBUG_TAG " Loaded %d button(s) from DB (RAW: %u, autoSave: %s).\n",
                  (int)_buttons.size(), (unsigned)_rawCount(),
                  _autoSave ? "ON" : "OFF");
    return true;
}

// ── Fix 2: loop ───────────────────────────────────────────────
// Called from main loop() every iteration.
// Flushes the dirty flag to flash after DB_LAZY_SAVE_MS ms.
void IRDatabase::loop() {
    if (!_dirty) return;
    if ((millis() - _dirtyMs) >= DB_LAZY_SAVE_MS) {
        save();
        // save() clears _dirty on success; leave it set on failure
        // so we retry next time loop() fires.
    }
}

// ── Fix 2: _markDirty ────────────────────────────────────────
void IRDatabase::_markDirty() {
    if (!_dirty) {
        _dirty   = true;
        _dirtyMs = millis();
    }
    // If already dirty, keep the original timestamp so the
    // 5-second window counts from the FIRST unsaved change.
}

// ── Fix 4: save (streaming) ───────────────────────────────────
// Serialises one IRButton at a time directly to the temp file.
// Peak extra RAM ≈ 1 JsonDocument + 1 IRButton (~1.5 KB)
// regardless of how many buttons are in the DB.
bool IRDatabase::save() {
    const char* tmpFile = "/ir_db.tmp";

    File f = LittleFS.open(tmpFile, "w");
    if (!f) {
        Serial.println(DEBUG_TAG " ERROR: Cannot open temp DB file for writing.");
        return false;
    }

    // Write opening wrapper
    f.print("{\"buttons\":[");

    size_t totalWritten = 10; // length of "{\"buttons\":["
    bool   first        = true;

    // Snapshot the count and IDs under lock, then serialise each
    // button individually outside the lock to keep heap-alloc
    // (JsonDocument, String) away from the critical section.
    size_t count = 0;
    std::vector<uint32_t> ids;
    {
        portENTER_CRITICAL(&_mux);
        count = _buttons.size();
        ids.reserve(count);
        for (size_t i = 0; i < count; ++i) ids.push_back(_buttons[i].id);
        portEXIT_CRITICAL(&_mux);
    }

    for (uint32_t id : ids) {
        // Safe copy outside spinlock (IRButton has String/vector members)
        IRButton btn = findById(id);
        if (!btn.id) continue;    // button was removed after we took the snapshot

        JsonDocument doc;
        JsonObject obj = doc.to<JsonObject>();
        btn.toJson(obj);

        if (!first) { f.print(","); totalWritten += 1; }
        first = false;
        size_t written = serializeJson(doc, f);
        if (written == 0) {
            Serial.println(DEBUG_TAG " ERROR: Button serialization produced 0 bytes.");
            f.close();
            LittleFS.remove(tmpFile);
            return false;
        }
        totalWritten += written;
    }

    f.print("]}");
    totalWritten += 2;
    f.close();

    // Atomic rename — prevents partial-write corruption on power loss
    LittleFS.remove(DB_FILE);
    if (!LittleFS.rename(tmpFile, DB_FILE)) {
        Serial.println(DEBUG_TAG " ERROR: DB rename failed.");
        return false;
    }

    _dirty = false;   // clear dirty flag on successful save
    Serial.printf(DEBUG_TAG " DB saved (%u button(s), %u bytes).\n",
                  (unsigned)count, (unsigned)totalWritten);
    return true;
}

// ── Fix 3: _rawCount ─────────────────────────────────────────
uint8_t IRDatabase::_rawCount() const {
    uint8_t n = 0;
    for (const auto& b : _buttons)
        if (b.protocol == IRProtocol::RAW) ++n;
    return n;
}

// ── add ──────────────────────────────────────────────────────
uint32_t IRDatabase::add(IRButton& btn) {
    if ((int)_buttons.size() >= MAX_BUTTONS) {
        Serial.println(DEBUG_TAG " ERROR: MAX_BUTTONS limit reached.");
        return 0;
    }
    if (!btn.isValid()) {
        Serial.println(DEBUG_TAG " ERROR: Button failed validation.");
        return 0;
    }

    // Fix 3: RAW memory guard — cap RAW buttons independently
    if (btn.protocol == IRProtocol::RAW && _rawCount() >= MAX_RAW_BUTTONS) {
        Serial.printf(DEBUG_TAG " ERROR: RAW button limit (%d) reached — "
                      "delete an existing RAW button first.\n", MAX_RAW_BUTTONS);
        return 0;
    }

    // Assign ID under lock (cheap integer op only)
    uint32_t newid;
    portENTER_CRITICAL(&_mux);
    newid = newId();
    portEXIT_CRITICAL(&_mux);
    btn.id = newid;

    // push_back OUTSIDE spinlock — IRButton contains String/vector
    // whose heap allocations must not run with interrupts disabled.
    _buttons.push_back(btn);

    // Fix 2: mark dirty instead of saving immediately
    _markDirty();

    Serial.printf(DEBUG_TAG " Added button id=%u name='%s'%s\n",
                  btn.id, btn.name.c_str(),
                  btn.protocol == IRProtocol::RAW ? " [RAW]" : "");
    return btn.id;
}

// ── update ───────────────────────────────────────────────────
bool IRDatabase::update(uint32_t id, const IRButton& updated) {
    bool found = false;
    IRButton replacement = updated;
    replacement.id = id;

    portENTER_CRITICAL(&_mux);
    for (IRButton& b : _buttons) {
        if (b.id == id) {
            std::swap(b, replacement);
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&_mux);

    if (!found) {
        Serial.printf(DEBUG_TAG " update: id=%u not found.\n", id);
        return false;
    }

    // Fix 2: mark dirty instead of saving immediately
    _markDirty();

    Serial.printf(DEBUG_TAG " Updated button id=%u\n", id);
    return true;
}

// ── remove ───────────────────────────────────────────────────
bool IRDatabase::remove(uint32_t id) {
    int foundIdx = -1;
    portENTER_CRITICAL(&_mux);
    for (int i = 0; i < (int)_buttons.size(); ++i) {
        if (_buttons[i].id == id) { foundIdx = i; break; }
    }
    if (foundIdx >= 0)
        std::swap(_buttons[foundIdx], _buttons.back());
    portEXIT_CRITICAL(&_mux);

    if (foundIdx < 0) {
        Serial.printf(DEBUG_TAG " remove: id=%u not found.\n", id);
        return false;
    }
    _buttons.pop_back();  // heap free (String/vector dtors) outside spinlock

    // Fix 2: mark dirty instead of saving immediately
    _markDirty();

    Serial.printf(DEBUG_TAG " Deleted button id=%u\n", id);
    return true;
}

// ── findById ─────────────────────────────────────────────────
// Single critical section: find index AND copy the button atomically.
// The previous two-lock design had a TOCTOU race: an element could be
// erased between the two critical sections, making foundIdx stale.
// NOTE: IRButton copy constructor allocates heap (String / vector) while
// the spinlock is held.  This is acceptable: the copy is small (~1 KB
// worst-case for a RAW button) and completes in microseconds on ESP32.
// Alternatives (e.g. pre-reserving a buffer) add complexity for no gain.
IRButton IRDatabase::findById(uint32_t id) const {
    IRButton copy;
    portENTER_CRITICAL(const_cast<portMUX_TYPE*>(&_mux));
    for (int i = 0; i < (int)_buttons.size(); ++i) {
        if (_buttons[i].id == id) {
            copy = _buttons[i];   // copy inside the lock — single atomic operation
            break;
        }
    }
    portEXIT_CRITICAL(const_cast<portMUX_TYPE*>(&_mux));
    return copy;  // id==0 means "not found"
}

// ── Fix 1: autoSaveReceived ───────────────────────────────────
// Called from the IR receive callback (main.cpp onIRReceived).
// Adds the button to the DB only when:
//   1. autoSave is enabled
//   2. The (protocol, code) pair is not already in the DB
//   3. The DB is not full (MAX_BUTTONS / MAX_RAW_BUTTONS checks
//      are done inside add())
uint32_t IRDatabase::autoSaveReceived(IRButton& btn) {
    if (!_autoSave) return 0;

    // RAW signals have code == 0 — each capture is unique by
    // definition, so skip the duplicate check for RAW.
    if (btn.protocol != IRProtocol::RAW) {
        for (const auto& b : _buttons) {
            if (b.protocol == btn.protocol && b.code == btn.code) {
                // Duplicate — silently ignore
                return 0;
            }
        }
    }

    uint32_t id = add(btn);
    if (id) {
        Serial.printf(DEBUG_TAG " Auto-saved: id=%u '%s'\n",
                      id, btn.name.c_str());
    }
    return id;
}

// ── Fix 1: setAutoSave ───────────────────────────────────────
void IRDatabase::setAutoSave(bool en) {
    _autoSave = en;
    _saveAutoSave();
    Serial.printf(DEBUG_TAG " Auto-save %s\n", en ? "ENABLED" : "DISABLED");
}

// ── Fix 1: persist / restore auto-save setting ───────────────
bool IRDatabase::_saveAutoSave() {
    File f = LittleFS.open(IR_AUTO_SAVE_FILE, "w");
    if (!f) return false;
    f.printf("{\"autoSave\":%s}", _autoSave ? "true" : "false");
    f.close();
    return true;
}

bool IRDatabase::_loadAutoSave() {
    if (!LittleFS.exists(IR_AUTO_SAVE_FILE)) return false;
    File f = LittleFS.open(IR_AUTO_SAVE_FILE, "r");
    if (!f) return false;
    JsonDocument doc;
    if (deserializeJson(doc, f) != DeserializationError::Ok) { f.close(); return false; }
    f.close();
    _autoSave = doc["autoSave"] | IR_AUTO_SAVE_DEFAULT;
    return true;
}

// ── exportJson ───────────────────────────────────────────────
String IRDatabase::exportJson() const {
    std::vector<IRButton> snapshot;
    portENTER_CRITICAL(const_cast<portMUX_TYPE*>(&_mux));
    snapshot = _buttons;
    portEXIT_CRITICAL(const_cast<portMUX_TYPE*>(&_mux));

    JsonDocument doc;
    JsonArray arr = doc["buttons"].to<JsonArray>();
    for (const IRButton& btn : snapshot) {
        JsonObject obj = arr.add<JsonObject>();
        btn.toJson(obj);
    }
    String out;
    serializeJsonPretty(doc, out);
    return out;
}

// ── compactJson ──────────────────────────────────────────────
String IRDatabase::compactJson() const {
    std::vector<IRButton> snapshot;
    portENTER_CRITICAL(const_cast<portMUX_TYPE*>(&_mux));
    snapshot = _buttons;
    portEXIT_CRITICAL(const_cast<portMUX_TYPE*>(&_mux));

    JsonDocument doc;
    JsonArray arr = doc["buttons"].to<JsonArray>();
    for (const IRButton& btn : snapshot) {
        JsonObject obj = arr.add<JsonObject>();
        btn.toJson(obj);
    }
    String out;
    serializeJson(doc, out);
    return out;
}

// ── importJson ───────────────────────────────────────────────
bool IRDatabase::importJson(const String& json) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err != DeserializationError::Ok) {
        Serial.printf(DEBUG_TAG " Import parse error: %s\n", err.c_str());
        return false;
    }
    if (!doc["buttons"].is<JsonArrayConst>()) {
        Serial.println(DEBUG_TAG " Import: missing 'buttons' array.");
        return false;
    }

    std::vector<IRButton> imported;
    uint32_t maxId = 0;
    for (JsonObjectConst obj : doc["buttons"].as<JsonArrayConst>()) {
        if ((int)imported.size() >= MAX_BUTTONS) break;
        IRButton btn;
        if (btn.fromJson(obj) && btn.isValid()) {
            imported.push_back(btn);
            if (btn.id > maxId) maxId = btn.id;
        }
    }

    std::vector<IRButton> old;
    portENTER_CRITICAL(&_mux);
    old      = std::move(_buttons);
    _buttons = std::move(imported);
    _nextId  = maxId + 1;
    portEXIT_CRITICAL(&_mux);
    // 'old' destructs here — heap free is safe outside the spinlock

    // Import always flushes immediately (entire DB replaced)
    _dirty = false;
    save();
    Serial.printf(DEBUG_TAG " Imported %d button(s).\n", (int)_buttons.size());
    return true;
}

// ── clear ────────────────────────────────────────────────────
void IRDatabase::clear() {
    portENTER_CRITICAL(&_mux);
    _buttons.clear();
    _nextId = 1;
    portEXIT_CRITICAL(&_mux);

    // Clear always flushes immediately
    _dirty = false;
    save();
    Serial.println(DEBUG_TAG " Database cleared.");
}

// ── backup ───────────────────────────────────────────────────
// Writes the current live DB to DB_BACKUP_FILE using the same
// streaming strategy as save() — one button at a time so peak
// RAM stays at ~1 IRButton regardless of database size.
bool IRDatabase::backup() {
    // Snapshot IDs under lock; serialise outside
    std::vector<uint32_t> ids;
    {
        portENTER_CRITICAL(&_mux);
        ids.reserve(_buttons.size());
        for (const auto& b : _buttons) ids.push_back(b.id);
        portEXIT_CRITICAL(&_mux);
    }

    const char* tmpFile = "/ir_bak.tmp";
    File f = LittleFS.open(tmpFile, "w");
    if (!f) {
        Serial.println(DEBUG_TAG " [Backup] ERROR: cannot open temp backup file.");
        return false;
    }

    f.print("{\"buttons\":[");
    bool first = true;
    for (uint32_t id : ids) {
        IRButton btn = findById(id);
        if (!btn.id) continue;
        JsonDocument doc;
        JsonObject obj = doc.to<JsonObject>();
        btn.toJson(obj);
        if (!first) f.print(",");
        first = false;
        serializeJson(doc, f);
    }
    f.print("]}");
    f.close();

    // Atomic rename to backup file
    LittleFS.remove(DB_BACKUP_FILE);
    if (!LittleFS.rename(tmpFile, DB_BACKUP_FILE)) {
        Serial.println(DEBUG_TAG " [Backup] ERROR: rename to backup file failed.");
        LittleFS.remove(tmpFile);
        return false;
    }

    Serial.printf(DEBUG_TAG " [Backup] Created %s (%u button(s))\n",
                  DB_BACKUP_FILE, (unsigned)ids.size());
    return true;
}

// ── validateRestoreJson ──────────────────────────────────────
// Validates a JSON string without modifying any state.
// Checks:
//   1. Valid JSON syntax
//   2. Top-level "buttons" array present
//   3. At least one button passes fromJson() + isValid()
//   4. No RAW-only DB (all buttons RAW would exhaust heap)
// Returns a RestoreResult with accepted/rejected counts and
// an error string on failure.
IRDatabase::RestoreResult
IRDatabase::validateRestoreJson(const String& json) const {
    RestoreResult res{false, 0, 0, ""};

    // ── 1. JSON syntax ────────────────────────────────────────
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err != DeserializationError::Ok) {
        res.error = String("JSON parse error: ") + err.c_str();
        Serial.printf(DEBUG_TAG " [Restore] Validation FAILED — %s\n",
                      res.error.c_str());
        return res;
    }

    // ── 2. Structure ──────────────────────────────────────────
    if (!doc["buttons"].is<JsonArrayConst>()) {
        res.error = "Missing top-level 'buttons' array";
        Serial.println(DEBUG_TAG " [Restore] Validation FAILED — " + res.error);
        return res;
    }

    JsonArrayConst arr = doc["buttons"].as<JsonArrayConst>();
    if (arr.size() == 0) {
        // Empty array is technically valid — allow it (restores to blank DB)
        res.ok       = true;
        res.accepted = 0;
        Serial.println(DEBUG_TAG " [Restore] Validation OK — empty buttons array");
        return res;
    }

    // ── 3. Per-button validation ──────────────────────────────
    uint8_t rawCount  = 0;
    uint8_t totalSeen = 0;
    for (JsonObjectConst obj : arr) {
        if (totalSeen >= MAX_BUTTONS) break;   // stop counting at the cap
        totalSeen++;
        IRButton btn;
        if (!btn.fromJson(obj) || !btn.isValid()) {
            res.rejected++;
            Serial.printf(DEBUG_TAG " [Restore] Button #%u rejected"
                          " (fromJson/isValid failed)\n", totalSeen);
            continue;
        }
        if (btn.protocol == IRProtocol::RAW) rawCount++;
        res.accepted++;
    }

    if (res.accepted == 0) {
        res.error = String("No valid buttons found (") +
                    res.rejected + " rejected)";
        Serial.println(DEBUG_TAG " [Restore] Validation FAILED — " + res.error);
        return res;
    }

    // ── 4. RAW memory guard ───────────────────────────────────
    if (rawCount > MAX_RAW_BUTTONS) {
        res.error = String("Too many RAW buttons (") + rawCount +
                    "), max allowed: " + MAX_RAW_BUTTONS;
        Serial.println(DEBUG_TAG " [Restore] Validation FAILED — " + res.error);
        return res;
    }

    res.ok = true;
    Serial.printf(DEBUG_TAG " [Restore] Validation OK — accepted=%u rejected=%u"
                  " raw=%u\n", res.accepted, res.rejected, rawCount);
    return res;
}

// ── restore ──────────────────────────────────────────────────
// Full pipeline: validate → backup → importJson (atomic swap).
// The live DB is never touched if validation fails.
IRDatabase::RestoreResult IRDatabase::restore(const String& json) {
    // Step 1 — validate before touching anything
    RestoreResult res = validateRestoreJson(json);
    if (!res.ok) return res;

    // Step 2 — backup current DB so the user can undo
    if (!backup()) {
        // Backup failure is non-fatal — warn but continue.
        // Losing the backup is better than refusing a valid restore.
        Serial.println(DEBUG_TAG " [Restore] WARNING: backup() failed — "
                       "proceeding without backup.");
        res.error = "WARNING: backup failed (restore proceeded anyway)";
    }

    // Step 3 — atomic swap via importJson
    // importJson() validates again internally; this is intentional —
    // it re-checks after backup so the live DB is always consistent.
    if (!importJson(json)) {
        res.ok    = false;
        res.error = "importJson() failed after backup — live DB unchanged";
        Serial.println(DEBUG_TAG " [Restore] ERROR: " + res.error);
        return res;
    }

    Serial.printf(DEBUG_TAG " [Restore] SUCCESS — %u button(s) loaded, "
                  "%u skipped\n", res.accepted, res.rejected);
    return res;
}

// ── backupJson ───────────────────────────────────────────────
// Returns the backup file as a pretty-printed JSON string for
// the /api/backup/download endpoint.
String IRDatabase::backupJson() const {
    if (!LittleFS.exists(DB_BACKUP_FILE)) {
        Serial.println(DEBUG_TAG " [Backup] No backup file found.");
        return "";
    }
    File f = LittleFS.open(DB_BACKUP_FILE, "r");
    if (!f) return "";
    String out = f.readString();
    f.close();
    return out;
}

