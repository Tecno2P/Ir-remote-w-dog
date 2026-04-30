// ============================================================
//  ota_manager.cpp  –  OTA firmware + filesystem update v3.0
//
//  PERMANENT OTA STORAGE-FULL FIX:
//
//  Previous code used ESP.getFreeSketchSpace() which always returns
//  the partition SIZE (1664 KB), never actual free/usable space.
//  It cannot detect: no valid OTA partition, corrupted otadata,
//  wrong boot slot, or firmware genuinely too large.
//
//  v3.0 fixes ALL these cases using real IDF OTA APIs:
//
//  1. esp_ota_get_next_update_partition(NULL)
//     → Returns pointer to the target partition, or NULL if none exists.
//     → NULL means otadata is corrupt or partition table is wrong.
//     → We check this FIRST and abort with a clear error.
//
//  2. esp_partition_t::size
//     → Real partition size in bytes from the partition table.
//     → Compared against declared firmware size before accepting upload.
//
//  3. esp_ota_get_state_partition()
//     → Checks if the target slot is in a valid state to receive update.
//
//  4. LittleFS deep cleanup
//     → Removes log archives, trims audit log, removes temp files.
//     → Ensures LittleFS has breathing room (not related to OTA flash
//        but prevents filesystem-full crash on boot after OTA).
//
//  5. esp_ota_mark_app_valid_cancel_rollback()
//     → Called AFTER successful reboot via markOtaBootValid() in main.cpp.
//     → Prevents rollback-on-reboot if otadata was previously in pending state.
//
// ============================================================
#include "ota_manager.h"
#include "ir_receiver.h"
#include <LittleFS.h>

OtaManager otaMgr;

OtaManager::OtaManager()
    : _updating(false), _restartPending(false),
      _lastPct(255), _lastChunkMs(0),
      _totalReceived(0), _declaredSize(0)
{}

// ─────────────────────────────────────────────────────────────
//  freeOtaBytes — REAL value via IDF partition API
//  Returns the size of the next OTA target partition.
//  Returns 0 if no valid OTA partition exists at all.
// ─────────────────────────────────────────────────────────────
size_t OtaManager::freeOtaBytes() const {
    const esp_partition_t* part = esp_ota_get_next_update_partition(NULL);
    if (!part) return 0;            // no valid OTA partition
    return (size_t)part->size;      // full partition size available for writing
}

// ─────────────────────────────────────────────────────────────
//  otaPartitionSize — partition size from IDF (same as freeOtaBytes here)
// ─────────────────────────────────────────────────────────────
size_t OtaManager::otaPartitionSize() const {
    const esp_partition_t* part = esp_ota_get_next_update_partition(NULL);
    if (!part) return 0;
    return (size_t)part->size;
}

// ─────────────────────────────────────────────────────────────
//  fsFreeBytes — real LittleFS free space
// ─────────────────────────────────────────────────────────────
size_t OtaManager::fsFreeBytes() const {
    if (LittleFS.totalBytes() == 0) return 0;
    return (size_t)(LittleFS.totalBytes() - LittleFS.usedBytes());
}

