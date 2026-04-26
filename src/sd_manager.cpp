// ============================================================
//  sd_manager.cpp  –  Optional SD card subsystem
//  v1.0.0  |  Graceful fallback when SD absent
// ============================================================
#include "sd_manager.h"
#include "ir_database.h"
#include "ota_manager.h"
#include "ir_transmitter.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ctime>

SdManager sdMgr;

// ── Constructor ──────────────────────────────────────────────
SdManager::SdManager()
    : _mounted(false),
      _spiBegun(false),
      _lastProbeMs(0),
      _probeFailCount(0),
      _remountIntervalMs(SD_REMOUNT_MIN_MS),
      _lastRemountMs(0),
      _logMux(portMUX_INITIALIZER_UNLOCKED),
      _logRingHead(0),
      _logRingLen(0),
      _lastLogFlushMs(0),
      _macroRunning(false),
      _macroStepIdx(0),
      _macroNextMs(0),
      _uploadOpen(false)
{
    memset(_logRing, 0, sizeof(_logRing));
}

// ── begin ────────────────────────────────────────────────────
// Called once from setup().  Non-blocking: if SD is absent or
// fails to init, we return immediately without hanging.
void SdManager::begin() {
    // SD uses a dedicated SPIClass(VSPI) instance so the global SPI object
    // remains available to other modules (NFC in SPI mode, etc.).
    // SD_CS is passed to SPI.begin() as hardware SS so the ESP32 SPI
    // peripheral drives it — SD.begin() then re-drives it via the library.
    if (!_spiBegun) {
        if (!_sdSpi) _sdSpi = new SPIClass(VSPI);
        _sdSpi->begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
        _spiBegun = true;
    }
    _mount();
}

// ── _mount ───────────────────────────────────────────────────
bool SdManager::_mount() {
    // Call SD.end() first to clear any stale SdFat state from a previous
    // failed or partial mount attempt. Without this, SD.begin() can return
    // false on retry even when a card is physically present.
    SD.end();

    // SD.begin() pulls CS low; if card is absent it returns false quickly.
    // Frequency capped at SD_SPI_FREQ (default 4 MHz) for reliability.
    if (!SD.begin(SD_CS_PIN, *_sdSpi, SD_SPI_FREQ)) {
        _mounted = false;
        // Don't log on every attempt — caller manages log rate
        return false;
    }

    // Quick sanity: reject CARD_NONE
    if (SD.cardType() == CARD_NONE) {
        SD.end();
        _mounted = false;
        return false;
    }

    _mounted             = true;
    _probeFailCount      = 0;             // reset debounce counter
    _remountIntervalMs   = SD_REMOUNT_MIN_MS; // reset back-off
    Serial.printf(DEBUG_TAG " [SD] Mounted OK  type=%s  size=%lluMB  free=%lluMB\n",
                  status().cardTypeStr.c_str(),
                  SD.totalBytes()  / (1024ULL * 1024ULL),
                  (SD.totalBytes() - SD.usedBytes()) / (1024ULL * 1024ULL));

    _ensureDirectories();
    log("SD mounted — IR Remote started");
    return true;
}

// ── _unmount ─────────────────────────────────────────────────
void SdManager::_unmount() {
    if (_mounted) {
        _flushLogRing();   // save pending log before unmount
        SD.end();
        _mounted = false;
        Serial.println(DEBUG_TAG " [SD] Unmounted.");
    }
}

// ── _ensureDirectories ───────────────────────────────────────
void SdManager::_ensureDirectories() {
    const char* dirs[] = {
        SD_DIR_IR_LIBRARY, SD_DIR_BACKUPS, SD_DIR_OTA,
        SD_DIR_MACROS,     SD_DIR_LOGS,    SD_DIR_ASSETS,
        SD_DIR_RAW_DUMPS,  SD_DIR_DEVICES, nullptr
    };
    for (const char** d = dirs; *d; ++d) {
        if (!SD.exists(*d)) {
            SD.mkdir(*d);
            Serial.printf(DEBUG_TAG " [SD] Created dir: %s\n", *d);
        }
    }
}

// ── loop ─────────────────────────────────────────────────────
void SdManager::loop() {
    unsigned long now = millis();

    // Hot-plug probe at SD_PROBE_INTERVAL when mounted.
    // When unmounted, _probeSd() manages its own back-off timer.
    if (now - _lastProbeMs >= SD_PROBE_INTERVAL) {
        _probeSd();
        _lastProbeMs = now;
    }

    if (!_mounted) return;

    // Flush log ring buffer to SD periodically
    if (now - _lastLogFlushMs >= SD_LOG_FLUSH_MS) {
        _flushLogRing();
        _lastLogFlushMs = now;
    }

    // Advance macro execution
    if (_macroRunning) _tickMacro();
}

