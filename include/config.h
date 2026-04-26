#pragma once
// ============================================================
//  config.h  –  Compile-time tunables for IR Remote Web GUI
//
//  v2.0.0 — Added:
//    + Button Groups / Remote Presets
//    + Scheduled IR Transmission (NTP + cron-style)
//    + IR Repeat / Timing Controls
//    + Raw IR Signal Visualization support
//    + System Status Dashboard
//    + AP+STA dual-mode Wi-Fi (reliable)
//    + Wi-Fi network scanner
// ============================================================

// ── Firmware version ─────────────────────────────────────────
// Defined via -DFIRMWARE_VERSION in platformio.ini build_flags
// (do NOT redefine here — would cause "macro redefined" warning)
#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION "5.0.0"
#endif

// ── Wi-Fi Access Point defaults ─────────────────────────────
#define DEFAULT_AP_SSID       "IR-Remote"
#define DEFAULT_AP_PASS       "irremote123"   // min 8 chars, or "" for open
#define DEFAULT_AP_CHANNEL    1
#define DEFAULT_AP_HIDDEN     false
#define DEFAULT_AP_MAX_CONN   4
#define AP_IP                 "192.168.4.1"

// ── Wi-Fi Station mode (optional) ───────────────────────────
#define DEFAULT_STA_SSID      ""
#define DEFAULT_STA_PASS      ""
#define STA_CONNECT_TIMEOUT   20000   // ms – increased for reliability
#define STA_RECONNECT_INTERVAL 30000  // ms – retry if disconnected
#define WIFI_SCAN_MAX_RESULTS 20      // max SSIDs returned

// ── Hardware pins ────────────────────────────────────────────
#define IR_RECV_PIN           14
#define IR_SEND_PIN           27
#define IR_RECV_ENABLE_PULLUP true

// ── IR capture settings ──────────────────────────────────────
#define IR_RECV_BUFFER_SIZE   1024
#define IR_CAPTURE_TIMEOUT_MS 15
#define IR_MIN_UNKNOWN_SIZE   12
#define IR_DEBOUNCE_MS        250

// ── IR transmit settings ─────────────────────────────────────
#define IR_DEFAULT_FREQ_KHZ   38
#define IR_DEFAULT_DUTY_CYCLE 33
#define IR_SEND_REPEATS       1    // FIX: was 0 — 1 repeat frame = reliable single-press for NEC/Samsung/LG

// ── IR Repeat / Timing defaults ──────────────────────────────
#define IR_DEFAULT_REPEAT_COUNT  1    // FIX: was 4 — device responded only after 3-4 presses.
                                           // 1 send + protocol repeat frames is sufficient for most devices.
                                           // User can increase per-button if needed.
#define IR_DEFAULT_REPEAT_DELAY  40   // ms between repeat sends (40ms = standard inter-frame gap)
#define IR_MAX_REPEAT_COUNT      20
#define IR_MAX_REPEAT_DELAY      2000 // ms

// ── Button Groups ────────────────────────────────────────────
#define MAX_GROUPS            16
#define GROUP_NAME_MAX        32
#define DEFAULT_GROUP_NAME    "Default"

// ── Scheduler ────────────────────────────────────────────────
#define MAX_SCHEDULES         32
#define SCHEDULER_CHECK_INTERVAL_MS 10000  // check every 10 seconds
#define NTP_SERVER1           "pool.ntp.org"
#define NTP_SERVER2           "time.nist.gov"
#define NTP_TIMEZONE_OFFSET   19800  // IST = UTC+5:30 (India Standard Time)
#define NTP_DST_OFFSET        0      // DST offset in seconds (user configurable)
#define NTP_SYNC_INTERVAL_MS  3600000UL  // re-sync every hour

// ── Web server ───────────────────────────────────────────────
#define HTTP_PORT             80
#define WS_PATH               "/ws"
#define WS_MAX_CLIENTS        4
#define WS_QUEUE_MAX          64     // max pending WS messages before oldest dropped
#define HTTP_MAX_BODY         65536