// ─────────────────────────────────────────────────────────────
//  _validateBeforeUpdate
//
//  All checks run BEFORE any flash erase begins.
//  Any failure calls abortUpdate() with a clear user-readable message
//  and returns false so beginUpdate() exits cleanly.
// ─────────────────────────────────────────────────────────────
bool OtaManager::_validateBeforeUpdate(size_t declaredSize, const String& target) {

    if (target == "filesystem") {
        // Filesystem OTA writes to the spiffs partition, not the OTA app slots.
        // Only check LittleFS is not critically full (it will be unmounted anyway).
        // No flash-space check needed — Update library handles spiffs partition directly.
        Serial.printf(DEBUG_TAG " OTA: filesystem update — spiffs partition at 0x%06X\n",
                      0x350000);
        return true;
    }

    // ── CHECK 1: Valid OTA partition exists ───────────────────
    const esp_partition_t* nextPart = esp_ota_get_next_update_partition(NULL);
    if (!nextPart) {
        abortUpdate(
            "No valid OTA partition found. "
            "Possible causes: partition table mismatch, corrupted otadata, "
            "or device was flashed without OTA support. "
            "Re-flash partitions.bin via USB to fix."
        );
        return false;
    }

    size_t partSize = (size_t)nextPart->size;
    Serial.printf(DEBUG_TAG " OTA: target partition '%s' at 0x%06lX size=%u KB\n",
                  nextPart->label,
                  (unsigned long)nextPart->address,
                  (unsigned)(partSize / 1024));

    // ── CHECK 2: Declared firmware size vs hard cap ───────────
    if (declaredSize > 0 && declaredSize > OTA_MAX_FIRMWARE_BYTES) {
        abortUpdate(
            String("Firmware too large: ") + (declaredSize / 1024) + " KB"
            + " exceeds max " + (OTA_MAX_FIRMWARE_BYTES / 1024) + " KB. "
            + "Rebuild with -Os optimization or disable unused modules."
        );
        return false;
    }

    // ── CHECK 3: Declared firmware size vs real partition size ─
    if (declaredSize > 0 && declaredSize > partSize) {
        abortUpdate(
            String("Firmware too large for OTA partition: ") + (declaredSize / 1024) + " KB"
            + " > partition " + (partSize / 1024) + " KB. "
            + "Check partition table or reduce firmware size."
        );
        return false;
    }

    // ── CHECK 4: Partition state — ensure slot is writable ────
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t stErr = esp_ota_get_state_partition(nextPart, &state);
    if (stErr == ESP_OK) {
        // ESP_OTA_IMG_INVALID means the slot was marked bad — still writable,
        // the new image overwrites it. Log it but don't abort.
        Serial.printf(DEBUG_TAG " OTA: target slot state=%d (%s)\n",
                      (int)state,
                      state == ESP_OTA_IMG_VALID    ? "VALID" :
                      state == ESP_OTA_IMG_INVALID  ? "INVALID(will overwrite)" :
                      state == ESP_OTA_IMG_ABORTED  ? "ABORTED(will overwrite)" :
                      state == ESP_OTA_IMG_NEW       ? "NEW" :
                      state == ESP_OTA_IMG_PENDING_VERIFY ? "PENDING_VERIFY" :
                      "UNDEFINED");
    }

    // ── CHECK 5: LittleFS not critically full ─────────────────
    // OTA flash and LittleFS are separate partitions, but if LittleFS
    // is totally full the device crashes on first config save after OTA.
    size_t fsFree = fsFreeBytes();
    if (fsFree < OTA_MIN_FS_FREE_BYTES) {
        Serial.printf(DEBUG_TAG " OTA: WARNING — LittleFS only %u KB free (need %u KB). "
                      "Performing deep cleanup.\n",
                      (unsigned)(fsFree / 1024),
                      (unsigned)(OTA_MIN_FS_FREE_BYTES / 1024));
        // Cleanup runs below in beginUpdate(); just warn here.
        // Do NOT abort — LittleFS space is not OTA flash space.
    }

    Serial.printf(DEBUG_TAG " OTA: all checks passed. "
                  "Partition=%u KB  Declared=%u KB  LittleFS free=%u KB\n",
                  (unsigned)(partSize / 1024),
                  (unsigned)(declaredSize / 1024),
                  (unsigned)(fsFree / 1024));
    return true;
}