// ── _probeSd ─────────────────────────────────────────────────
// Debounced hot-plug detection:
//   MOUNTED:   require SD_PROBE_FAIL_DEBOUNCE consecutive read failures
//              before unmounting — prevents transient SPI glitches from
//              causing flapping.
//   UNMOUNTED: use exponential back-off on remount attempts to avoid
//              spamming SD.begin() every 5 seconds when no card present.
void SdManager::_probeSd() {
    unsigned long now = millis();

    if (_mounted) {
        // Health-check via cardType() — direct SPI register read, no file handle.
        // SD.open("/") was unreliable on the ESP32 Arduino SD library:
        // f_opendir() can return a NULL handle even when the card IS present,
        // causing false "card removed" detections every 5 s.
        // cardType() == CARD_NONE is the correct and reliable presence check.
        bool cardPresent = (SD.cardType() != CARD_NONE);
        if (!cardPresent) {
            ++_probeFailCount;
            if (_probeFailCount >= SD_PROBE_FAIL_DEBOUNCE) {
                Serial.printf(DEBUG_TAG " [SD] %u consecutive cardType=NONE — unmounting.\n",
                              (unsigned)_probeFailCount);
                _probeFailCount = 0;
                _unmount();
                _remountIntervalMs = SD_REMOUNT_MIN_MS;
                _lastRemountMs     = now;
            }
            // else: transient — wait for next probe cycle
        } else {
            // Card still present — reset fail counter
            if (_probeFailCount > 0) {
                _probeFailCount = 0;
            }
        }
    } else {
        // Not mounted — try remount with back-off
        if ((now - _lastRemountMs) < _remountIntervalMs) return;
        _lastRemountMs = now;

        bool ok = _mount();
        if (!ok) {
            // Exponential back-off: 5s → 10s → 20s → … → 60s max
            _remountIntervalMs = min(_remountIntervalMs * 2, SD_REMOUNT_MAX_MS);
            Serial.printf(DEBUG_TAG " [SD] Not present. Next retry in %lus.\n",
                          _remountIntervalMs / 1000UL);
        }
        // On success, _mount() already resets _remountIntervalMs
    }
}

// ── status ───────────────────────────────────────────────────
SdStatus SdManager::status() const {
    SdStatus s;
    s.mounted    = _mounted;
    s.totalBytes = _mounted ? SD.totalBytes() : 0;
    s.usedBytes  = _mounted ? SD.usedBytes()  : 0;
    s.cardType   = _mounted ? (uint8_t)SD.cardType() : 0;
    switch (s.cardType) {
        case CARD_MMC:     s.cardTypeStr = "MMC";     break;
        case CARD_SD:      s.cardTypeStr = "SD";      break;
        case CARD_SDHC:    s.cardTypeStr = "SDHC";    break;
        case CARD_UNKNOWN: s.cardTypeStr = "Unknown"; break;
        default:           s.cardTypeStr = "None";    break;
    }
    return s;
}

// ── _safePath ────────────────────────────────────────────────
// Reject path traversal attempts (../ etc.)
bool SdManager::_safePath(const String& path) const {
    if (path.isEmpty())          return false;
    if (path.indexOf("..") >= 0) return false;  // no traversal
    if (!path.startsWith("/"))   return false;  // must be absolute
    if (path.length() > 128)     return false;  // sanity cap
    return true;
}

// ── _logTimestamp ────────────────────────────────────────────
String SdManager::_logTimestamp() const {
    time_t now = time(nullptr);
    if (now < 1700000000UL) {
        // NTP not synced yet — use uptime
        unsigned long s = millis() / 1000;
        char buf[24];
        snprintf(buf, sizeof(buf), "[up %02lu:%02lu:%02lu]",
                 s / 3600, (s % 3600) / 60, s % 60);
        return String(buf);
    }
    struct tm tmbuf, *t = localtime_r(&now, &tmbuf);
    char buf[24];
    snprintf(buf, sizeof(buf), "[%04d-%02d-%02d %02d:%02d:%02d]",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);
    return String(buf);
}

// ── log ──────────────────────────────────────────────────────
void SdManager::log(const String& message) { log(message.c_str()); }

