#pragma once
// ============================================================
//  web_server.h  –  IR Remote Web GUI  v5.0.0
//  All batches combined — clean, no duplicates
// ============================================================
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <queue>
#include "ir_button.h"
#include "config.h"
#include "gpio_config.h"

class WebUI {
public:
    WebUI();
    void begin();
    void loop();

    // ── Broadcasts (thread-safe via queue) ────────────────────
    void broadcastIREvent    (const IRButton& btn);
    void broadcastMessage    (const String& msg);
    void broadcastOtaProgress(size_t done, size_t total);
    void broadcastOtaResult  (bool success, const String& msg);
    void broadcastStatus     ();

    // ── Captive Portal (public — called from main.cpp) ────────
    void startCaptivePortal();
    void stopCaptivePortal ();
    void loopCaptivePortal ();

private:
    AsyncWebServer _server;
    AsyncWebSocket _ws;

    portMUX_TYPE       _wsMux = portMUX_INITIALIZER_UNLOCKED;
    std::queue<String> _wsQueue;
    void _flushWsQueue();
    void _pushWsMessage(const String& msg);

    // ── Captive Portal (DNS) ──────────────────────────────────
    DNSServer _dns;
    bool      _captiveActive = false;

    // ── Route setup ───────────────────────────────────────────
    void setupStaticRoutes    ();
    void setupApiRoutes       ();
    void setupOtaRoutes       ();
    void setupGpioRoutes      ();
    void setupGroupRoutes     ();
    void setupSchedulerRoutes ();
    void setupWifiRoutes      ();
    void setupWebSocket       ();
    void setupSdRoutes        ();
    void setupMacroRoutes     ();
    void setupModuleRoutes    ();

    // Batch 1
    void setupRestApiV1Routes ();
    void setupAuditRoutes     ();
    void setupDebugRoutes     ();

    // Batch 2
    void setupRuleRoutes      ();
    void setupNotifyRoutes    ();

    // Batch 3
    void setupAuthRoutes         ();
    void setupCaptivePortal      ();
    void setupOtaImprovedRoutes  ();
    void setupWatchdogRoutes     ();

    // Batch 4
    void setupLogRoutes       ();
    void setupAiRoutes        ();    // AI Assistant routes

