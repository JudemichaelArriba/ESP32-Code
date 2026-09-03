#include "core/structures.h"
#include "functions/wifi_functions.h"
#include "functions/utility_functions.h"
#include "functions/persistence_functions.h"
#include "functions/logger_functions.h"
#include "functions/firebase_functions.h"
#include "functions/energy_functions.h"
#include "functions/ac_control.h"
#include "functions/schedule_functions.h"
#include "functions/sensor_functions.h"
#include "functions/heartbeat_functions.h"
#include <esp_idf_version.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

DHT dht(DHTPIN, DHTTYPE);
FirebaseData fbdo;
FirebaseData streamFbdo;
FirebaseAuth auth;
FirebaseConfig config;
Adafruit_MLX90614 mlx = Adafruit_MLX90614();
IRCoolixAC coolixAc(IR_LED_PIN);

RoomConfig assignedRoom;
ScheduleStatus currentScheduleStatus;
EnergyRuntimeState energyRuntimeState;
EnergyDailyCache energyDailyCache;

float lastHumidity    = NAN;
float lastTemperature = NAN;
float mlxObjectTemp   = NAN;
float mlxAmbientTemp  = NAN;
float mlxDeltaTemp    = NAN;

bool pirMotionDetected    = false;
bool mlxPresenceDetected  = false;
bool presenceDetected     = false;
bool lastPresenceReported = true;
bool occupancyPublishPending = false;
unsigned long lastOccupancyPublishAttemptMillis = 0;
uint8_t mlxPositiveReadStreak = 0;
uint8_t mlxNegativeReadStreak = 0;

volatile bool pirMotionLatched             = false;
volatile unsigned long lastPirInterruptMillis = 0;
unsigned long lastPirMotionMillis          = 0;
unsigned long lastPresenceDetectedMillis   = 0;

bool   acPowerState  = false;
int    acTempState   = 24;
String acSourceState = "boot";
bool   acIrStateTrusted = false;

bool manualOverrideActive = false;
bool manualOverridePower  = false;
int  manualOverrideTemp   = 24;
bool aiAutoApplyEnabled   = false;

bool streamAttached      = false;
bool firebaseInitialized = false;
bool startupStateLoaded  = false;
bool restoredManualOverridePendingApply = false;

unsigned long lastDhtReadMillis            = 0;
unsigned long lastMlxReadMillis            = 0;
unsigned long lastMLCallMillis             = 0;
unsigned long lastWiFiReconnectAttempt     = 0;
unsigned long lastNtpSyncMillis            = 0;
unsigned long lastNtpValidMillis           = 0;
unsigned long lastNtpCheckMillis           = 0;
bool ntpTimeValid                          = false;
uint8_t consecutiveNtpFailures             = 0;
unsigned long lastWiFiConnectedMillis      = 0;
unsigned long lastFirebaseInitMillis       = 0;
unsigned long lastStreamRetryMillis        = 0;
unsigned long lastRoomsFetchAttemptMillis  = 0;
int  lastCheckedMinuteStamp    = -1;
bool minuteGateInitialized     = false;
bool wifiLinkUp                = false;
bool wifiHasConnectedOnce      = false;
uint8_t       netAuthState     = 0;
unsigned long netAuthStateSince = 0;
String lastScheduleMode        = "boot";
bool   wasInScheduleWindow     = false;
unsigned long scheduleWindowEnteredMillis = 0;
String lastActiveWindowKey     = "";
String lastScheduleWindowKey   = "";
String manualOverrideUntil     = "";
int    manualOverrideTargetTemp = 24;
int    estimatedWattsOn        = DEFAULT_ESTIMATED_WATTS_ON;

bool forcedOffActive = false;
String forcedOffWindowKey = "";
bool idleOccupancyPublished = false;
bool sensorWindowActive = false;
bool mlxAvailable = false;
unsigned long lastMlxInitAttemptMillis = 0;
RTC_DATA_ATTR uint32_t bootCount = 0;
RTC_DATA_ATTR RuntimeBreadcrumb runtimeBreadcrumb;
uint32_t firebaseRecoveryCount = 0;
unsigned long lastFirebaseReadyMillis = 0;
unsigned long firebaseUnavailableSinceMillis = 0;
unsigned long networkOutageSinceMillis = 0;
uint8_t firebaseSessionRecoveryStreak = 0;
unsigned long lastHeartbeatSuccessMillis = 0;
uint8_t consecutiveHeartbeatFailures = 0;
String lastFirebaseTokenStatus = "uninitialized";
int lastFirebaseTokenErrorCode = 0;
String lastFirebaseTokenError = "";
uint8_t startupStateFailureCount = 0;
bool previousResetBreadcrumbAvailable = false;
uint32_t previousResetUptimeMs = 0;
uint32_t previousResetBootNumber = 0;
char previousResetOperation[40] = "";