void SdManager::log(const char* message) {
    if (!_mounted) return;

    // Build the log line: "TIMESTAMP message\n"
    String line = _logTimestamp() + " " + message + "\n";
    const char* lc = line.c_str();
    size_t len = line.length();
    if (len == 0) return;

    // Write into ring buffer under spinlock
    portENTER_CRITICAL(&_logMux);
    for (size_t i = 0; i < len; ++i) {
        _logRing[(_logRingHead + _logRingLen) % LOG_RING_SIZE] = lc[i];
        if (_logRingLen < LOG_RING_SIZE) {
            ++_logRingLen;
        } else {
            // Overwrite oldest byte
            _logRingHead = (_logRingHead + 1) % LOG_RING_SIZE;
        }
    }
    portEXIT_CRITICAL(&_logMux);

    // Also echo to Serial for debug visibility
    Serial.printf(DEBUG_TAG " [SD-LOG] %s", lc);
}

// ── _flushLogRing ─────────────────────────────────────────────
void SdManager::_flushLogRing() {
    if (!_mounted) return;

    // Drain ring buffer under lock
    char buf[LOG_RING_SIZE];
    size_t len = 0;
    portENTER_CRITICAL(&_logMux);
    len          = _logRingLen;
    size_t head  = _logRingHead;
    if (len > 0) {
        for (size_t i = 0; i < len; ++i)
            buf[i] = _logRing[(head + i) % LOG_RING_SIZE];
        _logRingLen  = 0;
        _logRingHead = 0;
    }
    portEXIT_CRITICAL(&_logMux);
    if (len == 0) return;

    // Rotate log if too large
    if (SD.exists(SD_LOG_FILE)) {
        File f = SD.open(SD_LOG_FILE, FILE_READ);
        if (f && f.size() >= SD_LOG_MAX_BYTES) {
            f.close();
            // Rename current → activity.log.1 (simple rotation)
            if (SD.exists("/logs/activity.log.1"))
                SD.remove("/logs/activity.log.1");
            SD.rename(SD_LOG_FILE, "/logs/activity.log.1");
        } else if (f) {
            f.close();
        }
    }

    File f = SD.open(SD_LOG_FILE, FILE_APPEND);
    if (!f) {
        Serial.println(DEBUG_TAG " [SD] ERROR: cannot open log file for append.");
        return;
    }
    f.write((const uint8_t*)buf, len);
    f.close();
}

// ── tailLog ──────────────────────────────────────────────────
String SdManager::tailLog(uint16_t lines) const {
    if (!_mounted) return "";
    if (!SD.exists(SD_LOG_FILE)) return "";

    File f = SD.open(SD_LOG_FILE, FILE_READ);
    if (!f) return "";

    size_t fileSize = f.size();
    if (fileSize == 0) { f.close(); return ""; }

    // Scan backwards to find the last `lines` newlines
    const size_t SCAN_BUF = 4096;
    size_t scanSize = (fileSize < SCAN_BUF) ? fileSize : SCAN_BUF;
    size_t seekPos  = fileSize - scanSize;
    f.seek(seekPos);

    String chunk = "";
    chunk.reserve(scanSize);
    while (f.available()) {
        char c = (char)f.read();
        chunk += c;
    }
    f.close();

    // Count newlines from the end and slice
    int found = 0;
    int pos   = (int)chunk.length() - 1;
    while (pos >= 0 && found <= (int)lines) {
        if (chunk[pos] == '\n') ++found;
        --pos;
    }
    if (pos < 0) pos = 0;
    // Advance past the newline character we stopped on
    else if (pos < (int)chunk.length() && chunk[pos] == '\n') ++pos;
    return chunk.substring(pos);
}

// ── listDir ──────────────────────────────────────────────────
std::vector<SdFileEntry> SdManager::listDir(const String& path) const {
    std::vector<SdFileEntry> result;
    if (!_mounted || !_safePath(path)) return result;

    File dir = SD.open(path);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return result;
    }

    // Normalise parent path for building fullPath
    String parent = path;
    if (!parent.endsWith("/")) parent += "/";
    if (parent == "//") parent = "/";

    File entry = dir.openNextFile();
    while (entry) {
        SdFileEntry fe;
        fe.name  = String(entry.name());
        // entry.name() returns the full path on ESP32 SD lib — strip to basename
        int lastSlash = fe.name.lastIndexOf('/');
        if (lastSlash >= 0) fe.name = fe.name.substring(lastSlash + 1);
        fe.isDir   = entry.isDirectory();
        fe.size    = entry.isDirectory() ? 0 : (size_t)entry.size();
        fe.modTime = (uint32_t)entry.getLastWrite();   // FAT32 last-write timestamp
        fe.fullPath = (parent == "/") ? ("/" + fe.name) : (parent + fe.name);
        if (!fe.name.isEmpty() && fe.name != "." && fe.name != "..") {
            result.push_back(fe);
        }
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();
    return result;
}

