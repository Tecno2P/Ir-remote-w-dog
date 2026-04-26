// ============================================================
//  main.cpp  –  IR Remote Web GUI  v5.0.0  |  ESP32-WROOM-32
//
//  v5.0.0 build:
//    + 5 AC protocols: DAIKIN, MITSUBISHI_AC, WHYNTER, HAIER_AC, COOLIX
//    + IRButton icon + color fields
//    + Scheduler per-entry repeatCount / repeatDelay
//    + Internal LittleFS macros (MacroManager — no SD needed)
// ============================================================
#include <Arduino.h>
#include <LittleFS.h>
#include <esp_log.h>
#include <esp_ota_ops.h>     // esp_ota_mark_app_valid_cancel_rollback()
#include "config.h"
#include "gpio_config.h"
#include "ota_manager.h"
#include "ir_database.h"
#include "ir_receiver.h"
#include "ir_transmitter.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "group_manager.h"
#include "scheduler.h"
#include "sd_manager.h"
#include "macro_manager.h"
#include "nfc_module.h"
#include "rfid_module.h"
#include "subghz_module.h"
#include "nrf24_module.h"
#include "system_module.h"
#include "ai_assistant.h"        // Multi-Provider AI Integration
#include "audit_manager.h"          // Batch 1: Audit Trail
#include "rule_manager.h"           // Batch 2: IF-THEN Automation
#include "notification_manager.h"   // Batch 2: Telegram + WhatsApp
#include "auth_manager.h"           // Batch 3: Authentication
#include "watchdog_manager.h"       // Batch 3: Self-Healing Watchdog
#include "log_rotation.h"           // Batch 4: Log Rotation + CSV Export

static void initFilesystem();
static void ensureDefaultFiles();
static void onIRReceived(const IRButton& btn);
static void onScheduleFire(const ScheduleEntry& entry);
static void printBanner();

// FIX: removed static — web_server.cpp accesses this via extern for safe restart
portMUX_TYPE s_restartMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t s_restartAt = 0;

static unsigned long s_lastStatusBroadcast = 0;
#define STATUS_BROADCAST_INTERVAL_MS 5000

// ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(300);
    printBanner();

    // Suppress VFS "does not exist" spam from ESPAsyncWebServer
    esp_log_level_set("vfs_api", ESP_LOG_ERROR);
    esp_log_level_set("vfs",     ESP_LOG_ERROR);

    // ── LittleFS (always required) ────────────────────────────
    initFilesystem();

    // ── SD Card (optional — non-blocking if absent) ───────────
    // begin() returns immediately if no card is detected.
    // All existing features work unchanged without SD.
    sdMgr.begin();

    // ── IR Database ───────────────────────────────────────────
    irDB.begin();

    // ── Groups ────────────────────────────────────────────────
    groupMgr.begin();

    // ── Internal Macros (LittleFS — no SD needed) ─────────────
    macroMgr.onTransmit([](uint32_t buttonId) {
        IRButton btn = irDB.findById(buttonId);
        if (btn.id) irTransmitter.transmit(btn);
    });
    macroMgr.begin();

    // ── New modules (Tasks 7-11) ─────────────────────────────
    nfcModule.begin();
    rfidModule.begin();
    subGhzModule.begin();
    nrf24Module.begin();
    sysModule.begin();

    // ── Audit Trail (Batch 1) ─────────────────────────────────
    auditMgr.begin();

    // ── Notifications (Batch 2) ───────────────────────────────
    notifyMgr.begin();

    // ── Rule Engine (Batch 2) ─────────────────────────────────
    ruleMgr.onIrTransmit([](uint32_t buttonId) {
        IRButton btn = irDB.findById(buttonId);
        if (btn.id) {
            irTransmitter.transmit(btn);
            auditMgr.logIrTx(btn.name, btn.id);
        }
    });
    ruleMgr.onMacroRun([](const String& name) {
        macroMgr.run(name);
        auditMgr.logMacro(name, true);
    });
    ruleMgr.onNotify([](const String& msg, bool tg, bool wa) {
        notifyMgr.send(msg, tg, wa);
    });
    ruleMgr.onBuzzer([](uint8_t times) {
        // Buzzer handled inside notifyMgr
        for (uint8_t i = 0; i < times; i++) {
            notifyMgr.notify("");   // empty msg = buzzer only
        }
    });
    ruleMgr.begin();

    // ── Boot notification ─────────────────────────────────────
    notifyMgr.onBoot();
    ruleMgr.triggerBoot();

    // ── Auth (Batch 3) ────────────────────────────────────────
    authMgr.begin();

    // ── Watchdog (Batch 3) ────────────────────────────────────
    wdtMgr.begin();
    aiAssistant.begin();       // AI: loads provider+model from NVS

    // ── Log Rotation (Batch 4) ────────────────────────────────
    logRotMgr.begin();

    // ── WiFi (AP+STA dual-mode with event handler) ────────────
    wifiMgr.begin();

    // ── Load persisted IR pin config ──────────────────────────
    IrPinConfig irPins;
    wifiMgr.loadIrPins(irPins);

    // ── OTA callbacks ─────────────────────────────────────────
    otaMgr.onProgress([](size_t done, size_t total) {
        webUI.broadcastOtaProgress(done, total);
    });
    otaMgr.onEnd([](bool success, const String& msg) {
        webUI.broadcastOtaResult(success, msg);
        if (success) { taskENTER_CRITICAL(&s_restartMux); s_restartAt = (uint32_t)(millis() + 1200); taskEXIT_CRITICAL(&s_restartMux); }
    });

    // ── IR Receiver ───────────────────────────────────────────
    irReceiver.onReceive(onIRReceived);
    irReceiver.begin(irPins.recvPin);

    // ── IR Transmitter ────────────────────────────────────────
    irTransmitter.begin(irPins);

    // ── Web Server ────────────────────────────────────────────
    webUI.begin();
    webUI.startCaptivePortal();  // Batch 3: DNS redirect on AP mode

    // ── Scheduler ────────────────────────────────────────────
    scheduler.onFire(onScheduleFire);
    scheduler.begin();

    Serial.printf(DEBUG_TAG " Ready v%s  AP: http://%s\n",
                  FIRMWARE_VERSION, wifiMgr.apIP().c_str());
    Serial.printf(DEBUG_TAG " RX=GPIO%d  TX-active=%d  Groups=%u  Schedules=%u  Heap=%u\n",
                  irReceiver.activePin(),
                  irTransmitter.activeCount(),
                  (unsigned)groupMgr.size(),
                  (unsigned)scheduler.size(),
                  ESP.getFreeHeap());
    Serial.printf(DEBUG_TAG " SD: %s\n",
                  sdMgr.isAvailable() ? "MOUNTED" : "not present");

    if (sdMgr.isAvailable()) {
        SdStatus ss = sdMgr.status();
        Serial.printf(DEBUG_TAG " SD: %s  %lluMB total  %lluMB free\n",
                      ss.cardTypeStr.c_str(),
                      ss.totalBytes / (1024ULL * 1024ULL),
                      (ss.totalBytes - ss.usedBytes) / (1024ULL * 1024ULL));
    }

    // All modules initialised successfully — reset boot-failure counter.
    // This must be the LAST call in setup() so only a full clean boot clears it.
    wdtMgr.markBootSuccess();

    // Tell IDF this boot is valid — cancels any pending OTA rollback.
    // Without this, after an OTA update the device could roll back to the
    // previous firmware on the next reboot if the boot isn't confirmed.
    // esp_ota_mark_app_valid_cancel_rollback() is a no-op if we are already
    // running from the confirmed slot (normal non-OTA boot).
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.println(DEBUG_TAG " OTA: boot confirmed — rollback cancelled");
}

