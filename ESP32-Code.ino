#include "core/structures.h"
#include "functions/utility_functions.h"
#include "functions/firebase_functions.h"
#include "functions/energy_functions.h"
#include "functions/ac_control.h"
#include "functions/schedule_functions.h"
#include "functions/sensor_functions.h"

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

bool pirMotionDetected    = false;
bool mlxPresenceDetected  = false;
bool presenceDetected     = false;
bool lastPresenceReported = true;

volatile bool pirMotionLatched             = false;
volatile unsigned long lastPirInterruptMillis = 0;
unsigned long lastPirMotionMillis          = 0;
unsigned long lastPresenceDetectedMillis   = 0;

bool   acPowerState  = false;
int    acTempState   = 24;
String acSourceState = "boot";

bool manualOverrideActive = false;
bool manualOverridePower  = false;
int  manualOverrideTemp   = 24;

bool streamAttached      = false;
bool firebaseInitialized = false;
bool startupStateLoaded  = false;

unsigned long lastDhtReadMillis            = 0;
unsigned long lastMlxReadMillis            = 0;
unsigned long lastMLCallMillis             = 0;
unsigned long lastWiFiReconnectAttempt     = 0;
unsigned long lastNtpSyncMillis            = 0;
unsigned long lastWiFiConnectedMillis      = 0;
unsigned long lastFirebaseInitMillis       = 0;
unsigned long lastStreamRetryMillis        = 0;
unsigned long lastRoomsFetchAttemptMillis  = 0;
int  lastCheckedMinuteStamp    = -1;
bool minuteGateInitialized     = false;
bool wifiLinkUp                = false;
bool wifiHasConnectedOnce      = false;
bool wifiReconnectRestartPending = false;
unsigned long wifiReconnectStableSince = 0;
uint8_t       netAuthState     = 0;
unsigned long netAuthStateSince = 0;
String lastScheduleMode        = "boot";
bool   wasInScheduleWindow     = false;
unsigned long scheduleWindowEnteredMillis = 0;
String manualOverrideUntil     = "";
int    manualOverrideTargetTemp = 24;
int    estimatedWattsOn        = DEFAULT_ESTIMATED_WATTS_ON;

const unsigned long SCHEDULE_NO_OCC_OFF_MS = 5UL * 60UL * 1000UL;

bool forcedOffActive     = false;
bool forcedOffSeenWindow = false;

// Deferred action populated by streamCallback, consumed by the main loop.
StreamPendingAction streamPendingAction;

extern unsigned long lastEspControlWriteMillis; // Imported from firebase_functions.h

void logScheduleModeChange(const String& mode) {
  if (lastScheduleMode == mode) return;
  lastScheduleMode = mode;
  Serial.println("Mode: " + mode);
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

  // Track schedule entry securely at the very top to ignore early returns
  static bool isFirstMinuteRun = true;
  bool justEnteredSchedule = false;

  if (isFirstMinuteRun) {
    wasInScheduleWindow = currentScheduleStatus.inSchedule;
    if (wasInScheduleWindow) {
      scheduleWindowEnteredMillis = millis();
    }
    isFirstMinuteRun = false;
  } else {
    if (currentScheduleStatus.inSchedule && !wasInScheduleWindow) {
      wasInScheduleWindow = true;
      scheduleWindowEnteredMillis = millis();
      justEnteredSchedule = true;
    } else if (!currentScheduleStatus.inSchedule) {
      wasInScheduleWindow = false;
    }
  }

  const bool inAnyWindow = currentScheduleStatus.inPreCool || currentScheduleStatus.inSchedule;

  if (forcedOffActive) {
    if (justEnteredSchedule) {
      forcedOffActive     = false;
      forcedOffSeenWindow = false;
      clearForcedOffPersistedInFirebase();
      Serial.println("ForcedOff: exact schedule started — resuming normal control.");
    }
    else if (inAnyWindow) {
      if (!forcedOffSeenWindow) {
        forcedOffActive     = false;
        forcedOffSeenWindow = false;
        clearForcedOffPersistedInFirebase();
        Serial.println("ForcedOff: new schedule window detected — resuming normal control.");
      } else {
        logScheduleModeChange("FORCED_OFF");
        applyAcState(false, acTempState, "forced_off");
        return;
      }
    } else {
      forcedOffSeenWindow = false;
      logScheduleModeChange("FORCED_OFF");
      applyAcState(false, acTempState, "forced_off");
      disableSensorsAndOccupancyIfIdle();
      return;
    }
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
    applyAcState(true, PRECOOL_TEMP, "pre_cool");
    return;
  }

  logScheduleModeChange("SCHEDULE");
  unsigned long nowMs = millis();

  unsigned long emptyReferenceMillis = scheduleWindowEnteredMillis;
  if (lastPresenceDetectedMillis > emptyReferenceMillis) {
    emptyReferenceMillis = lastPresenceDetectedMillis;
  }

  bool scheduleNoOccupancyTooLong = (nowMs - emptyReferenceMillis) >= SCHEDULE_NO_OCC_OFF_MS;

  if (!presenceDetected && scheduleNoOccupancyTooLong) {
    applyAcState(false, acTempState, "empty");
    return;
  }

  if (!presenceDetected) return;

  if (!acPowerState) {
    applyAcState(true, PRECOOL_TEMP, "schedule");
  }

  if ((nowMs - lastMLCallMillis) >= ML_INTERVAL_MS || lastMLCallMillis == 0) {
    if (isnan(lastTemperature) || isnan(lastHumidity)) forceReadDhtNow();

    Serial.println("ML: attempt from schedule controller");
    int mlTemp = acTempState;
    if (callRenderMLAndGetTarget(mlTemp)) {
      Serial.printf("ML: apply target %d\n", mlTemp);
      applyAcState(true, mlTemp, "ml");
      lastMLCallMillis = nowMs;
    } else {
      Serial.println("ML: attempt failed, retry in ~1 minute");
      lastMLCallMillis = nowMs - (ML_INTERVAL_MS - 60000UL);
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

  int yr, mo, dy, hr, mn, sc;
  String s = manualOverrideUntil;
  s.replace("Z", "");
  int dotIdx = s.indexOf('.');
  if (dotIdx > 0) s = s.substring(0, dotIdx);
  if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &yr, &mo, &dy, &hr, &mn, &sc) != 6) return;

  struct tm expiryLocal = {};
  expiryLocal.tm_year  = yr - 1900;
  expiryLocal.tm_mon   = mo - 1;
  expiryLocal.tm_mday  = dy;
  expiryLocal.tm_hour  = hr + (int)(GMT_OFFSET_SEC / 3600);
  expiryLocal.tm_min   = mn;
  expiryLocal.tm_sec   = sc;
  expiryLocal.tm_isdst = 0;
  mktime(&expiryLocal);

  int nowDayStamp    = now.tm_year * 366 + now.tm_yday;
  int expiryDayStamp = expiryLocal.tm_year * 366 + expiryLocal.tm_yday;
  int nowMin         = now.tm_hour * 60 + now.tm_min;
  int expiryMin      = expiryLocal.tm_hour * 60 + expiryLocal.tm_min;

  bool expired = (nowDayStamp > expiryDayStamp) ||
                 (nowDayStamp == expiryDayStamp && nowMin >= expiryMin);

  if (expired) {
    Serial.println("Override expired, clearing and turning AC off.");
    manualOverrideActive = false;
    manualOverrideUntil  = "";
    applyAcState(false, acTempState, "override_expired");
    clearOverrideInFirebase();
  }
}