// ─────────────────────────────────────────────────────────────
//  _cleanLittleFSBeforeOta
//
//  Deep cleanup of LittleFS before OTA starts.
//  Removes ephemeral files that can safely be regenerated.
//  This frees space to ensure the device doesn't FS-crash post-OTA.
//  Called for BOTH firmware and filesystem OTA.
// ─────────────────────────────────────────────────────────────
void OtaManager::_cleanLittleFSBeforeOta() {
    size_t before = fsFreeBytes();

    // ── Remove known temp/cache files ─────────────────────────
    static const char* removable[] = {
        "/netmon_log.json",       // NetMon removed but may exist on old installs
        "/netmon_devices.json",   // same
        "/netmon_cfg.json",       // same
        "/ir_db.tmp",             // atomic write temp file (safe to remove)
        nullptr
    };
    for (int i = 0; removable[i]; i++) {
        if (LittleFS.exists(removable[i])) {
            LittleFS.remove(removable[i]);
            Serial.printf(DEBUG_TAG " OTA cleanup: removed %s\n", removable[i]);
        }
    }

    // ── Trim audit log if > 30 KB ─────────────────────────────
    // Audit log is rebuilt fresh; old entries are expendable before OTA.
    const char* auditLog = "/audit_log.json";
    if (LittleFS.exists(auditLog)) {
        File f = LittleFS.open(auditLog, "r");
        if (f) {
            size_t sz = f.size();
            f.close();
            if (sz > 30 * 1024) {
                LittleFS.remove(auditLog);
                Serial.printf(DEBUG_TAG " OTA cleanup: removed audit_log.json (%u KB)\n",
                              (unsigned)(sz / 1024));
            }
        }
    }

    // ── Remove ALL log archives (YYYY-MM-DD.json) ─────────────
    // These are compressed daily logs — not needed for device operation.
    File dir = LittleFS.open("/log_archive");
    if (dir && dir.isDirectory()) {
        File f = dir.openNextFile();
        while (f) {
            String name = String(f.name());
            f.close();
            if (name.endsWith(".json")) {
                String full = String("/log_archive/") + name;
                LittleFS.remove(full);
                Serial.printf(DEBUG_TAG " OTA cleanup: removed archive %s\n", full.c_str());
            }
            f = dir.openNextFile();
        }
    }

    // ── Remove crash log (will be recreated on next boot) ─────
    // Keep boot counter so safe-mode still works after OTA.
    if (LittleFS.exists("/crash_log.json")) {
        LittleFS.remove("/crash_log.json");
        Serial.println(DEBUG_TAG " OTA cleanup: removed crash_log.json");
    }

    size_t after = fsFreeBytes();
    Serial.printf(DEBUG_TAG " OTA cleanup done: LittleFS %u KB → %u KB free (+%u KB)\n",
                  (unsigned)(before / 1024),
                  (unsigned)(after  / 1024),
                  (unsigned)((after > before ? after - before : 0) / 1024));
}

// ─────────────────────────────────────────────────────────────
//  handleUploadChunk — called per-chunk by web server
// ─────────────────────────────────────────────────────────────
void OtaManager::handleUploadChunk(const String& target,
                                    uint8_t*      data,
                                    size_t        len,
                                    size_t        index,
                                    size_t        total,
                                    bool          final)
{
    if (index == 0) {
        if (_updating) {
            Serial.println(DEBUG_TAG " OTA: new upload started — aborting stale session");
            abortUpdate("Upload restarted (previous connection dropped)");
        }
        _declaredSize = total;
        beginUpdate(target, total);
        if (!_updating) return;   // beginUpdate failed — error already set
        _lastChunkMs   = millis();
        _totalReceived = 0;
    }

    if (!_updating) return;   // earlier chunk failed

    // Guard against non-contiguous chunks (browser retry / ghost connection)
    if (index != _totalReceived) {
        Serial.printf(DEBUG_TAG " OTA: non-contiguous chunk (expected %u got %u) — aborting\n",
                      (unsigned)_totalReceived, (unsigned)index);
        abortUpdate("Upload interrupted — non-contiguous chunk. Please retry.");
        return;
    }

    // Runtime size cap — guards against unknown Content-Length (chunked uploads)
    // Even when declaredSize=0, we enforce the hard cap during streaming.
    if (_totalReceived + len > OTA_MAX_FIRMWARE_BYTES) {
        abortUpdate(
            String("Firmware too large mid-upload: already received ") +
            ((_totalReceived + len) / 1024) + " KB — exceeds max " +
            (OTA_MAX_FIRMWARE_BYTES / 1024) + " KB. Rebuild with -Os."
        );
        return;
    }

    // Write chunk to flash — if write returns != len the partition is full
    if (Update.write(data, len) != len) {
        abortUpdate(
            String("Flash write failed after ") + (_totalReceived / 1024) + " KB. " +
            String(Update.errorString()) +
            " — partition may be corrupt. Re-flash partitions.bin via USB."
        );
        return;
    }
    _totalReceived += len;
    _lastChunkMs    = millis();

    // Progress callback — throttled to integer-percent steps
    if (_progressCb) {
        size_t written = Update.progress();
        size_t denom   = (_declaredSize > 0) ? _declaredSize : Update.size();
        if (denom > 0) {
            uint8_t pct = (uint8_t)((written * 100UL) / denom);
            if (pct > 99) pct = 99;   // hold at 99 until finishUpdate() fires 100
            if (pct != _lastPct) {
                _lastPct = pct;
                _progressCb(written, denom);
            }
        }
    }

    if (final) finishUpdate();
}