StreamPendingAction streamPendingAction;

extern unsigned long lastEspControlWriteMillis;

static void initializeLoopWatchdog() {
#if ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t watchdogConfig = {};
  watchdogConfig.timeout_ms = LOOP_WATCHDOG_TIMEOUT_MS;
  // Monitor loopTask only. Firebase streaming can legitimately keep CPU1 busy,
  // so subscribing its idle task would cause false watchdog resets.
  watchdogConfig.idle_core_mask = 0;
  watchdogConfig.trigger_panic = true;
  // Arduino already initializes TWDT on current ESP32 cores. Reconfigure it
  // first to avoid the noisy "TWDT already initialized" error.
  esp_err_t initResult = esp_task_wdt_reconfigure(&watchdogConfig);
  if (initResult == ESP_ERR_INVALID_STATE) {
    initResult = esp_task_wdt_init(&watchdogConfig);
  }
#else
  esp_err_t initResult = esp_task_wdt_init(LOOP_WATCHDOG_TIMEOUT_MS / 1000U, true);
#endif
  if (initResult != ESP_OK && initResult != ESP_ERR_INVALID_STATE) {
    Serial.printf("Watchdog: init failed (%d)\n", (int)initResult);
    return;
  }

  esp_err_t addResult = esp_task_wdt_add(nullptr);
  if (addResult != ESP_OK && addResult != ESP_ERR_INVALID_STATE) {
    Serial.printf("Watchdog: task registration failed (%d)\n", (int)addResult);
  }
}

void logScheduleModeChange(const String& mode) {
  if (lastScheduleMode == mode) return;
  lastScheduleMode = mode;
  Serial.println("Mode: " + mode);
  logDecisionEvent("mode_change", mode, acPowerState, acTempState,
                   -1, aiAutoApplyEnabled, false, "schedule_mode_changed");
}

