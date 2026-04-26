#pragma once
// ============================================================
//  ir_database.h  –  CRUD storage for IRButton objects
//                    backed by a LittleFS JSON file.
//
//  Thread-safety note: All methods must be called from the
//  Arduino loop task.  Async callbacks that modify the DB
//  must post work back to the main task (or use a mutex).
//  In this firmware all API handlers run in the async task
//  but immediately call synchronous methods; because ESP32
//  async server handlers run from a dedicated TCP task we use
//  a simple portMUX_TYPE spinlock to guard the vector.
//
//  Storage improvements (v2.1.0):
//    Fix 1 — Auto-save: received IR signals optionally saved
//             automatically, with duplicate detection.
//    Fix 2 — Lazy save: dirty flag + 5 s timer batches flash
//             writes; call loop() from main loop().
//    Fix 3 — RAW guard: MAX_RAW_BUTTONS cap prevents heap
//             exhaustion when many RAW buttons are stored.
//    Fix 4 — Streaming save: JSON written directly to flash
//             one button at a time — no full-DB RAM copy.
// ============================================================
#include <Arduino.h>
#include <vector>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "ir_button.h"
#include "config.h"

class IRDatabase {
public:
    IRDatabase();

    // Mount filesystem and load DB from flash.
    // Returns false only on a LittleFS read/parse error.
    // A missing file is not an error (clean first boot).
    bool begin();

    // ── Fix 2: Lazy save ─────────────────────────────────────
    // Call from main loop() every iteration.
    // Flushes dirty RAM state to flash after DB_LAZY_SAVE_MS ms.
    void loop();

    // Persist current state to flash immediately (Fix 4: streaming).
    // Normally called by loop(); call directly only when an
    // immediate flush is required (e.g. importJson, clear).
    bool save();

    // ── Fix 1: Auto-save ─────────────────────────────────────
    // Pass a freshly decoded IRButton from the IR receiver.
    // If autoSave is enabled and the code is not a duplicate,
    // the button is added to the DB and lazy-saved.
    // Returns the assigned ID, or 0 if skipped / DB full.
    uint32_t autoSaveReceived(IRButton& btn);

    bool autoSaveEnabled() const { return _autoSave; }
    void setAutoSave(bool en);    // persists setting to IR_AUTO_SAVE_FILE

    // Add a new button; auto-assigns ID.
    // Returns assigned ID, or 0 on error (DB full / invalid).
    uint32_t add(IRButton& btn);

    // Replace an existing button by ID.
    bool update(uint32_t id, const IRButton& updated);

    // Remove by ID.
    bool remove(uint32_t id);

    // Lookup by ID; returns default IRButton (id==0) if not found.
    // Returns by value with the spinlock held — safe across tasks.
    IRButton findById(uint32_t id) const;

    // Read-only view of all buttons.
    const std::vector<IRButton>& buttons() const { return _buttons; }

    // Count.
    size_t size() const { return _buttons.size(); }

    // True when unsaved changes exist in RAM.
    bool isDirty() const { return _dirty; }

    // Number of RAW-protocol buttons currently in DB.
    uint8_t rawCount() const { return _rawCount(); }

    // Export entire DB as pretty-printed JSON string.
    String exportJson() const;    // pretty-printed (for file export/download)
    String compactJson() const;   // compact (for API responses — saves bandwidth)

    // Replace entire DB from a JSON string.
    // Returns false on parse error; leaves DB unchanged on failure.
    bool importJson(const String& json);

    // Erase all buttons (does not remove the file; writes empty DB).
    void clear();

    // ── Backup & Restore ─────────────────────────────────────
    // Create /ir_database_backup.json from the current live DB.
    // Returns false if the flash write fails.
    bool backup();

    // Validate a JSON string as a well-formed IR database.
    // Populates result with per-field error details on failure.
    struct RestoreResult {
        bool    ok;
        uint8_t accepted;    // buttons that passed validation
        uint8_t rejected;    // buttons that failed validation
        String  error;       // human-readable reason on failure
    };
    RestoreResult validateRestoreJson(const String& json) const;

    // Full restore pipeline:
    //   1. validateRestoreJson()   — reject on bad JSON / no valid buttons
    //   2. backup()                — snapshot current DB to backup file
    //   3. importJson()            — atomic swap + immediate save
    // Returns a RestoreResult; on failure the live DB is untouched.
    RestoreResult restore(const String& json);

    // Download the backup file as a pretty-printed JSON string.
    // Returns empty string if no backup exists.
    String backupJson() const;

    // True if a backup file exists on LittleFS.
    bool hasBackup() const { return LittleFS.exists(DB_BACKUP_FILE); }

private:
    std::vector<IRButton> _buttons;
    uint32_t              _nextId;
    portMUX_TYPE          _mux;

    // Fix 2: lazy-save state
    bool          _dirty;      // true = RAM has changes not yet flushed to flash
    unsigned long _dirtyMs;    // millis() when dirty flag was last set

    // Fix 1: auto-save state
    bool          _autoSave;
    bool          _loadAutoSave();
    bool          _saveAutoSave();

    uint32_t newId() { return _nextId++; }

    // Mark DB dirty; schedules a lazy flash write via loop().
    // Use instead of save() for normal mutations.
    void _markDirty();

    // Fix 3: count RAW-protocol buttons currently in DB.
    uint8_t _rawCount() const;
};

// ── Global singleton ─────────────────────────────────────────
extern IRDatabase irDB;