// ── deleteFile ───────────────────────────────────────────────
bool SdManager::deleteFile(const String& path) {
    if (!_mounted || !_safePath(path)) return false;
    if (!SD.exists(path)) return false;
    bool ok = SD.remove(path);
    if (ok) log(String("Deleted: ") + path);
    return ok;
}

// ── renameFile ───────────────────────────────────────────────
bool SdManager::renameFile(const String& from, const String& to) {
    if (!_mounted || !_safePath(from) || !_safePath(to)) return false;
    bool ok = SD.rename(from, to);
    if (ok) log(String("Renamed: ") + from + " → " + to);
    return ok;
}

// ── makeDir ──────────────────────────────────────────────────
bool SdManager::makeDir(const String& path) {
    if (!_mounted || !_safePath(path)) return false;
    return SD.mkdir(path);
}

// ── exists ───────────────────────────────────────────────────
bool SdManager::exists(const String& path) const {
    if (!_mounted) return false;
    return SD.exists(path);
}

// ── _copyFile ────────────────────────────────────────────────
// Copy a file between LittleFS and SD (or within either).
// srcOnSD / dstOnSD determine which FS each side uses.
bool SdManager::_copyFile(const String& src, const String& dst,
                           bool srcOnSD, bool dstOnSD) {
    File srcFile = srcOnSD ? SD.open(src, FILE_READ)
                           : LittleFS.open(src, "r");
    if (!srcFile) {
        Serial.printf(DEBUG_TAG " [SD] _copyFile: cannot open src %s\n", src.c_str());
        return false;
    }

    File dstFile = dstOnSD ? SD.open(dst, FILE_WRITE)
                           : LittleFS.open(dst, "w");
    if (!dstFile) {
        srcFile.close();
        Serial.printf(DEBUG_TAG " [SD] _copyFile: cannot open dst %s\n", dst.c_str());
        return false;
    }

    uint8_t copyBuf[512];
    size_t total = 0;
    while (srcFile.available()) {
        size_t n = srcFile.read(copyBuf, sizeof(copyBuf));
        if (n == 0) break;
        if (dstFile.write(copyBuf, n) != n) {
            Serial.printf(DEBUG_TAG " [SD] _copyFile: write error at offset %u\n",
                          (unsigned)total);
            srcFile.close(); dstFile.close();
            return false;
        }
        total += n;
    }
    srcFile.close(); dstFile.close();
    Serial.printf(DEBUG_TAG " [SD] Copied %s → %s (%u bytes)\n",
                  src.c_str(), dst.c_str(), (unsigned)total);
    return true;
}

// ── exportIRLibrary ──────────────────────────────────────────
bool SdManager::exportIRLibrary(const String& name) {
    if (!_mounted) return false;
    String path = String(SD_DIR_IR_LIBRARY) + "/" + name + ".json";
    if (!_safePath(path)) return false;

    // Use irDB streaming export — writes compact JSON
    String json = irDB.exportJson();
    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    f.print(json);
    f.close();

    log(String("IR library exported: ") + name + ".json  (" +
        String((unsigned)irDB.size()) + " buttons)");
    return true;
}

// ── importIRLibrary ──────────────────────────────────────────
bool SdManager::importIRLibrary(const String& name) {
    if (!_mounted) return false;
    String path = String(SD_DIR_IR_LIBRARY) + "/" + name + ".json";
    if (!_safePath(path) || !SD.exists(path)) return false;

    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    // Read into String — IR library files should be < 200 KB
    // (128 buttons × ~1.5 KB RAW worst-case)
    String json = "";
    json.reserve(min((size_t)f.size(), (size_t)65536));  // cap: corrupted FAT guard
    while (f.available()) {
        char c = (char)f.read();
        json += c;
        if (json.length() > 65536) break;  // safety cap
    }
    f.close();

    bool ok = irDB.importJson(json);
    if (ok) log(String("IR library imported: ") + name + ".json");
    return ok;
}