// ── Filesystem ───────────────────────────────────────────────
#define DB_FILE               "/ir_database.json"
#define DB_BACKUP_FILE        "/ir_database_backup.json"  // Backup & Restore
#define DB_TMP_FILE           "/ir_db.tmp"                // atomic write temp
#define CFG_FILE              "/config.json"
#define GROUPS_FILE           "/groups.json"
#define SCHEDULES_FILE        "/schedules.json"
#define MAX_BUTTONS           128     // increased from 64
#define MAX_RAW_EDGES         512

// ── Backup & Restore limits ───────────────────────────────────
// Maximum JSON body accepted for a restore upload (64 KB).
// Larger files are rejected with HTTP 413 before parsing.
#define DB_RESTORE_MAX_BYTES  65536UL

// ── IR Storage improvements (Fix 1–4) ───────────────────────
//
// Fix 1: Auto-save received IR signals
//   When enabled, every decoded IR signal is checked against
//   the DB and saved automatically if it is a new code.
#define IR_AUTO_SAVE_DEFAULT  false        // off by default; user enables via API
#define IR_AUTO_SAVE_FILE     "/ir_autosave.json"
//
// Fix 2: Lazy save — batch flash writes to reduce wear
//   Instead of writing to flash on every add/update/delete,
//   a dirty flag is set and the DB is flushed after this many
//   milliseconds of inactivity.
#define DB_LAZY_SAVE_MS       5000UL       // flush dirty DB 5 s after last change
//
// Fix 3: RAW button memory guard
//   Each RAW button holds up to MAX_RAW_EDGES uint16_t values
//   (512 x 2 = 1 KB).  Allowing all 128 buttons to be RAW
//   would consume ~136 KB heap — unsafe on ESP32.
#define MAX_RAW_BUTTONS       16           // max simultaneous RAW buttons in DB
//
// Fix 4: Streaming JSON save (see ir_database.cpp save())
//   save() serialises one button at a time directly to the
//   LittleFS file instead of building a full in-RAM JsonDocument.
//   Peak extra RAM during save ~1 x IRButton (~1 KB)
//   instead of N x IRButton (up to 136 KB for 128 RAW buttons).

// ── Serial debug ─────────────────────────────────────────────
#define SERIAL_BAUD           115200
#define DEBUG_TAG             "[IR]"

// ── SD Card (optional) ───────────────────────────────────────
// VSPI bus — wiring: MOSI=23, MISO=19, SCK=18, CS=4
// All SD features are automatically disabled if card is absent.
// Wrapped in #ifndef so -D flags in platformio.ini take precedence
// without triggering "macro redefined" warnings.
#ifndef SD_CS_PIN
  #define SD_CS_PIN          4    // Chip-select; change if wired differently
#endif
#ifndef SD_MOSI_PIN
  #define SD_MOSI_PIN        23
#endif
#ifndef SD_MISO_PIN
  #define SD_MISO_PIN        19
#endif
#ifndef SD_SCK_PIN
  #define SD_SCK_PIN         18
#endif
#ifndef SD_SPI_FREQ
  #define SD_SPI_FREQ        4000000UL   // 4 MHz initial; can increase to 20 MHz
#endif

// ── Internal Macros (LittleFS — no SD required) ── v2.2.0 ────
// Up to MACRO_MAX_INTERNAL macros stored directly in LittleFS.
// Each macro is a JSON file at /macros/<name>.json
// Same format as SD macros so they are interchangeable.
#define MACRO_DIR              "/macros"
#define MACRO_MAX_INTERNAL     16       // max macros in LittleFS
#define MACRO_NAME_MAX         32       // max filename chars (without .json)
#define MACRO_MAX_STEPS        32       // max steps per LittleFS macro
#define MACRO_STEP_MAX_DELAY   10000    // ms max delay between steps