    // ── Button handlers ───────────────────────────────────────
    void handleGetButtons  (AsyncWebServerRequest*);
    void handleAddButton   (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleUpdateButton(AsyncWebServerRequest*, uint8_t*, size_t);
    void handleDeleteButton(AsyncWebServerRequest*);
    void handleClearButtons(AsyncWebServerRequest*);
    void handleTransmit    (AsyncWebServerRequest*, uint8_t*, size_t);
    void handlePwmTest     (AsyncWebServerRequest*, uint8_t*, size_t);  // POST /api/v1/ir/pwm-test
    void handlePwmInfo     (AsyncWebServerRequest*);                     // GET  /api/v1/ir/pwm-info
    void handleExport      (AsyncWebServerRequest*);
    void handleImport      (AsyncWebServerRequest*, uint8_t*, size_t);

    // ── Backup & Restore ──────────────────────────────────────
    void handleBackupCreate  (AsyncWebServerRequest*);
    void handleBackupDownload(AsyncWebServerRequest*);
    void handleBackupStatus  (AsyncWebServerRequest*);
    void handleRestore       (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleGetConfig     (AsyncWebServerRequest*);
    void handleSetConfig     (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleRestart       (AsyncWebServerRequest*);
    void handleGetStatus     (AsyncWebServerRequest*);
    void handleGetAutoSave   (AsyncWebServerRequest*);
    void handleSetAutoSave   (AsyncWebServerRequest*);

    // ── Groups ────────────────────────────────────────────────
    void handleGetGroups   (AsyncWebServerRequest*);
    void handleAddGroup    (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleUpdateGroup (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleDeleteGroup (AsyncWebServerRequest*);
    void handleReorderGroup(AsyncWebServerRequest*, uint8_t*, size_t);

    // ── Scheduler ─────────────────────────────────────────────
    void handleGetSchedules  (AsyncWebServerRequest*);
    void handleAddSchedule   (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleUpdateSchedule(AsyncWebServerRequest*, uint8_t*, size_t);
    void handleDeleteSchedule(AsyncWebServerRequest*);
    void handleToggleSchedule(AsyncWebServerRequest*, uint8_t*, size_t);
    void handleGetNtpStatus  (AsyncWebServerRequest*);
    void handleSetTimezone   (AsyncWebServerRequest*, uint8_t*, size_t);

    // ── WiFi scan ─────────────────────────────────────────────
    void handleStartScan  (AsyncWebServerRequest*);
    void handleScanResults(AsyncWebServerRequest*);

    // ── OTA (original) ────────────────────────────────────────
    void handleOtaUpload(AsyncWebServerRequest*, const String&,
                         size_t index, uint8_t*, size_t len, size_t total, bool final);

    // ── GPIO ──────────────────────────────────────────────────
    void handleGetGpioPins(AsyncWebServerRequest*);
    void handleSetGpioPins(AsyncWebServerRequest*, uint8_t*, size_t);
    void handleGetPinList (AsyncWebServerRequest*);

    // ── Macro (LittleFS) ──────────────────────────────────────
    void handleMacroList  (AsyncWebServerRequest*);
    void handleMacroRead  (AsyncWebServerRequest*);
    void handleMacroSave  (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleMacroDelete(AsyncWebServerRequest*);
    void handleMacroRun   (AsyncWebServerRequest*);
    void handleMacroAbort (AsyncWebServerRequest*);
    void handleMacroStatus(AsyncWebServerRequest*);

    // ── SD Card ───────────────────────────────────────────────
    void handleSdStatus      (AsyncWebServerRequest*);
    void handleSdList        (AsyncWebServerRequest*);
    void handleSdDelete      (AsyncWebServerRequest*);
    void handleSdRename      (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleSdMkdir       (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleSdLog         (AsyncWebServerRequest*);
    void handleSdOtaTrigger  (AsyncWebServerRequest*);
    void handleSdBackup      (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleSdRestore     (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleSdMacroRun    (AsyncWebServerRequest*);
    void handleSdMacroList   (AsyncWebServerRequest*);
    void handleSdMacroStatus (AsyncWebServerRequest*);
    void handleSdIRLibExport (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleSdIRLibImport (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleSdIRLibList   (AsyncWebServerRequest*);
    void handleSdDeviceList  (AsyncWebServerRequest*);
    void handleSdDeviceRead  (AsyncWebServerRequest*);
    void handleSdBackupList  (AsyncWebServerRequest*);
    void handleSdUpload      (AsyncWebServerRequest*, const String&,
                              size_t, uint8_t*, size_t, bool);
    void handleSdDownload    (AsyncWebServerRequest*);
    void handleSdCopy           (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleSdMove           (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleSdDeleteRecursive(AsyncWebServerRequest*);
    void handleSdFileInfo       (AsyncWebServerRequest*);
    void handleSdReadText       (AsyncWebServerRequest*);
    void handleSdFormat         (AsyncWebServerRequest*, uint8_t*, size_t);

    // ── Batch 1: REST API v1 ──────────────────────────────────
    void handleV1Status        (AsyncWebServerRequest*);
    void handleV1IrTrigger     (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleV1IrList        (AsyncWebServerRequest*);
    void handleV1MacroRun      (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleV1MacroList     (AsyncWebServerRequest*);
    void handleV1ScheduleList  (AsyncWebServerRequest*);
    void handleV1RfidLog       (AsyncWebServerRequest*);
    void handleV1SystemInfo    (AsyncWebServerRequest*);
    void handleV1SystemRestart (AsyncWebServerRequest*, uint8_t*, size_t);

    // ── Batch 1: Audit ────────────────────────────────────────
    void handleAuditGet   (AsyncWebServerRequest*);
    void handleAuditClear (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleAuditExport(AsyncWebServerRequest*);

    // ── Batch 1: Debug ────────────────────────────────────────
    void handleDebugStats  (AsyncWebServerRequest*);
    void handleDebugModules(AsyncWebServerRequest*);

    // ── Batch 2: Rules ────────────────────────────────────────
    void handleRuleCreate(AsyncWebServerRequest*, uint8_t*, size_t);
    void handleRuleUpdate(AsyncWebServerRequest*, uint8_t*, size_t);
    void handleRuleDelete(AsyncWebServerRequest*);
    void handleRuleToggle(AsyncWebServerRequest*);
    void handleRuleFire  (AsyncWebServerRequest*);

    // ── Batch 2: Notifications ────────────────────────────────
    void handleNotifyConfigSave(AsyncWebServerRequest*, uint8_t*, size_t);
    void handleNotifyTest      (AsyncWebServerRequest*, uint8_t*, size_t);

    // ── Batch 3: Auth ─────────────────────────────────────────
    void handleAuthLogin   (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleAuthLogout  (AsyncWebServerRequest*, uint8_t*, size_t);
    void handleAuthPassword(AsyncWebServerRequest*, uint8_t*, size_t);
    void handleAuthConfig  (AsyncWebServerRequest*, uint8_t*, size_t);

    // ── Batch 3: OTA Improved ────────────────────────────────
    void handleOtaVersionCheck(AsyncWebServerRequest*);

    // ── Batch 4: Log Rotation ────────────────────────────────
    void handleLogExportCsv(AsyncWebServerRequest*);
    void handleLogConfig   (AsyncWebServerRequest*, uint8_t*, size_t);

    static void onWsEvent(AsyncWebSocket*, AsyncWebSocketClient*,
                          AwsEventType, void*, uint8_t*, size_t);
};

extern WebUI webUI;