// ── listIRLibraries ──────────────────────────────────────────
std::vector<String> SdManager::listIRLibraries() const {
    std::vector<String> result;
    if (!_mounted) return result;
    auto entries = listDir(SD_DIR_IR_LIBRARY);
    for (const auto& e : entries) {
        if (!e.isDir && e.name.endsWith(".json"))
            result.push_back(e.name);
    }
    return result;
}

// ── saveRawDump ──────────────────────────────────────────────
bool SdManager::saveRawDump(const String& name, const uint16_t* data,
                              size_t len, uint16_t freqKHz) {
    if (!_mounted || !data || len == 0) return false;
    String path = String(SD_DIR_RAW_DUMPS) + "/" + name + ".csv";
    if (!_safePath(path)) return false;

    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    f.printf("# RAW IR dump: %s  freq=%u kHz  samples=%u\n",
             name.c_str(), (unsigned)freqKHz, (unsigned)len);
    f.print("idx,us\n");
    for (size_t i = 0; i < len; ++i) {
        f.printf("%u,%u\n", (unsigned)i, (unsigned)data[i]);
    }
    f.close();
    log(String("Raw IR dump saved: ") + name + ".csv");
    return true;
}

// ── backupToSD ───────────────────────────────────────────────
// Copies all LittleFS config files to /sd/backups/<tag>/
bool SdManager::backupToSD(const String& tag) {
    if (!_mounted) return false;
    String dir = String(SD_DIR_BACKUPS) + "/" + tag;
    if (!_safePath(dir)) return false;
    SD.mkdir(dir);

    // Files to backup from LittleFS
    const char* files[] = {
        "/ir_database.json", "/config.json", "/groups.json",
        "/schedules.json",   "/ntp_config.json", "/ir_autosave.json",
        "/ir_pins.json",     nullptr
    };

    uint8_t copied = 0;
    for (const char** fp = files; *fp; ++fp) {
        if (!LittleFS.exists(*fp)) continue;
        String dst = dir + *fp;
        if (_copyFile(*fp, dst, false, true)) ++copied;
    }
    log(String("Backup created: ") + tag + " (" + String(copied) + " files)");
    return (copied > 0);
}

// ── restoreFromSD ─────────────────────────────────────────────
bool SdManager::restoreFromSD(const String& tag) {
    if (!_mounted) return false;
    String dir = String(SD_DIR_BACKUPS) + "/" + tag;
    if (!_safePath(dir) || !SD.exists(dir)) return false;

    auto entries = listDir(dir);
    uint8_t restored = 0;
    for (const auto& e : entries) {
        if (e.isDir) continue;
        String src = dir + "/" + e.name;
        String dst = "/" + e.name;
        if (_copyFile(src, dst, true, false)) ++restored;
    }
    log(String("Restored from SD: ") + tag + " (" + String(restored) + " files)");
    return (restored > 0);
}

// ── listBackups ──────────────────────────────────────────────
std::vector<String> SdManager::listBackups() const {
    std::vector<String> result;
    if (!_mounted) return result;
    auto entries = listDir(SD_DIR_BACKUPS);
    for (const auto& e : entries) {
        if (e.isDir) result.push_back(e.name);
    }
    return result;
}

// ── otaFileExists / otaFileSize ──────────────────────────────
bool SdManager::otaFileExists(const String& target) const {
    if (!_mounted) return false;
    const char* path = (target == "filesystem") ? SD_OTA_FILESYSTEM : SD_OTA_FIRMWARE;
    return SD.exists(path);
}
size_t SdManager::otaFileSize(const String& target) const {
    if (!_mounted) return 0;
    const char* path = (target == "filesystem") ? SD_OTA_FILESYSTEM : SD_OTA_FIRMWARE;
    File f = SD.open(path, FILE_READ);
    if (!f) return 0;
    size_t sz = f.size();
    f.close();
    return sz;
}

