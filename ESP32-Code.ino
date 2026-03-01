#include "core/structures.h"
#include "functions/utility_functions.h"
#include "functions/firebase_functions.h"
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

float lastHumidity = NAN;
float lastTemperature = NAN;
float mlxObjectTemp = NAN;
float mlxAmbientTemp = NAN;

bool pirMotionDetected = false;
bool mlxPresenceDetected = false;
bool presenceDetected = false;
bool lastPresenceReported = false;

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
int lastCheckedMinuteStamp = -1;
bool minuteGateInitialized = false;

void runMinuteControl(const struct tm& t) {
  fetchAssignedRoomFromFirebase();

  if (!assignedRoom.found) {
    applyAcState(false, acTempState, "no_assigned_room");
    currentScheduleStatus = ScheduleStatus();
    return;
  }

  currentScheduleStatus = evaluateScheduleStatus(t);

  // Priority 1: No schedule today -> AC off.
  if (!currentScheduleStatus.hasScheduleToday) {
    manualOverrideActive = false;
    setControlStateToFirebase(false);
    applyAcState(false, acTempState, "no_schedule_today");
    return;
  }

  const bool inAnyWindow = currentScheduleStatus.inPreCool || currentScheduleStatus.inSchedule;

  // Clear manual override when schedule window is gone.
  if (manualOverrideActive && !inAnyWindow) {
    manualOverrideActive = false;
    setControlStateToFirebase(false);
  }

  // Priority 2: Outside schedule windows -> AC off.
  if (!inAnyWindow) {
    applyAcState(false, acTempState, "outside_schedule");
    return;
  }

  // Priority 3: Manual override.
  if (manualOverrideActive) {
    applyAcState(manualOverridePower, manualOverrideTemp, "manual");
    return;
  }

  // Pre-cool: default temp only, no ML.
  if (currentScheduleStatus.inPreCool && !currentScheduleStatus.inSchedule) {
    applyAcState(true, PRECOOL_TEMP, "schedule");
    return;
  }

  // Inside schedule.
  unsigned long nowMs = millis();
  bool emptyTooLong = (lastPresenceDetectedMillis == 0) || ((nowMs - lastPresenceDetectedMillis) >= OCCUPANCY_EMPTY_OFF_MS);

  if (emptyTooLong) {
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
    int mlTemp = acTempState;
    if (callRenderMLAndGetTarget(mlTemp)) {
      applyAcState(true, mlTemp, "ml");
    }
    lastMLCallMillis = nowMs;
  }
}

void checkMinuteTickAndRunControl() {
  struct tm t;
  if (!timeIsValid(t)) return;

  int minuteStamp = t.tm_yday * 1440 + t.tm_hour * 60 + t.tm_min;

  if (!minuteGateInitialized) {
    minuteGateInitialized = true;
    lastCheckedMinuteStamp = minuteStamp;
    return;
  }

  if (minuteStamp == lastCheckedMinuteStamp) return;

  lastCheckedMinuteStamp = minuteStamp;
  runMinuteControl(t);
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
      streamAttached = false;
    }

    if (!startupStateLoaded) {
      fetchAssignedRoomFromFirebase();
      loadAcStateFromFirebase();
      loadControlStateFromFirebase();
      syncAcStateToFirebase();
      startupStateLoaded = true;
    }
  }

  if (shouldPollSensors()) {
    refreshSensorsAndOccupancy();
  } else {
    disableSensorsAndOccupancyIfIdle();
  }

  checkMinuteTickAndRunControl();
}