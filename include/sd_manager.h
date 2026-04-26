#pragma once
// ============================================================
//  sd_manager.h  –  Optional SD card subsystem
//  v1.0.0  |  ESP32-WROOM-32  |  VSPI bus
//
//  DESIGN PRINCIPLES
//  ─────────────────
//  1. ALWAYS OPTIONAL — if SD is absent or fails to mount,
//     every existing LittleFS/internal feature continues to
//     work exactly as before.  No crashes, no boot delays,
//     no broken UI.
//
//  2. HOT-PLUG AWARE — isAvailable() re-probes the bus on
//     every call (with a debounce timer) so insertion and
//     removal are handled gracefully at runtime.
//
//  3. RAM CONSERVATIVE — streaming I/O throughout; large
//     file transfers never buffer the whole file in RAM.
//     Log writes use a small fixed ring buffer, flushed
//     periodically from loop().
//
//  4. OTA-SAFE — SD OTA flashes via the same Update library
//     path as browser OTA, reusing OtaManager internals.
//     Browser OTA is NOT affected whether SD is present or not.
//
//  WIRING (VSPI — default SPI bus on ESP32)
//  ────────────────────────────────────────
//  SD Card │  ESP32 GPIO
//  ────────┼────────────────────────────────────
//  VCC     │  3.3 V  (use 3.3 V-compatible module; 5 V SD
//           │          modules must have level-shifter)
//  GND     │  GND
//  MOSI    │  GPIO 23  (SD_MOSI_PIN)
//  MISO    │  GPIO 19  (SD_MISO_PIN)
//  SCK     │  GPIO 18  (SD_SCK_PIN)
//  CS      │  GPIO  4  (SD_CS_PIN) — configurable
//
//  SD CARD FORMAT
//  ──────────────
//  • FAT32, up to 32 GB SDHC tested
//  • exFAT not supported by the ESP32 Arduino SD library
//  • Recommended: format with SD Card Formatter (sdcard.org)
//    or: mkfs.fat -F 32 /dev/sdX
//
//  DIRECTORY STRUCTURE CREATED ON FIRST MOUNT
//  ──────────────────────────────────────────
//  /sd/
//  ├── ir_library/      IR button JSON libraries
//  ├── backups/         Config + LittleFS snapshots
//  ├── ota/             firmware.bin / littlefs.bin for SD-OTA
//  ├── macros/          IR macro scripts (.json)
//  ├── logs/            activity.log (rolling)
//  ├── assets/          Large HTML/CSS/JS served over HTTP
//  ├── raw_dumps/       Captured RAW IR timing arrays
//  └── devices/         TV/AC/device JSON profiles
//
//  MACRO SCRIPT FORMAT  (/sd/macros/*.json)
//  ─────────────────────────────────────────
//  {
//    "name": "TV On + HDMI1",
//    "steps": [
//      {"buttonId": 12, "delayAfterMs": 500},
//      {"buttonId": 15, "delayAfterMs": 200},
//      {"buttonId": 15, "delayAfterMs": 0}
//    ]
//  }
// ============================================================
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <vector>
#include <functional>
#include "config.h"

// ── SD hardware pin defaults (overridable in config.h) ───────
#ifndef SD_CS_PIN
  #define SD_CS_PIN   4    // GPIO4  — safe, no boot-strap conflict
#endif
#ifndef SD_MOSI_PIN
  #define SD_MOSI_PIN 23   // VSPI MOSI
#endif
#ifndef SD_MISO_PIN
  #define SD_MISO_PIN 19   // VSPI MISO
#endif
#ifndef SD_SCK_PIN
  #define SD_SCK_PIN  18   // VSPI SCK
#endif
#ifndef SD_SPI_FREQ
  #define SD_SPI_FREQ 4000000UL   // 4 MHz — conservative; bump to 20 MHz if stable
#endif