// ── triggerOtaFromSD ─────────────────────────────────────────
// Streams the SD OTA file through OtaManager::handleUploadChunk()
// — identical code path to browser OTA, same Update library,
// same safety guarantees.
bool SdManager::triggerOtaFromSD(const String& target) {
    if (!_mounted) {
        Serial.println(DEBUG_TAG " [SD-OTA] SD not available.");
        return false;
    }
    if (otaMgr.isUpdating()) {
        Serial.println(DEBUG_TAG " [SD-OTA] OTA already in progress.");
        return false;
    }

    const char* path = (target == "filesystem") ? SD_OTA_FILESYSTEM : SD_OTA_FIRMWARE;
    if (!SD.exists(path)) {
        Serial.printf(DEBUG_TAG " [SD-OTA] File not found: %s\n", path);
        return false;
    }

    File f = SD.open(path, FILE_READ);
    if (!f) {
        Serial.printf(DEBUG_TAG " [SD-OTA] Cannot open: %s\n", path);
        return false;
    }

    size_t fileSize = f.size();
    Serial.printf(DEBUG_TAG " [SD-OTA] Starting %s OTA from SD (%u bytes)\n",
                  target.c_str(), (unsigned)fileSize);
    log(String("SD-OTA started: ") + target + " (" + String((unsigned)fileSize) + " bytes)");

    static uint8_t otaBuf[512];
    size_t offset = 0;
    bool   ok     = true;

    while (f.available() && ok) {
        size_t n = f.read(otaBuf, sizeof(otaBuf));
        if (n == 0) break;
        bool isFinal = (offset + n >= fileSize);
        otaMgr.handleUploadChunk(target, otaBuf, n, offset, fileSize, isFinal);
        if (isFinal && otaMgr.lastError().length() > 0) {
            ok = false;
        }
        offset += n;
        yield();  // prevent WDT reset during large file streaming
    }
    f.close();

    if (!ok) {
        log(String("SD-OTA FAILED: ") + otaMgr.lastError());
        Serial.printf(DEBUG_TAG " [SD-OTA] FAILED: %s\n", otaMgr.lastError().c_str());
        return false;
    }

    log("SD-OTA: image accepted — rebooting");
    return true;
}


// ── getFileInfo ───────────────────────────────────────────────
SdFileEntry SdManager::getFileInfo(const String& path) const {
    SdFileEntry fe;
    fe.size    = 0;
    fe.isDir   = false;
    fe.modTime = 0;
    fe.fullPath = path;
    if (!_mounted || !_safePath(path)) return fe;
    File f = SD.open(path);
    if (!f) return fe;
    fe.isDir   = f.isDirectory();
    fe.size    = fe.isDir ? 0 : (size_t)f.size();
    fe.modTime = (uint32_t)f.getLastWrite();
    int sl = path.lastIndexOf('/');
    fe.name    = (sl >= 0) ? path.substring(sl + 1) : path;
    if (fe.name.isEmpty()) fe.name = "/";   // guard for root path
    f.close();
    return fe;
}

// ── readTextFile ──────────────────────────────────────────────
// Returns up to maxBytes of a text file for preview.
// Stops early and appends "[...truncated]" if file is larger.
String SdManager::readTextFile(const String& path, size_t maxBytes) const {
    if (!_mounted || !_safePath(path)) return "";
    File f = SD.open(path, FILE_READ);
    if (!f) return "";
    size_t fileSize = (size_t)f.size();
    bool truncated  = fileSize > maxBytes;
    size_t readSize = truncated ? maxBytes : fileSize;
    String out = "";
    out.reserve(min(readSize + 20, (size_t)8212));
    size_t read = 0;
    while (f.available() && read < readSize) {
        char c = (char)f.read();
        out += c;
        ++read;
    }
    f.close();
    if (truncated) out += "\n[...truncated — showing first " +
                          String((unsigned)maxBytes) + " bytes of " +
                          String((unsigned)fileSize) + " total]";
    return out;
}

// ── copyFileSd ────────────────────────────────────────────────
// Copy a file within the SD card (both src and dst are on SD).
bool SdManager::copyFileSd(const String& src, const String& dst) {
    if (!_mounted || !_safePath(src) || !_safePath(dst)) return false;
    if (src == dst) return false;  // no-op
    // Refuse to overwrite a directory with a file
    if (SD.exists(dst)) {
        File existing = SD.open(dst);
        if (existing && existing.isDirectory()) { existing.close(); return false; }
        if (existing) existing.close();
    }
    bool ok = _copyFile(src, dst, true, true);
    if (ok) log(String("Copied: ") + src + " → " + dst);
    return ok;
}

// ── moveFile ──────────────────────────────────────────────────
// Move = copy + delete source (SD.rename is atomic but only works
// within the same filesystem and partition; using copy+delete also
// works across different SD directories on FAT32).
bool SdManager::moveFile(const String& src, const String& dst) {
    if (!_mounted || !_safePath(src) || !_safePath(dst)) return false;
    if (src == dst) return false;
    // Try atomic rename first (faster, no data duplication)
    if (SD.rename(src, dst)) {
        log(String("Moved (rename): ") + src + " → " + dst);
        return true;
    }
    // Fall back to copy + delete
    if (!copyFileSd(src, dst)) return false;
    if (!SD.remove(src)) {
        // Copy succeeded but source delete failed — delete the copy
        SD.remove(dst);
        return false;
    }
    log(String("Moved (copy+del): ") + src + " → " + dst);
    return true;
}