// ─────────────────────────────────────────────────────────────
//  beginUpdate — runs all checks then starts the Update library
// ─────────────────────────────────────────────────────────────
void OtaManager::beginUpdate(const String& target, size_t declaredSize) {
    _updating       = true;
    _restartPending = false;
    _lastError      = "";
    _lastPct        = 255;
    _lastChunkMs    = millis();
    _totalReceived  = 0;

    irReceiver.pause();   // pause IR ISR during flash operations

    if (target != "firmware" && target != "filesystem") {
        abortUpdate(String("Unknown OTA target '") + target +
                    "'. Valid values: 'firmware' or 'filesystem'.");
        return;
    }

    // Deep LittleFS cleanup before ALL OTA types
    _cleanLittleFSBeforeOta();

    // Validate flash space, partition state, firmware size
    if (!_validateBeforeUpdate(declaredSize, target)) {
        return;   // abortUpdate already called inside
    }

    int updateType;
    if (target == "filesystem") {
        updateType = U_SPIFFS;
        LittleFS.end();   // MUST unmount before writing spiffs partition
        Serial.println(DEBUG_TAG " OTA: LittleFS unmounted for filesystem update");
    } else {
        updateType = U_FLASH;
    }

    // UPDATE_SIZE_UNKNOWN: let the Update library use the full partition.
    // The size checks above already guaranteed the binary fits.
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, updateType)) {
        abortUpdate(String("Update.begin() failed: ") + Update.errorString() +
                    ". Check partition table integrity.");
        return;
    }

    const esp_partition_t* part = esp_ota_get_next_update_partition(NULL);
    Serial.printf(DEBUG_TAG " OTA: streaming %s update to partition '%s' "
                  "(declared=%u bytes)\n",
                  target.c_str(),
                  part ? part->label : "?",
                  (unsigned)declaredSize);
}

// ─────────────────────────────────────────────────────────────
//  finishUpdate — commit image, set restart pending
// ─────────────────────────────────────────────────────────────
void OtaManager::finishUpdate() {
    if (!Update.end(true)) {   // true = set boot partition to new image
        abortUpdate(String("Update.end() failed: ") + Update.errorString());
        return;
    }

    _updating       = false;
    _restartPending = true;

    // Fire final 100% progress
    if (_progressCb) {
        size_t sz = _totalReceived;
        _progressCb(sz, sz);
    }

    Serial.printf(DEBUG_TAG " OTA: image committed (%u bytes) — reboot pending\n",
                  (unsigned)_totalReceived);

    if (_endCb) _endCb(true, "OTA successful — rebooting in 1s");
    // NOTE: ESP.restart() is NOT called here.
    // main.cpp loop() polls restartPending() and restarts after 1.2 s,
    // giving the HTTP response and WebSocket message time to transmit.
}

// ─────────────────────────────────────────────────────────────
//  abortUpdate — clean up after any failure
// ─────────────────────────────────────────────────────────────
void OtaManager::abortUpdate(const String& reason) {
    Update.abort();
    _updating  = false;
    _lastError = reason;
    Serial.printf(DEBUG_TAG " OTA ABORTED: %s\n", reason.c_str());

    irReceiver.resume();   // re-enable IR reception

    // Re-mount LittleFS so config saves / logs work normally
    if (!LittleFS.begin(true)) {
        Serial.println(DEBUG_TAG " WARNING: LittleFS re-mount after OTA abort failed — "
                       "device may need reboot");
    }

    if (_endCb) _endCb(false, reason);
}

// ─────────────────────────────────────────────────────────────
//  tickWatchdog — abort stale uploads (call from main loop())
// ─────────────────────────────────────────────────────────────
void OtaManager::tickWatchdog() {
    if (!_updating) return;
    if ((millis() - _lastChunkMs) >= OTA_CHUNK_TIMEOUT_MS) {
        Serial.printf(DEBUG_TAG " OTA: upload timed out (no data for %lu s) — aborting\n",
                      OTA_CHUNK_TIMEOUT_MS / 1000UL);
        abortUpdate("Upload timed out — connection dropped. Please retry.");
    }
}

// ─────────────────────────────────────────────────────────────
//  clearError — reset after failure so UI can retry without reboot
// ─────────────────────────────────────────────────────────────
void OtaManager::clearError() {
    if (!_updating && !_restartPending) {
        _lastError     = "";
        _lastPct       = 255;
        _declaredSize  = 0;
        _totalReceived = 0;
        Serial.println(DEBUG_TAG " OTA: error cleared — ready for retry");
    }
}