void runMinuteControl(const struct tm& t) {
  fetchAssignedRoomFromFirebase();

  if (!assignedRoom.found) {
    if (!Firebase.ready()) return;
    logScheduleModeChange("NO_ASSIGNED_ROOM");
    applyAcState(false, acTempState, "no_assigned_room");
    currentScheduleStatus = ScheduleStatus();
    disableSensorsAndOccupancyIfIdle();
    return;
  }

  currentScheduleStatus = evaluateScheduleStatus(t);

  static bool isFirstMinuteRun = true;
  bool justEnteredSchedule = false;
  bool justEnteredWindow = false;
  const bool inAnyWindow = currentScheduleStatus.inPreCool || currentScheduleStatus.inSchedule;
  const String activeWindowKey = inAnyWindow ? currentScheduleStatus.windowKey : String("");
  const bool inScheduleWindow = currentScheduleStatus.inSchedule &&
                                currentScheduleStatus.windowKey.length() > 0;
  const String scheduleWindowKey = inScheduleWindow ? currentScheduleStatus.windowKey : String("");
  unsigned long nowMs = millis();

  if (isFirstMinuteRun) {
    if (activeWindowKey.length() > 0) {
      justEnteredWindow = true;
    }
    lastActiveWindowKey = activeWindowKey;

    if (inScheduleWindow) {
      justEnteredSchedule = true;
      scheduleWindowEnteredMillis = nowMs;
      lastScheduleWindowKey = scheduleWindowKey;
      Serial.println("Schedule window entered: " + scheduleWindowKey);
    } else {
      lastScheduleWindowKey = "";
    }

    wasInScheduleWindow = currentScheduleStatus.inSchedule;
    isFirstMinuteRun = false;
  } else {
    if (activeWindowKey.length() > 0 && activeWindowKey != lastActiveWindowKey) {
      justEnteredWindow = true;
      lastActiveWindowKey = activeWindowKey;
    } else if (activeWindowKey.length() == 0 && lastActiveWindowKey.length() > 0) {
      lastActiveWindowKey = "";
    }

    if (inScheduleWindow && scheduleWindowKey != lastScheduleWindowKey) {
      justEnteredSchedule = true;
      scheduleWindowEnteredMillis = nowMs;
      lastScheduleWindowKey = scheduleWindowKey;
      Serial.println("Schedule window entered: " + scheduleWindowKey);
    } else if (!inScheduleWindow && lastScheduleWindowKey.length() > 0) {
      lastScheduleWindowKey = "";
    }

    if (currentScheduleStatus.inSchedule) {
      wasInScheduleWindow = true;
    } else if (!currentScheduleStatus.inSchedule) {
      wasInScheduleWindow = false;
    }
  }

  if (forcedOffActive) {
    if (!inAnyWindow || forcedOffWindowKey.length() == 0 || forcedOffWindowKey != activeWindowKey) {
      clearForcedOffPersistedInFirebase();
      Serial.println("ForcedOff: stale window lock cleared.");
    } else {
      logScheduleModeChange("FORCED_OFF");
      applyAcState(false, acTempState, "forced_off");
      return;
    }
  }

  if (justEnteredSchedule) {
    lastMLCallMillis = 0;
  }

  if (!currentScheduleStatus.hasScheduleToday) {
    if (manualOverrideActive) {
      logScheduleModeChange("MANUAL_OVERRIDE");
      applyAcState(manualOverridePower, manualOverrideTargetTemp, "manual");
      return;
    }
    logScheduleModeChange("NO_SCHEDULE_TODAY");
    applyAcState(false, acTempState, "no_schedule_today");
    disableSensorsAndOccupancyIfIdle();
    return;
  }

  if (manualOverrideActive && !inAnyWindow) {
    logScheduleModeChange("MANUAL_OVERRIDE");
    applyAcState(manualOverridePower, manualOverrideTargetTemp, "manual");
    return;
  }

  if (!inAnyWindow) {
    logScheduleModeChange("OUTSIDE_SCHEDULE");
    applyAcState(false, acTempState, "outside_schedule");
    disableSensorsAndOccupancyIfIdle();
    return;
  }

  if (manualOverrideActive) {
    logScheduleModeChange("MANUAL_OVERRIDE");
    applyAcState(manualOverridePower, manualOverrideTargetTemp, "manual");
    return;
  }

  if (currentScheduleStatus.inPreCool && !currentScheduleStatus.inSchedule) {
    logScheduleModeChange("PRE_COOL");
    applyAcState(true, PRECOOL_TEMP, "pre_cool", justEnteredWindow || !acIrStateTrusted);
    return;
  }

  logScheduleModeChange("SCHEDULE");

  const bool forceScheduleIr = justEnteredSchedule || !acIrStateTrusted;
  if (justEnteredSchedule || forceScheduleIr) {
    Serial.println("Schedule fallback ON before occupancy grace");
    applyAcState(true, PRECOOL_TEMP, "schedule", true);
  }

  unsigned long emptyReferenceMillis = scheduleWindowEnteredMillis;
  if (lastPresenceDetectedMillis > emptyReferenceMillis) {
    emptyReferenceMillis = lastPresenceDetectedMillis;
  }

  bool scheduleNoOccupancyTooLong = (nowMs - emptyReferenceMillis) >= SCHEDULE_NO_OCC_OFF_MS;

  if (!presenceDetected && scheduleNoOccupancyTooLong) {
    Serial.println("Schedule empty grace expired");
    applyAcState(false, acTempState, "empty");
    return;
  }

  if (!acPowerState) {
    Serial.println("Schedule fallback ON before occupancy grace");
    applyAcState(true, PRECOOL_TEMP, "schedule");
  }

  if (!presenceDetected) return;

  if ((nowMs - lastMLCallMillis) >= ML_INTERVAL_MS || lastMLCallMillis == 0) {
    bool needsFreshDht = lastDhtReadMillis == 0 ||
                         (nowMs - lastDhtReadMillis) >= DHT_INTERVAL_MS ||
                         isnan(lastTemperature) ||
                         isnan(lastHumidity);
    if (needsFreshDht && !forceReadDhtNow()) {
      Serial.println("ML: skipped (fresh DHT read failed), retry in ~1 minute");
      logMlFailure("fresh_dht_failed");
      const unsigned long retryOffset = ML_INTERVAL_MS - 60000UL;
      lastMLCallMillis = nowMs > retryOffset ? nowMs - retryOffset : 0;
      return;
    }

    Serial.println("ML: attempt from schedule controller");
    int mlTemp = acTempState;
    if (callRenderMLAndGetTarget(mlTemp)) {
      const bool shouldAutoApply = aiAutoApplyEnabled;
      const String mlReason = shouldAutoApply ? "auto_apply" : "suggest_only";
      logMlSuggestion(mlTemp, shouldAutoApply, mlReason);

      if (shouldAutoApply) {
        Serial.printf("ML: auto-apply target %d\n", mlTemp);
        applyAcState(true, mlTemp, "ml");
        logDecisionEvent("ml_auto_applied", "ml", true, mlTemp, mlTemp,
                         aiAutoApplyEnabled, true, "auto_apply");
      } else {
        Serial.printf("ML: suggestion only target %d (aiAutoApply=false)\n", mlTemp);
      }
      lastMLCallMillis = nowMs;
    } else {
      Serial.println("ML: attempt failed, retry in ~1 minute");
      logMlFailure("render_call_failed");
      const unsigned long retryOffset = ML_INTERVAL_MS - 60000UL;
      lastMLCallMillis = nowMs > retryOffset ? nowMs - retryOffset : 0;
    }
  }
}