// ── deleteRecursive ───────────────────────────────────────────
// Deletes a file OR an entire directory tree safely.
bool SdManager::deleteRecursive(const String& path) {
    if (!_mounted || !_safePath(path)) return false;
    if (!SD.exists(path)) return false;
    bool ok = _deleteRecursiveImpl(path);
    if (ok) log(String("Deleted (recursive): ") + path);
    return ok;
}

bool SdManager::_deleteRecursiveImpl(const String& path) {
    File f = SD.open(path);
    if (!f) return false;
    if (!f.isDirectory()) {
        f.close();
        return SD.remove(path);
    }
    // Directory: first delete all children
    File child = f.openNextFile();
    while (child) {
        String childPath = String(child.name());
        child.close();
        if (!_deleteRecursiveImpl(childPath)) {
            f.close();
            return false;
        }
        child = f.openNextFile();
    }
    f.close();
    return SD.rmdir(path);
}

// ── formatCard ────────────────────────────────────────────────
// WARNING: Destroys ALL data on the SD card.
// Only call after explicit user confirmation.
// Unmounts, formats FAT32, then remounts.
bool SdManager::formatCard() {
    if (!_mounted) return false;
    Serial.println(DEBUG_TAG " [SD] FORMAT REQUESTED — unmounting before format");
    _flushLogRing();   // save pending log first
    SD.end();
    _mounted = false;

    // ESP32 Arduino SD library does not expose a format() function.
    // We use the underlying SDFS object from the SD library.
    // formatSD() is available in framework-arduinoespressif32 >= 2.0.5
    // via #include <SDFS.h> — but it's not always exposed in the standard SD.h.
    // Safest cross-version approach: use the low-level SD card write to
    // zero the MBR/FAT, then call SD.begin() which will re-init.
    // LIMITATION: This is a basic format — not as thorough as mkfs.
    // For a full FAT32 format, the user should use a PC tool.
    //
    // We use SDFS.format() which IS available in the ESP32 Arduino framework:
    if (!SD.begin(SD_CS_PIN, *_sdSpi, SD_SPI_FREQ)) {
        Serial.println(DEBUG_TAG " [SD] Format: SD.begin() failed");
        return false;
    }
    // SD.format() available since framework-arduinoespressif32 2.0.14
    // Returns true on success
    bool ok = false;
#if defined(SD_FORMAT_AVAILABLE)
    ok = SD.format();
#else
    // Fallback: not available in this framework version
    // Return false and tell the user to format with a PC tool
    Serial.println(DEBUG_TAG " [SD] format() not available in this framework version");
    SD.end();
    return false;
#endif
    if (ok) {
        Serial.println(DEBUG_TAG " [SD] Format complete — remounting");
        _mount();
    } else {
        Serial.println(DEBUG_TAG " [SD] Format failed");
        SD.end();
    }
    return ok;
}

// ── queueMacro ───────────────────────────────────────────────
bool SdManager::queueMacro(const String& filename) {
    if (!_mounted) return false;
    if (_macroRunning) return false;  // one at a time

    String path = String(SD_DIR_MACROS) + "/" + filename;
    if (!_safePath(path) || !SD.exists(path)) return false;

    File f = SD.open(path, FILE_READ);
    if (!f) return false;

    String json = "";
    json.reserve(min((size_t)f.size(), (size_t)4096));
    while (f.available() && json.length() < 4096) json += (char)f.read();
    f.close();

    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) return false;

    JsonArrayConst steps = doc["steps"].as<JsonArrayConst>();
    _macroSteps.clear();
    uint8_t count = 0;
    for (JsonObjectConst s : steps) {
        if (count >= SD_MACRO_MAX_STEPS) break;
        MacroStep ms;
        ms.buttonId     = s["buttonId"]     | (uint32_t)0;
        ms.delayAfterMs = s["delayAfterMs"] | (uint32_t)100;
        if (ms.buttonId > 0) {
            _macroSteps.push_back(ms);
            ++count;
        }
    }

    if (_macroSteps.empty()) return false;

    _macroStepIdx = 0;
    _macroNextMs  = millis();
    _macroRunning = true;

    log(String("Macro queued: ") + filename + " (" + String(count) + " steps)");
    Serial.printf(DEBUG_TAG " [SD] Macro '%s' queued (%u steps)\n",
                  filename.c_str(), (unsigned)count);
    return true;
}

