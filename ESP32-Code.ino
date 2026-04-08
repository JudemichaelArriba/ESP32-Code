//ESP32_code.ino
#include "core/structures.h"
#include "functions/utility_functions.h"
#include "functions/firebase_functions.h"
#include "functions/energy_functions.h"
#include "functions/ac_control.h"
#include "functions/schedule_functions.h"
#include "functions/sensor_functions.h"

// Define global objects
DHT dht(DHTPIN, DHTTYPE);
FirebaseData fbdo;
FirebaseData streamFbdo;
FirebaseAuth auth;
FirebaseConfig config;
Adafruit_MLX90614 mlx = Adafruit_MLX90614();
IRCoolixAC coolixAc(IR_LED_PIN);

// Define global variables
RoomConfig assignedRoom;
ScheduleStatus currentScheduleStatus;
EnergyRuntimeState energyRuntimeState;
EnergyDailyCache energyDailyCache;

float lastHumidity = NAN;
float lastTemperature = NAN;
float mlxObjectTemp = NAN;
float mlxAmbientTemp = NAN;

bool pirMotionDetected = false;
bool mlxPresenceDetected = false;
bool presenceDetected = false;
bool lastPresenceReported = true;

volatile bool pirMotionLatched = false;
volatile unsigned long lastPirInterruptMillis = 0;
unsigned long lastPirMotionMillis = 0;
unsigned long lastPresenceDetectedMillis = 0;

bool acPowerState = false;
int acTempState = 24;
String acSourceState = "boot";

bool manualOverrideActive = false;
bool manualOverridePower = false;
int manualOverrideTemp = 24;

bool streamAttached = false;
bool firebaseInitialized = false;
bool startupStateLoaded = false;

unsigned long lastDhtReadMillis = 0;
unsigned long lastMlxReadMillis = 0;
unsigned long lastMLCallMillis = 0;
unsigned long lastWiFiReconnectAttempt = 0;
unsigned long lastNtpSyncMillis = 0;
unsigned long lastWiFiConnectedMillis = 0;
unsigned long lastFirebaseInitMillis = 0;
unsigned long lastStreamRetryMillis = 0;
unsigned long lastRoomsFetchAttemptMillis = 0;
int lastCheckedMinuteStamp = -1;
bool minuteGateInitialized = false;
bool wifiLinkUp = false;
bool wifiHasConnectedOnce = false;
bool wifiReconnectRestartPending = false;
unsigned long wifiReconnectStableSince = 0;
uint8_t netAuthState = 0;
unsigned long netAuthStateSince = 0;
String lastScheduleMode = "boot";
bool wasInScheduleWindow = false;
unsigned long scheduleWindowEnteredMillis = 0;
String manualOverrideUntil = "";
int manualOverrideTargetTemp = 24;
int estimatedWattsOn = DEFAULT_ESTIMATED_WATTS_ON;
const unsigned long SCHEDULE_NO_OCC_OFF_MS = 5UL * 60UL * 1000UL;

void logScheduleModeChange(const String& mode) {
  if (lastScheduleMode == mode) return;
  lastScheduleMode = mode;
  Serial.println("Mode: " + mode);
}