void checkMinuteTickAndRunControl() {
  struct tm t;
  if (!timeIsValid(t)) return;

  int minuteStamp = t.tm_yday * 1440 + t.tm_hour * 60 + t.tm_min;

  if (!minuteGateInitialized) {
    minuteGateInitialized  = true;
    lastCheckedMinuteStamp = minuteStamp - 1;
  }

  if (minuteStamp == lastCheckedMinuteStamp) return;

  lastCheckedMinuteStamp = minuteStamp;
  runMinuteControl(t);
}

void checkOverrideExpiry() {
  if (!manualOverrideActive) return;
  if (manualOverrideUntil.length() == 0) return;

  struct tm now;
  if (!timeIsValid(now)) return;

  time_t expiryEpoch;
  if (!parseIso8601ToEpoch(manualOverrideUntil, expiryEpoch)) {
    Serial.println("Override expiry: invalid timestamp, keeping override active.");
    return;
  }

  const bool expired = time(nullptr) >= expiryEpoch;

  if (expired) {
    Serial.println("Override expired, clearing and turning AC off.");
    manualOverrideActive = false;
    manualOverrideUntil  = "";
    persistManualOverrideState();
    applyAcState(false, acTempState, "override_expired");
    clearOverrideInFirebase();
  }
}

void processPendingStreamAction() {
  if (!streamPendingAction.hasPending) return;

  StreamPendingAction act = streamPendingAction;
  streamPendingAction = StreamPendingAction();

  const bool hasControlPatch = act.writeForcedOffPersisted ||
                               act.writeForcedOffWindowKey ||
                               act.writeForcedOffFalse;
  if (hasControlPatch && WiFi.status() == WL_CONNECTED && Firebase.ready()) {
    FirebaseJson patch;
    if (act.writeForcedOffPersisted) {
      patch.set("forcedOffPersisted", act.forcedOffPersistedVal);
    }
    if (act.writeForcedOffWindowKey) {
      patch.set("forcedOffWindowKey", String(act.forcedOffWindowKey));
    }
    if (act.writeForcedOffFalse) {
      patch.set("forcedOff", false);
      patch.set("overrideActive", false);
    }

    String base = "/devices/" + String(DEVICE_ID) + "/control";
    lastEspControlWriteMillis = millis();
    markRuntimeOperation("firebase_control_ack");
    bool acknowledged = Firebase.RTDB.updateNode(&fbdo, base, &patch);
    clearRuntimeOperation();
    if (!acknowledged) {
      String err = fbdo.errorReason();
      Serial.println("Control stream ack failed: " + err);
      if (!isFirebaseTokenPendingError(err) && isFirebaseAuthOrSslError(err)) {
        requestFirebaseReinit(err);
      }
    }
  }

  String pendingSource = String(act.source);
  bool forcePendingIr = pendingSource == "forced_off" || pendingSource == "override_cleared";
  applyAcState(act.power, act.temp, pendingSource, forcePendingIr);
}