// ─────────────────────────────────────────────────────────────
void loop() {
    taskENTER_CRITICAL(&s_restartMux); uint32_t _ra = s_restartAt; taskEXIT_CRITICAL(&s_restartMux);
    if (_ra != 0 && millis() >= _ra) {  // fire when future timestamp reached
        taskENTER_CRITICAL(&s_restartMux); s_restartAt = 0; taskEXIT_CRITICAL(&s_restartMux);
        Serial.println(DEBUG_TAG " Restarting for OTA...");
        ESP.restart();
    }
    if (!otaMgr.isUpdating() && !otaMgr.restartPending()) {
        irReceiver.loop();
    }
    wifiMgr.loop();
    scheduler.loop();
    webUI.loop();
    irDB.loop();

    // SD loop: hot-plug probe, log flush, macro step tick
    sdMgr.loop();

    // Internal macro tick (LittleFS macros — no SD needed)
    macroMgr.loop();
    nfcModule.loop();
    rfidModule.loop();
    subGhzModule.loop();
    nrf24Module.loop();
    sysModule.loop();
    auditMgr.loop();   // Batch 1
    notifyMgr.loop();  // Batch 2
    ruleMgr.loop();    // Batch 2
    authMgr.loop();    // Batch 3: expire sessions
    wdtMgr.loop();     // Batch 3: watchdog feed + health check
    webUI.loopCaptivePortal(); // Batch 3: DNS captive portal
    logRotMgr.loop();  // Batch 4: auto log rotation

    // OTA watchdog: abort stale uploads if TCP connection was dropped
    otaMgr.tickWatchdog();

    if (millis() - s_lastStatusBroadcast >= STATUS_BROADCAST_INTERVAL_MS) {
        s_lastStatusBroadcast = millis();
        webUI.broadcastStatus();
    }
    yield();
}

// ─────────────────────────────────────────────────────────────
//  LittleFS init + default file creation
// ─────────────────────────────────────────────────────────────
static void initFilesystem() {
    if (!LittleFS.begin(true)) {
        Serial.println(DEBUG_TAG " FATAL: LittleFS mount+format failed.");
        return;
    }
    Serial.printf(DEBUG_TAG " LittleFS OK: total=%uKB  used=%uKB\n",
                  (unsigned)(LittleFS.totalBytes() / 1024),
                  (unsigned)(LittleFS.usedBytes()  / 1024));
    ensureDefaultFiles();
}