void runMinuteControl(const struct tm& t) {
  fetchAssignedRoomFromFirebase();

  if (!assignedRoom.found) {
    if (!Firebase.ready()) {
      return;
    }
    logScheduleModeChange("NO_ASSIGNED_ROOM");
    applyAcState(false, acTempState, "no_assigned_room");
    currentScheduleStatus = ScheduleStatus();
    disableSensorsAndOccupancyIfIdle();
    return;
  }

  currentScheduleStatus = evaluateScheduleStatus(t);

// FIXED - in ESP32_code.ino, runMinuteControl()
if (!currentScheduleStatus.hasScheduleToday) {
  if (manualOverrideActive) {
    logScheduleModeChange("MANUAL_OVERRIDE");
    applyAcState(true, manualOverrideTargetTemp, "manual");
    return;
  }
  logScheduleModeChange("NO_SCHEDULE_TODAY");
  applyAcState(false, acTempState, "no_schedule_today");
  disableSensorsAndOccupancyIfIdle();
  return;
}

  const bool inAnyWindow = currentScheduleStatus.inPreCool || currentScheduleStatus.inSchedule;

  if (!currentScheduleStatus.inSchedule) {
    wasInScheduleWindow = false;
  }

// FIXED - in ESP32_code.ino, runMinuteControl()
if (manualOverrideActive && !inAnyWindow) {
  logScheduleModeChange("MANUAL_OVERRIDE");
  applyAcState(true, manualOverrideTargetTemp, "manual");
  return;
}
  // Priority 2: Outside schedule windows -> AC off.
  if (!inAnyWindow) {
    logScheduleModeChange("OUTSIDE_SCHEDULE");
    applyAcState(false, acTempState, "outside_schedule");
    disableSensorsAndOccupancyIfIdle();
    return;
  }

  // Priority 3: Manual override.
  if (manualOverrideActive) {
    logScheduleModeChange("MANUAL_OVERRIDE");
    // Do NOT call ML. Do NOT check occupancy. Use user's targetTemp.
    applyAcState(true, manualOverrideTargetTemp, "manual");
    return;
  }

  // Pre-cool: default temp only, no ML.
  if (currentScheduleStatus.inPreCool && !currentScheduleStatus.inSchedule) {
    logScheduleModeChange("PRE_COOL");
    applyAcState(true, PRECOOL_TEMP, "pre_cool");
    return;
  }

  // Inside schedule.
  logScheduleModeChange("SCHEDULE");
  unsigned long nowMs = millis();

  if (!wasInScheduleWindow) {
    wasInScheduleWindow = true;
    scheduleWindowEnteredMillis = nowMs;
  }

  unsigned long emptyReferenceMillis = scheduleWindowEnteredMillis;
  if (lastPresenceDetectedMillis > emptyReferenceMillis) {
    emptyReferenceMillis = lastPresenceDetectedMillis;
  }

  bool scheduleNoOccupancyTooLong = (nowMs - emptyReferenceMillis) >= SCHEDULE_NO_OCC_OFF_MS;

  if (!presenceDetected && scheduleNoOccupancyTooLong) {
    applyAcState(false, acTempState, "empty");
    return;
  }

  if (!presenceDetected) {
    return;
  }

  if (!acPowerState) {
    applyAcState(true, PRECOOL_TEMP, "schedule");
  }

  if ((nowMs - lastMLCallMillis) >= ML_INTERVAL_MS || lastMLCallMillis == 0) {
    if (isnan(lastTemperature) || isnan(lastHumidity)) {
      forceReadDhtNow();
    }

    Serial.println("ML: attempt from schedule controller");
    int mlTemp = acTempState;
    if (callRenderMLAndGetTarget(mlTemp)) {
      Serial.printf("ML: apply target %d\n", mlTemp);
      applyAcState(true, mlTemp, "ml");
      lastMLCallMillis = nowMs;
    } else {
      Serial.println("ML: attempt failed, retry in ~1 minute");
      // Retry sooner on failed ML call without affecting the normal interval.
      lastMLCallMillis = nowMs - (ML_INTERVAL_MS - 60000UL);
    }
  }
}

void checkMinuteTickAndRunControl() {
  struct tm t;
  if (!timeIsValid(t)) return;

  int minuteStamp = t.tm_yday * 1440 + t.tm_hour * 60 + t.tm_min;

  // Run once immediately when time becomes valid, then once per minute.
  if (!minuteGateInitialized) {
    minuteGateInitialized = true;
    lastCheckedMinuteStamp = minuteStamp - 1;
  }

  if (minuteStamp == lastCheckedMinuteStamp) return;

  lastCheckedMinuteStamp = minuteStamp;
  runMinuteControl(t);
}