// ── SD directory structure ───────────────────────────────────
#define SD_DIR_IR_LIBRARY  "/ir_library"
#define SD_DIR_BACKUPS     "/backups"
#define SD_DIR_OTA         "/ota"
#define SD_DIR_MACROS      "/macros"
#define SD_DIR_LOGS        "/logs"
#define SD_DIR_ASSETS      "/assets"
#define SD_DIR_RAW_DUMPS   "/raw_dumps"
#define SD_DIR_DEVICES     "/devices"

#define SD_LOG_FILE        "/logs/activity.log"
#define SD_OTA_FIRMWARE    "/ota/firmware.bin"
#define SD_OTA_FILESYSTEM  "/ota/littlefs.bin"

// ── Log / OTA limits ─────────────────────────────────────────
#define SD_LOG_MAX_BYTES   (512UL * 1024UL)  // 512 KB max log before rotation
#define SD_LOG_FLUSH_MS    10000UL           // flush log ring every 10 s
#define SD_PROBE_INTERVAL      15000UL  // probe interval when mounted (15s — less SPI bus load)
#define SD_PROBE_FAIL_DEBOUNCE 3        // consecutive failures before unmount (~45s total)
#define SD_REMOUNT_MIN_MS      10000UL  // first remount retry delay (10s)
#define SD_REMOUNT_MAX_MS      60000UL  // max remount retry back-off (60s)
#define SD_MACRO_MAX_STEPS 64                // max steps in one macro

// ── Macro step ───────────────────────────────────────────────
struct MacroStep {
    uint32_t buttonId;
    uint32_t delayAfterMs;
};

// ── SD status snapshot (for /api/sd/status) ──────────────────
struct SdStatus {
    bool     mounted;
    uint64_t totalBytes;
    uint64_t usedBytes;
    uint8_t  cardType;     // CARD_NONE/MMC/SD/SDHC/UNKNOWN
    String   cardTypeStr;
};

// ── File entry (for /api/sd/ls) ──────────────────────────────
struct SdFileEntry {
    String   name;
    bool     isDir;
    size_t   size;
    uint32_t modTime;   // Unix timestamp (0 if unknown — FAT32 has limited time support)
    String   fullPath;  // Absolute path for convenience
};

// ─────────────────────────────────────────────────────────────
class SdManager {
public:
    SdManager();

    // ── Lifecycle ─────────────────────────────────────────────
    // Non-blocking: returns immediately if SD absent.
    // Safe to call every boot whether or not SD is wired.
    void begin();

    // Call from main loop() every iteration.
    // Handles: log flush, macro execution, hot-plug probe.
    void loop();

    // ── Status ────────────────────────────────────────────────
    bool     isAvailable() const { return _mounted; }
    SdStatus status()      const;

    // ── Logging ───────────────────────────────────────────────
    // Thread-safe via portMUX.  Queues into a small ring buffer
    // and flushes to SD asynchronously from loop().
    void log(const String& message);
    void log(const char*   message);

    // Return last N lines of the log file as a String.
    // Returns "" if SD not available.
    String  tailLog(uint16_t lines = 50) const;

    // ── File Manager ──────────────────────────────────────────
    std::vector<SdFileEntry> listDir(const String& path) const;
    bool     deleteFile(const String& path);   // single file
    bool     deleteRecursive(const String& path); // file or dir tree
    bool     renameFile(const String& from, const String& to);
    bool     copyFileSd(const String& src, const String& dst); // SD-to-SD copy
    bool     moveFile(const String& src, const String& dst);   // copy + delete
    bool     makeDir(const String& path);
    bool     exists(const String& path) const;
    SdFileEntry  getFileInfo(const String& path) const;  // metadata (size, time)
    String   readTextFile(const String& path, size_t maxBytes = 8192) const; // preview
    bool     formatCard();   // full format — use with confirmation only!

    // ── IR Library ────────────────────────────────────────────
    // Export current irDB to /sd/ir_library/<name>.json
    bool     exportIRLibrary(const String& name);
    // Import from /sd/ir_library/<name>.json into irDB
    bool     importIRLibrary(const String& name);
    // List available IR library files
    std::vector<String> listIRLibraries() const;