// ── _tickMacro ───────────────────────────────────────────────
// Called from loop() — fires one step at a time without blocking.
void SdManager::_tickMacro() {
    if (!_macroRunning) return;
    if (millis() < _macroNextMs) return;

    if (_macroStepIdx >= _macroSteps.size()) {
        _macroRunning = false;
        log("Macro completed.");
        return;
    }

    const MacroStep& step = _macroSteps[_macroStepIdx];
    IRButton btn = irDB.findById(step.buttonId);
    if (btn.id) {
        irTransmitter.transmit(btn);
        Serial.printf(DEBUG_TAG " [SD-Macro] Step %u: TX btn=%u '%s'\n",
                      (unsigned)_macroStepIdx, btn.id, btn.name.c_str());
    } else {
        Serial.printf(DEBUG_TAG " [SD-Macro] Step %u: btn %u not found — skipped\n",
                      (unsigned)_macroStepIdx, step.buttonId);
    }

    ++_macroStepIdx;
    _macroNextMs = millis() + step.delayAfterMs;

    if (_macroStepIdx >= _macroSteps.size()) {
        _macroRunning = false;
        log("Macro completed.");
    }
}

// ── listMacros ───────────────────────────────────────────────
std::vector<String> SdManager::listMacros() const {
    std::vector<String> result;
    if (!_mounted) return result;
    auto entries = listDir(SD_DIR_MACROS);
    for (const auto& e : entries) {
        if (!e.isDir && e.name.endsWith(".json"))
            result.push_back(e.name);
    }
    return result;
}

// ── assetPath / hasAsset ─────────────────────────────────────
String SdManager::assetPath(const String& name) const {
    if (!_mounted) return "";
    String path = String(SD_DIR_ASSETS) + "/" + name;
    if (!_safePath(path)) return "";
    return SD.exists(path) ? path : "";
}
bool SdManager::hasAsset(const String& name) const {
    return !assetPath(name).isEmpty();
}

// ── listDeviceProfiles ───────────────────────────────────────
std::vector<String> SdManager::listDeviceProfiles() const {
    std::vector<String> result;
    if (!_mounted) return result;
    auto entries = listDir(SD_DIR_DEVICES);
    for (const auto& e : entries) {
        if (!e.isDir && e.name.endsWith(".json"))
            result.push_back(e.name);
    }
    return result;
}

// ── readDeviceProfile ────────────────────────────────────────
String SdManager::readDeviceProfile(const String& name) const {
    if (!_mounted) return "";
    String path = String(SD_DIR_DEVICES) + "/" + name;
    if (!_safePath(path) || !SD.exists(path)) return "";
    File f = SD.open(path, FILE_READ);
    if (!f) return "";
    String out = "";
    out.reserve(min((size_t)f.size(), (size_t)16384));
    while (f.available() && out.length() < 16384) out += (char)f.read();
    f.close();
    return out;
}

// ── openForRead / openForWrite ───────────────────────────────
File SdManager::openForRead(const String& path) const {
    if (!_mounted || !_safePath(path)) return File();
    return SD.open(path, FILE_READ);
}
File SdManager::openForWrite(const String& path) {
    if (!_mounted || !_safePath(path)) return File();
    return SD.open(path, FILE_WRITE);
}

// ── Upload helpers ────────────────────────────────────────────
bool SdManager::beginUpload(const String& path) {
    if (!_mounted || !_safePath(path)) return false;
    if (_uploadOpen) abortUpload();
    _uploadFile = SD.open(path, FILE_WRITE);
    _uploadOpen = (bool)_uploadFile;
    return _uploadOpen;
}
bool SdManager::writeUploadChunk(const uint8_t* data, size_t len) {
    if (!_uploadOpen || !_uploadFile) return false;
    return _uploadFile.write(data, len) == len;
}
bool SdManager::endUpload() {
    if (!_uploadOpen) return false;
    size_t sz = _uploadFile.size();
    _uploadFile.close();
    _uploadOpen = false;
    log(String("Upload complete: ") + String((unsigned)sz) + " bytes");
    return true;
}
void SdManager::abortUpload() {
    if (_uploadFile) _uploadFile.close();
    _uploadOpen = false;
}