static void ensureFile(const char* path, const char* defaultContent) {
    if (LittleFS.exists(path)) return;
    File f = LittleFS.open(path, "w");
    if (!f) {
        Serial.printf(DEBUG_TAG " ERROR: Cannot create %s\n", path);
        return;
    }
    f.print(defaultContent);
    f.close();
    Serial.printf(DEBUG_TAG " Created default: %s\n", path);
}

static void ensureDefaultFiles() {
    ensureFile("/ir_database.json",  "{\"buttons\":[]}");
    ensureFile("/ir_pins.json",
        "{\"recvPin\":14,\"emitCount\":1,"
        "\"emit\":["
        "{\"pin\":27,\"enabled\":true},"
        "{\"pin\":26,\"enabled\":false},"
        "{\"pin\":25,\"enabled\":false},"
        "{\"pin\":33,\"enabled\":false}"
        "]}");
    ensureFile(CFG_FILE,
        "{\"apSSID\":\"IR-Remote\",\"apPass\":\"irremote123\","
        "\"apChannel\":1,\"apHidden\":false,"
        "\"staSSID\":\"\",\"staPass\":\"\",\"staEnabled\":false}");
    ensureFile("/groups.json",      "{\"groups\":[]}");
    ensureFile("/schedules.json",   "{\"schedules\":[]}");
    ensureFile("/ntp_config.json",  "{\"tzOffset\":19800,\"dstOffset\":0}");
    ensureFile(IR_AUTO_SAVE_FILE,   "{\"autoSave\":false}");
}

// ─────────────────────────────────────────────────────────────
static void onIRReceived(const IRButton& btn) {
    webUI.broadcastIREvent(btn);
    auditMgr.logIrRx(protocolName(btn.protocol), String(btn.code, HEX));  // Batch 1
    ruleMgr.triggerIrReceived(btn.id, protocolName(btn.protocol));         // Batch 2

    if (!irDB.autoSaveEnabled()) return;

    IRButton copy = btn;
    uint32_t savedId = irDB.autoSaveReceived(copy);

    if (savedId) {
        Serial.printf(DEBUG_TAG " [AutoSave] Saved '%s' as id=%u\n",
                      copy.name.c_str(), savedId);
        webUI.broadcastMessage(String("Auto-saved: ") + copy.name);

        // Mirror auto-saved signal to SD raw dump if SD available
        if (sdMgr.isAvailable() && btn.protocol == IRProtocol::RAW &&
            !btn.rawData.empty()) {
            sdMgr.saveRawDump(copy.name, btn.rawData.data(),
                              btn.rawData.size(), btn.freqKHz);
        }
    } else {
        if (btn.protocol != IRProtocol::RAW) {
            for (const auto& b : irDB.buttons()) {
                if (b.protocol == btn.protocol && b.code == btn.code) {
                    Serial.printf(DEBUG_TAG " [AutoSave] Duplicate skipped: '%s' (id=%u)\n",
                                  b.name.c_str(), b.id);
                    break;
                }
            }
        }
    }
}

static void onScheduleFire(const ScheduleEntry& entry) {
    IRButton copy = irDB.findById(entry.buttonId);
    if (!copy.id) {
        Serial.printf(DEBUG_TAG " Scheduler: button %u not found\n", entry.buttonId);
        return;
    }

    // Honour schedule-level repeatCount (overrides button default if > 1)
    uint8_t  fireCount = (entry.repeatCount > 1) ? entry.repeatCount : copy.repeatCount;
    uint16_t fireDelay = (entry.repeatDelay > 0) ? entry.repeatDelay : copy.repeatDelay;

    Serial.printf(DEBUG_TAG " Scheduler TX: btn=%u '%s'  fires=%u  delay=%ums\n",
                  copy.id, copy.name.c_str(), fireCount, fireDelay);

    for (uint8_t i = 0; i < fireCount; ++i) {
        irTransmitter.transmit(copy);
        if (i + 1 < fireCount && fireDelay > 0) vTaskDelay(pdMS_TO_TICKS(fireDelay));
    }

    webUI.broadcastMessage(String("Scheduled TX: ") + copy.name);
    auditMgr.logScheduler(entry.name, entry.buttonId);  // Batch 1
    notifyMgr.onScheduleFire(entry.name);               // Batch 2
    if (sdMgr.isAvailable())
        sdMgr.log(String("Scheduler TX: ") + copy.name);
}

static void printBanner() {
    Serial.println(F("\n"
        "╔══════════════════════════════════════════════╗\n"
        "║   IR Remote Web GUI                          ║\n"
        "║   ESP32-WROOM-32  ·  Full Feature Build      ║\n"
        "║   NFC · RFID · SubGHz · NRF24 · System       ║\n"
        "╚══════════════════════════════════════════════╝"));
    Serial.printf("Version: %s\n", FIRMWARE_VERSION);
    Serial.printf("Chip: %s rev%d %uMHz  Flash:%uMB  Heap:%u\n",
                  ESP.getChipModel(), ESP.getChipRevision(),
                  getCpuFrequencyMhz(),
                  (unsigned)(ESP.getFlashChipSize() >> 20),
                  ESP.getFreeHeap());
}