void setup() {
  Serial.begin(115200);

  dht.begin();
  
  // Force Coolix AC library to construct a fully valid 24-bit state block 
  // right at boot, preventing garbage data on the first turn-off command.
  coolixAc.begin();
  coolixAc.on(); 
  coolixAc.setFan(kCoolixFanAuto);
  coolixAc.setMode(kCoolixCool);
  coolixAc.setTemp(acTempState);
  coolixAc.off();

  pinMode(PIR_PIN, PIR_ACTIVE_HIGH ? INPUT_PULLDOWN : INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIR_PIN), onPirMotion, PIR_ACTIVE_HIGH ? RISING : FALLING);

  if (!mlx.begin()) {
    Serial.println("Could not find MLX90614 sensor. Check wiring!");
    while (1) { delay(1000); }
  }

  config.database_url = String("https://") + FIREBASE_HOST;
  config.api_key      = FIREBASE_API_KEY;
  auth.user.email     = ESP_EMAIL;
  auth.user.password  = ESP_PASSWORD;

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);

  Serial.print("Waiting for NTP time sync...");
  struct tm timeinfo;
  int retry = 0;
  while (!getLocalTime(&timeinfo) && retry < 20) {
    Serial.print(".");
    delay(500);
    retry++;
  }
  Serial.println("\nTime synced.");
}

void loop() {
  reconnectWiFiNonBlocking();
  initFirebaseIfNeeded();

  if (WiFi.status() == WL_CONNECTED &&
      (millis() - lastNtpSyncMillis >= NTP_RESYNC_MS || lastNtpSyncMillis == 0)) {
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    lastNtpSyncMillis = millis();
  }

  if (firebaseInitialized && Firebase.ready()) {
    ensureControlStream();

    if (streamPendingAction.hasPending) {
      StreamPendingAction act = streamPendingAction;  
      streamPendingAction     = StreamPendingAction(); 

      if (act.writeForcedOffPersisted) {
        String base = "/devices/" + String(DEVICE_ID) + "/control";
        lastEspControlWriteMillis = millis(); 
        Firebase.RTDB.setBool(&fbdo, base + "/forcedOffPersisted", act.forcedOffPersistedVal);
      }
      if (act.writeForcedOffFalse) {
        String base = "/devices/" + String(DEVICE_ID) + "/control";
        lastEspControlWriteMillis = millis(); 
        Firebase.RTDB.setBool(&fbdo, base + "/forcedOff", false);
        Firebase.RTDB.setBool(&fbdo, base + "/overrideActive", false);
      }

      applyAcState(act.power, act.temp, String(act.source));
    }

    if (!startupStateLoaded) {
      if (fetchAssignedRoomFromFirebase()) {
        loadAcStateFromFirebase();
        loadControlStateFromFirebase();
        loadEnergyProfileFromFirebase();
        loadEnergyStateFromFirebase();
        syncAcStateToFirebase();
        syncEnergyProfileToFirebase();
        initializeEnergyTrackingForCurrentState();
        startupStateLoaded = true;

        struct tm t;
        if (timeIsValid(t)) {
          runMinuteControl(t);
        }
      }
    }
  }

  checkMinuteTickAndRunControl();

  struct tm _tCheck;
  if (timeIsValid(_tCheck)) {
    checkOverrideExpiry();
  }

  tickEnergyTracking();

  if (shouldPollSensors()) {
    if (currentScheduleStatus.inSchedule && !manualOverrideActive && !presenceDetected) {
      refreshOccupancyOnly();
    } else {
      refreshSensorsAndOccupancy();
    }
  } else if (manualOverrideActive) {
    refreshSensorsAndOccupancy();
  } else {
    disableSensorsAndOccupancyIfIdle();
  }
}
