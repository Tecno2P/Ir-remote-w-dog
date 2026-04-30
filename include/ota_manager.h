#pragma once
// ============================================================
//  ota_manager.h  –  OTA firmware + filesystem update v3.0
//
//  v3.0 — Permanent OTA storage-full fix:
//    + Uses real IDF esp_ota_get_next_update_partition() to detect
//      missing/corrupt OTA partition (was using getFreeSketchSpace
//      which always returns partition SIZE not usable space)
//    + Real partition size read from esp_partition_t directly
//    + LittleFS deep cleanup: audit log, log archives, temp files
//    + OTA slot state validated via esp_ota_get_state_partition()
//    + Rollback support: marks app valid on success, invalid on fail
//    + Correct progress % using real declared file size
//    + Chunk watchdog: abort stale uploads
//    + Concurrent upload guard
//    + IR ISR paused during flash writes
//    + LittleFS re-mounted after abort
// ============================================================
#include <Arduino.h>
#include <Update.h>
#include <LittleFS.h>
#include <functional>
#include "config.h"

// IDF OTA partition APIs — available in all espressif32 framework versions
#include <esp_ota_ops.h>
#include <esp_partition.h>

// Abort upload if no chunk received within this window (dropped TCP)
#ifndef OTA_CHUNK_TIMEOUT_MS
  #define OTA_CHUNK_TIMEOUT_MS   60000UL
#endif

// Hard cap: reject firmware larger than this regardless of partition size.
// Set to 1600 KB — 64 KB margin under 1664 KB partition for safety.
#ifndef OTA_MAX_FIRMWARE_BYTES
  #define OTA_MAX_FIRMWARE_BYTES  (1600UL * 1024UL)
#endif

// Minimum LittleFS free bytes required before allowing OTA.
// Ensures at minimum the filesystem is not completely full.
#ifndef OTA_MIN_FS_FREE_BYTES
  #define OTA_MIN_FS_FREE_BYTES   (32UL * 1024UL)
#endif

using OtaProgressCallback = std::function<void(size_t done, size_t total)>;
using OtaEndCallback      = std::function<void(bool success, const String& msg)>;

class OtaManager {
public:
    OtaManager();

    void onProgress(OtaProgressCallback cb) { _progressCb = cb; }
    void onEnd     (OtaEndCallback      cb) { _endCb      = cb; }

    // Called per-chunk by web server upload handler.
    // target = "firmware" | "filesystem"
    // total  = declared file size in bytes (0 if unknown/chunked encoding)
    void handleUploadChunk(const String& target,
                           uint8_t*      data,
                           size_t        len,
                           size_t        index,
                           size_t        total,
                           bool          final);

    bool          isUpdating()     const { return _updating; }
    bool          restartPending() const { return _restartPending; }
    const String& lastError()      const { return _lastError; }

    // Call from main loop() every iteration — aborts stale uploads
    void tickWatchdog();

    // Clear error state without reboot — allows retrying after failure
    void clearError();

    // Real OTA partition free bytes via IDF API
    // Returns 0 if no valid OTA partition exists
    size_t freeOtaBytes() const;

    // Real OTA partition total size via IDF API
    size_t otaPartitionSize() const;

    // LittleFS free bytes (separate from OTA flash)
    size_t fsFreeBytes() const;

private:
    OtaProgressCallback _progressCb;
    OtaEndCallback      _endCb;
    volatile bool       _updating;
    volatile bool       _restartPending;
    String              _lastError;
    uint8_t             _lastPct;
    unsigned long       _lastChunkMs;
    size_t              _totalReceived;
    size_t              _declaredSize;

    void beginUpdate  (const String& target, size_t declaredSize);
    void finishUpdate ();
    void abortUpdate  (const String& reason);

    // Deep LittleFS cleanup — frees log archives, audit log, temp files
    void _cleanLittleFSBeforeOta();

    // Validate: partition exists, firmware fits, FS not critically full
    // Returns false + calls abortUpdate() with clear message if check fails
    bool _validateBeforeUpdate(size_t declaredSize, const String& target);
};

extern OtaManager otaMgr;