    // ── Raw IR Dump ───────────────────────────────────────────
    // Append a raw IR timing array to /sd/raw_dumps/<filename>
    bool     saveRawDump(const String& name,
                         const uint16_t* data, size_t len,
                         uint16_t freqKHz = 38);

    // ── Config Backup / Restore ───────────────────────────────
    // Copy all LittleFS config files to /sd/backups/<tag>/
    bool     backupToSD(const String& tag);
    // Restore /sd/backups/<tag>/ back to LittleFS
    bool     restoreFromSD(const String& tag);
    std::vector<String> listBackups() const;

    // ── OTA from SD ───────────────────────────────────────────
    // Reads SD_OTA_FIRMWARE or SD_OTA_FILESYSTEM and flashes it.
    // Calls otaMgr.handleUploadChunk() internally — same code
    // path as browser OTA, same safety guarantees.
    // Returns false immediately if SD not available or file missing.
    bool     triggerOtaFromSD(const String& target);  // "firmware" or "filesystem"
    bool     otaFileExists(const String& target) const;
    size_t   otaFileSize(const String& target)   const;

    // ── Macros ────────────────────────────────────────────────
    // Run a macro JSON file from /sd/macros/
    // Execution is deferred to loop() to avoid blocking the async task.
    bool     queueMacro(const String& filename);
    bool     isMacroRunning() const { return _macroRunning; }
    std::vector<String> listMacros() const;

    // ── Asset serving ─────────────────────────────────────────
    // Returns the path to an asset on SD if it exists,
    // empty string otherwise.  Used by web_server to prefer
    // SD assets over LittleFS when SD is available.
    String   assetPath(const String& name) const;
    bool     hasAsset(const String& name)  const;

    // ── Device profiles ───────────────────────────────────────
    std::vector<String> listDeviceProfiles() const;
    String   readDeviceProfile(const String& name) const;

    // ── Streaming helpers for web server ─────────────────────
    // Open a file on SD for streaming via ESPAsyncWebServer.
    // Caller is responsible for sending the response.
    File     openForRead(const String& path) const;
    File     openForWrite(const String& path);

    // Write an incoming HTTP upload chunk to SD.
    // Manages an open write handle across chunks.
    bool     beginUpload(const String& path);
    bool     writeUploadChunk(const uint8_t* data, size_t len);
    bool     endUpload();
    void     abortUpload();

private:
    bool          _mounted;
    bool          _spiBegun;        // SPI.begin() called once; never reset
    SPIClass*     _sdSpi = nullptr;  // dedicated VSPI instance for SD card
    unsigned long _lastProbeMs;     // last probe timestamp (millis)
    uint8_t       _probeFailCount;  // consecutive probe failures (debounce)
    unsigned long _remountIntervalMs; // current back-off interval for remount
    unsigned long _lastRemountMs;   // timestamp of last remount attempt
    portMUX_TYPE  _logMux;

    // Log ring buffer (fits in DRAM, not IRAM)
    static constexpr size_t LOG_RING_SIZE = 1024;
    char          _logRing[LOG_RING_SIZE];
    size_t        _logRingHead;
    size_t        _logRingLen;
    unsigned long _lastLogFlushMs;

    // Macro execution state
    bool              _macroRunning;
    std::vector<MacroStep> _macroSteps;
    size_t            _macroStepIdx;
    unsigned long     _macroNextMs;   // millis() when next step fires

    // Upload state
    File   _uploadFile;
    bool   _uploadOpen;

    // Private helpers
    bool   _mount();
    void   _unmount();
    void   _ensureDirectories();
    void   _flushLogRing();
    void   _tickMacro();
    void   _probeSd();
    bool   _safePath(const String& path) const;  // reject path traversal
    String _logTimestamp()               const;
    bool   _copyFile(const String& src, const String& dst,
                     bool srcOnSD, bool dstOnSD);
    bool   _deleteRecursiveImpl(const String& path);  // recursive helper
};

extern SdManager sdMgr;