// ESP32_code.ino — checkOverrideExpiry()
void checkOverrideExpiry() {
  if (!manualOverrideActive) return;
  if (manualOverrideUntil.length() == 0) return;

  // Get current LOCAL time the same way the schedule checker does
  struct tm now;
  if (!timeIsValid(now)) return;  // same guard as checkMinuteTickAndRunControl

  // Parse overrideUntil UTC string: "2026-04-07T19:23:46.031Z"
  int yr, mo, dy, hr, mn, sc;
  String s = manualOverrideUntil;
  s.replace("Z", "");
  int dotIdx = s.indexOf('.');
  if (dotIdx > 0) s = s.substring(0, dotIdx);
  if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &yr, &mo, &dy, &hr, &mn, &sc) != 6) return;

  // Convert UTC hour:min to local by adding GMT offset
  // Work in total minutes to handle day rollover simply
  int expiryTotalMinUTC = hr * 60 + mn;
  int expiryTotalMinLocal = expiryTotalMinUTC + (int)(GMT_OFFSET_SEC / 60);  // +480 for UTC+8
  // Normalize to 0–1439
  expiryTotalMinLocal = ((expiryTotalMinLocal % 1440) + 1440) % 1440;

  // Also convert expiry date to local (only the day matters for day comparison)
  // Simplification: compare yday+year using tm fields from getLocalTime
  // Build expiry as a local tm by shifting the UTC time
  struct tm expiryLocal = {};
  expiryLocal.tm_year = yr - 1900;
  expiryLocal.tm_mon  = mo - 1;
  expiryLocal.tm_mday = dy;
  expiryLocal.tm_hour = hr + (int)(GMT_OFFSET_SEC / 3600);  // shift hour to local
  expiryLocal.tm_min  = mn;
  expiryLocal.tm_sec  = sc;
  expiryLocal.tm_isdst = 0;
  // Let mktime normalize overflow (e.g. hour 25 → next day)
  mktime(&expiryLocal);

  // Now compare local time fields, same as schedule uses tm_yday/tm_hour/tm_min
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
  coolixAc.begin();

  pinMode(PIR_PIN, PIR_ACTIVE_HIGH ? INPUT_PULLDOWN : INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIR_PIN), onPirMotion, PIR_ACTIVE_HIGH ? RISING : FALLING);

  if (!mlx.begin()) {
    Serial.println("Could not find MLX90614 sensor. Check wiring!");
    while (1) { delay(1000); }
  }

  config.database_url = String("https://") + FIREBASE_HOST;
  config.api_key = FIREBASE_API_KEY;
  auth.user.email = ESP_EMAIL;
  auth.user.password = ESP_PASSWORD;

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
}

void loop() {
  reconnectWiFiNonBlocking();
  initFirebaseIfNeeded();

  if (WiFi.status() == WL_CONNECTED && (millis() - lastNtpSyncMillis >= NTP_RESYNC_MS || lastNtpSyncMillis == 0)) {
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    lastNtpSyncMillis = millis();
  }

  if (firebaseInitialized && Firebase.ready()) {
    ensureControlStream();

    if (streamAttached && !Firebase.RTDB.readStream(&streamFbdo)) {
      String err = streamFbdo.errorReason();
      streamAttached = false;
      Firebase.RTDB.endStream(&streamFbdo);
      if (isFirebaseTokenPendingError(err)) {
        // Let token mint/refresh complete without resetting Firebase session.
      } else if (isFirebaseAuthOrSslError(err)) {
        requestFirebaseReinit(err);
      }
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

  // Evaluate schedule first to reduce end-time lag.
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
    // During override outside schedule: still read sensors, just don't ML
    refreshSensorsAndOccupancy();
  } else {
    disableSensorsAndOccupancyIfIdle();
  }
}