void setup() {
  Serial.begin(115200);
  capturePreviousResetBreadcrumb();
  bootCount++;
  restoreManualOverrideFromPreferences();

  dht.begin();

  coolixAc.begin();
  coolixAc.on();
  coolixAc.setFan(kCoolixFanAuto);
  coolixAc.setMode(kCoolixCool);
  coolixAc.setTemp(acTempState);
  coolixAc.off();

  pinMode(PIR_PIN, PIR_ACTIVE_HIGH ? INPUT_PULLDOWN : INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIR_PIN), onPirMotion, PIR_ACTIVE_HIGH ? RISING : FALLING);

  lastMlxInitAttemptMillis = millis();
  mlxAvailable = mlx.begin();
  if (!mlxAvailable) {
    Serial.println("MLX90614 unavailable at boot; continuing and retrying in loop.");
  }

  config.database_url = String("https://") + FIREBASE_HOST;
  config.api_key      = FIREBASE_API_KEY;
  auth.user.email     = ESP_EMAIL;
  auth.user.password  = ESP_PASSWORD;
  config.token_status_callback = firebaseTokenStatusCallback;

  setupWiFiProvisioning();

  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);

  Serial.print("Waiting for NTP time sync...");
  struct tm timeinfo;
  int retry = 0;
  while (!getLocalTime(&timeinfo) && retry < 20) {
    Serial.print(".");
    delay(500);
    retry++;
  }
  if (timeIsValid(timeinfo)) {
    ntpTimeValid = true;
    lastNtpValidMillis = millis();
    consecutiveNtpFailures = 0;
    Serial.println("\nTime synced.");
  } else {
    ntpTimeValid = false;
    Serial.println("\nTime sync unavailable; Firebase will wait and retry.");
  }
  lastNtpSyncMillis = millis();

  if (restoredManualOverridePendingApply && manualOverrideActive) {
    checkOverrideExpiry();
    if (manualOverrideActive) {
      Serial.println("Applying persisted manual override before Firebase startup.");
      applyAcState(manualOverridePower, manualOverrideTargetTemp, "manual_restore", true);
    }
    restoredManualOverridePendingApply = false;
  }
  initializeLoopWatchdog();
}

void loop() {
  serviceWiFiProvisioning();
  reconnectWiFiNonBlocking();
  serviceNetworkTime();
  serviceNetworkRecovery();
  initFirebaseIfNeeded();

  if (firebaseInitialized && Firebase.ready()) {
    if (!startupStateLoaded) {
      if (fetchAssignedRoomFromFirebase()) {
        bool acStateLoaded = loadAcStateFromFirebase();
        struct tm startupTime;
        bool startupTimeValid = timeIsValid(startupTime);
        if (startupTimeValid) {
          currentScheduleStatus = evaluateScheduleStatus(startupTime);
        }
        bool controlStateLoaded = loadControlStateFromFirebase();
        if (!acStateLoaded || !controlStateLoaded) {
          if (startupStateFailureCount < 255) startupStateFailureCount++;
          Serial.println("Startup state incomplete; retaining safe local state and retrying.");
          if (startupStateFailureCount >= FIREBASE_FAILURES_BEFORE_REINIT) {
            startupStateFailureCount = 0;
            requestFirebaseReinit("startup state restore failed");
          }
        } else {
          startupStateFailureCount = 0;
          loadEnergyProfileFromFirebase();
          loadEnergyStateFromFirebase();
          syncAcStateToFirebase();
          syncEnergyProfileToFirebase();
          initializeEnergyTrackingForCurrentState();
          startupStateLoaded = true;
          logDecisionEvent("boot", "startup", acPowerState, acTempState,
                           -1, aiAutoApplyEnabled, false, "startup_state_loaded");

          if (startupTimeValid) {
            runMinuteControl(startupTime);
          }
        }
      }
    }

    if (startupStateLoaded) {
      ensureControlStream();
      pollControlStream();
    }

    processPendingStreamAction();
  }

  if (startupStateLoaded) {
    checkMinuteTickAndRunControl();
  }

  struct tm _tCheck;
  if (ntpTimeValid && timeIsValid(_tCheck)) {
    checkOverrideExpiry();
  }

  tickEnergyTracking();
  tickHeartbeat();  // <-- only addition to loop()

  const bool sensorWindowOnline = shouldPollSensors();
  if (sensorWindowOnline) {
    if (currentScheduleStatus.inSchedule && !manualOverrideActive && !presenceDetected) {
      refreshOccupancyOnly();
    } else {
      refreshSensorsAndOccupancy();
    }
  } else {
    disableSensorsAndOccupancyIfIdle();
  }

  tickOccupancySerialDiagnostics();

  esp_task_wdt_reset();
}
