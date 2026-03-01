#include "DHT.h"
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_MLX90614.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <ir_Coolix.h>
#include <time.h>
#include "secrets.h"


#define DHTPIN  18
#define DHTTYPE DHT22
#define PIR_PIN 4
#define IR_LED_PIN 14

const char* renderURL = "https://ocutemp-backend.onrender.com/predict";
const char* NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_SEC = 8 * 3600;
const int DAYLIGHT_OFFSET_SEC = 0;

DHT dht(DHTPIN, DHTTYPE);
FirebaseData fbdo;
FirebaseData streamFbdo;
FirebaseAuth auth;
FirebaseConfig config;
Adafruit_MLX90614 mlx = Adafruit_MLX90614();
IRCoolixAC coolixAc(IR_LED_PIN);

struct ScheduleSlot {
  String day;
  int startMinute;
  int endMinute;
};

struct RoomConfig {
  bool found = false;
  String uid;
  String roomName;
  String device;
  ScheduleSlot schedules[16];
  int scheduleCount = 0;
};

struct ScheduleStatus {
  bool hasScheduleToday = false;
  bool inPreCool = false;
  bool inSchedule = false;
};

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

const float occupancyThreshold = 30.0f;
const bool PIR_ACTIVE_HIGH = true;
const unsigned long PIR_RETRIGGER_GUARD_MS = 80;
const unsigned long PIR_HOLD_MS = 3UL * 60UL * 1000UL;
const unsigned long OCCUPANCY_EMPTY_OFF_MS = 20UL * 60UL * 1000UL;
const unsigned long DHT_INTERVAL_MS = 7000;
const unsigned long MLX_INTERVAL_MS = 3000;
const unsigned long ML_INTERVAL_MS = 30UL * 60UL * 1000UL;
const unsigned long WIFI_RECONNECT_MS = 5000;
const unsigned long NTP_RESYNC_MS = 60000;

const int AC_TEMP_MIN = 17;
const int AC_TEMP_MAX = 30;
const int PRECOOL_MINUTES = 15;
const int PRECOOL_TEMP = 24;

void IRAM_ATTR onPirMotion() {
  const unsigned long nowMs = millis();
  if ((nowMs - lastPirInterruptMillis) >= PIR_RETRIGGER_GUARD_MS) {
    pirMotionLatched = true;
    lastPirInterruptMillis = nowMs;
  }
}

int normalizeACTemp(float recommendedTemp) {
  int target = (int)roundf(recommendedTemp);
  if (target < AC_TEMP_MIN) target = AC_TEMP_MIN;
  if (target > AC_TEMP_MAX) target = AC_TEMP_MAX;
  return target;
}

String nowIsoString() {
  struct tm t;
  if (!getLocalTime(&t, 50)) return String("1970-01-01T00:00:00");

  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &t);
  return String(buf);
}

bool timeIsValid(struct tm& t) {
  if (!getLocalTime(&t, 50)) return false;
  return (t.tm_year + 1900) >= 2024;
}

int parseTimeToMinute(const String& hhmm) {
  int sep = hhmm.indexOf(':');
  if (sep <= 0) return -1;

  int hh = hhmm.substring(0, sep).toInt();
  int mm = hhmm.substring(sep + 1).toInt();
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return -1;
  return hh * 60 + mm;
}

const char* weekdayName(int wday) {
  static const char* names[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
  };
  if (wday < 0 || wday > 6) return "";
  return names[wday];
}

bool sameDay(const String& scheduleDay, int wday) {
  String d = scheduleDay;
  d.trim();
  d.toLowerCase();

  String cur = String(weekdayName(wday));
  cur.toLowerCase();

  return d == cur;
}

bool isWithinMinuteRange(int nowMin, int startMin, int endMin) {
  if (endMin >= startMin) {
    return nowMin >= startMin && nowMin < endMin;
  }
  return (nowMin >= startMin || nowMin < endMin);
}

void setControlStateToFirebase(bool active) {
  String controlPath = "/devices/" + String(DEVICE_ID) + "/control/overrideActive";
  Firebase.RTDB.setBool(&fbdo, controlPath, active);
}

bool fetchAssignedRoomFromFirebase() {
  assignedRoom = RoomConfig();

  if (!Firebase.RTDB.getJSON(&fbdo, "/rooms")) {
    Serial.println("Failed to read /rooms: " + fbdo.errorReason());
    return false;
  }

  DynamicJsonDocument doc(16384);
  if (deserializeJson(doc, fbdo.jsonString()) != DeserializationError::Ok) {
    Serial.println("Failed to parse /rooms JSON");
    return false;
  }

  JsonObject rooms = doc.as<JsonObject>();
  for (JsonPair kv : rooms) {
    JsonObject room = kv.value().as<JsonObject>();
    const char* device = room["device"] | "";
    if (String(device) != String(DEVICE_ID)) continue;

    assignedRoom.found = true;
    assignedRoom.uid = String(kv.key().c_str());
    assignedRoom.roomName = String((const char*)(room["roomName"] | ""));
    assignedRoom.device = String(device);

    JsonArray schedules = room["schedules"].as<JsonArray>();
    if (!schedules.isNull()) {
      for (JsonObject item : schedules) {
        if (assignedRoom.scheduleCount >= 16) break;

        String day = String((const char*)(item["day"] | ""));
        String startTime = String((const char*)(item["startTime"] | ""));
        String endTime = String((const char*)(item["endTime"] | ""));

        int startMin = parseTimeToMinute(startTime);
        int endMin = parseTimeToMinute(endTime);
        if (day.length() == 0 || startMin < 0 || endMin < 0) continue;

        assignedRoom.schedules[assignedRoom.scheduleCount++] = {day, startMin, endMin};
      }
    }

    Serial.printf("Assigned room: %s (%s), schedules: %d\n",
                  assignedRoom.roomName.c_str(),
                  assignedRoom.uid.c_str(),
                  assignedRoom.scheduleCount);
    return true;
  }

  Serial.println("No room matched this device ID.");
  return false;
}

void syncAcStateToFirebase() {
  String basePath = "/devices/" + String(DEVICE_ID) + "/acState";
  Firebase.RTDB.setBool(&fbdo, basePath + "/power", acPowerState);
  Firebase.RTDB.setInt(&fbdo, basePath + "/currentTemp", acTempState);
  Firebase.RTDB.setString(&fbdo, basePath + "/source", acSourceState);
  Firebase.RTDB.setString(&fbdo, basePath + "/updatedAt", nowIsoString());
  if (assignedRoom.found) {
    Firebase.RTDB.setString(&fbdo, basePath + "/roomUid", assignedRoom.uid);
  }
}

bool applyAcState(bool targetPower, int targetTemp, const String& source) {
  targetTemp = normalizeACTemp((float)targetTemp);

  if (targetPower == acPowerState && (!targetPower || targetTemp == acTempState)) {
    return false;
  }

  if (targetPower) {
    coolixAc.on();
    coolixAc.setMode(kCoolixCool);
    coolixAc.setTemp(targetTemp);
    coolixAc.send();
    acTempState = targetTemp;
    Serial.printf("IR: AC ON %dC (%s)\n", targetTemp, source.c_str());
  } else {
    coolixAc.off();
    coolixAc.send();
    Serial.printf("IR: AC OFF (%s)\n", source.c_str());
  }

  acPowerState = targetPower;
  acSourceState = source;
  syncAcStateToFirebase();
  return true;
}

void loadAcStateFromFirebase() {
  String path = "/devices/" + String(DEVICE_ID) + "/acState";
  if (!Firebase.RTDB.getJSON(&fbdo, path)) return;

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, fbdo.jsonString()) != DeserializationError::Ok) return;

  if (!doc["power"].isNull()) acPowerState = doc["power"].as<bool>();
  if (!doc["currentTemp"].isNull()) acTempState = normalizeACTemp((float)doc["currentTemp"].as<int>());
}

void applyControlJson(JsonVariant data) {
  if (data.isNull()) return;

  bool hasOverride = !data["overrideActive"].isNull() || !data["active"].isNull();
  bool hasPower = !data["power"].isNull();
  bool hasTemp = !data["temp"].isNull();

  if (hasOverride) {
    if (!data["overrideActive"].isNull()) {
      manualOverrideActive = data["overrideActive"].as<bool>();
    } else {
      manualOverrideActive = data["active"].as<bool>();
    }
  }

  if (hasPower) manualOverridePower = data["power"].as<bool>();
  if (hasTemp) manualOverrideTemp = normalizeACTemp((float)data["temp"].as<int>());

  if (manualOverrideActive || hasPower || hasTemp) {
    manualOverrideActive = true;
    if (!hasPower) manualOverridePower = acPowerState || hasTemp;
    if (!hasTemp) manualOverrideTemp = acTempState;
    applyAcState(manualOverridePower, manualOverrideTemp, "manual");
  }
}

void loadControlStateFromFirebase() {
  String path = "/devices/" + String(DEVICE_ID) + "/control";
  if (!Firebase.RTDB.getJSON(&fbdo, path)) return;

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, fbdo.jsonString()) != DeserializationError::Ok) return;

  if (!doc["overrideActive"].isNull()) manualOverrideActive = doc["overrideActive"].as<bool>();
  else if (!doc["active"].isNull()) manualOverrideActive = doc["active"].as<bool>();

  if (!doc["power"].isNull()) manualOverridePower = doc["power"].as<bool>();
  if (!doc["temp"].isNull()) manualOverrideTemp = normalizeACTemp((float)doc["temp"].as<int>());
}

void streamCallback(FirebaseStream data) {
  String path = data.dataPath();
  String type = data.dataType();

  if (type == "json") {
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, data.stringData()) == DeserializationError::Ok) {
      applyControlJson(doc.as<JsonVariant>());
    }
    return;
  }

  if ((path == "/overrideActive" || path == "/active") && type == "boolean") {
    manualOverrideActive = data.boolData();
    if (!manualOverrideActive) {
      acSourceState = "manual_cleared";
      syncAcStateToFirebase();
    } else {
      manualOverridePower = acPowerState;
      manualOverrideTemp = acTempState;
    }
    return;
  }

  if (path == "/power" && type == "boolean") {
    manualOverridePower = data.boolData();
    manualOverrideActive = true;
    applyAcState(manualOverridePower, acTempState, "manual");
    return;
  }

  if (path == "/temp" && (type == "int" || type == "float" || type == "double")) {
    manualOverrideTemp = normalizeACTemp((float)data.floatData());
    manualOverrideActive = true;
    manualOverridePower = true;
    applyAcState(true, manualOverrideTemp, "manual");
    return;
  }
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) {
    streamAttached = false;
    Serial.println("Firebase stream timeout, retry pending.");
  }
}

void ensureControlStream() {
  if (!firebaseInitialized || WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
  if (streamAttached) return;

  String streamPath = "/devices/" + String(DEVICE_ID) + "/control";
  if (!Firebase.RTDB.beginStream(&streamFbdo, streamPath)) {
    Serial.println("Control stream begin failed: " + streamFbdo.errorReason());
    return;
  }

  Firebase.RTDB.setStreamCallback(&streamFbdo, streamCallback, streamTimeoutCallback);
  streamAttached = true;
}

void reconnectWiFiNonBlocking() {
  if (WiFi.status() == WL_CONNECTED) return;

  unsigned long now = millis();
  if (now - lastWiFiReconnectAttempt < WIFI_RECONNECT_MS) return;

  lastWiFiReconnectAttempt = now;
  streamAttached = false;
  firebaseInitialized = false;
  startupStateLoaded = false;
  Serial.println("WiFi disconnected, reconnecting...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void initFirebaseIfNeeded() {
  if (WiFi.status() != WL_CONNECTED || firebaseInitialized) return;

  Firebase.begin(&config, &auth);
  Firebase.reconnectNetwork(true);
  firebaseInitialized = true;
  streamAttached = false;
  startupStateLoaded = false;
  Serial.println("Firebase initialized.");
}

ScheduleStatus evaluateScheduleStatus(const struct tm& t) {
  ScheduleStatus status;

  if (!assignedRoom.found) return status;

  int nowMinute = t.tm_hour * 60 + t.tm_min;
  for (int i = 0; i < assignedRoom.scheduleCount; i++) {
    ScheduleSlot& s = assignedRoom.schedules[i];
    if (!sameDay(s.day, t.tm_wday)) continue;

    status.hasScheduleToday = true;

    int preCoolStart = s.startMinute - PRECOOL_MINUTES;
    if (preCoolStart < 0) preCoolStart += 1440;

    if (isWithinMinuteRange(nowMinute, preCoolStart, s.startMinute)) {
      status.inPreCool = true;
    }

    if (isWithinMinuteRange(nowMinute, s.startMinute, s.endMinute)) {
      status.inSchedule = true;
    }
  }

  return status;
}

bool shouldPollSensors() {
  return manualOverrideActive || currentScheduleStatus.inPreCool || currentScheduleStatus.inSchedule;
}

void pushOccupancyIfChanged() {
  if (lastPresenceReported == presenceDetected) return;
  lastPresenceReported = presenceDetected;

  String basePath = "/devices/" + String(DEVICE_ID);
  Firebase.RTDB.setBool(&fbdo, basePath + "/occupancy", presenceDetected);
}

void refreshSensorsAndOccupancy() {
  const unsigned long now = millis();

  if (pirMotionLatched) {
    pirMotionLatched = false;
    lastPirMotionMillis = now;
  }

  const bool pirRawActive = PIR_ACTIVE_HIGH ? (digitalRead(PIR_PIN) == HIGH) : (digitalRead(PIR_PIN) == LOW);
  pirMotionDetected = pirRawActive;

  if ((now - lastDhtReadMillis) >= DHT_INTERVAL_MS || lastDhtReadMillis == 0) {
    lastDhtReadMillis = now;
    float humidity = dht.readHumidity();
    float temperature = dht.readTemperature();

    if (!isnan(humidity) && !isnan(temperature)) {
      lastHumidity = humidity;
      lastTemperature = temperature;

      String basePath = "/devices/" + String(DEVICE_ID);
      Firebase.RTDB.setFloat(&fbdo, basePath + "/temperature", lastTemperature);
      Firebase.RTDB.setFloat(&fbdo, basePath + "/humidity", lastHumidity);
    }
  }

  if ((now - lastMlxReadMillis) >= MLX_INTERVAL_MS || lastMlxReadMillis == 0) {
    lastMlxReadMillis = now;
    mlxObjectTemp = mlx.readObjectTempC();
    mlxAmbientTemp = mlx.readAmbientTempC();
    mlxPresenceDetected = !isnan(mlxObjectTemp) && (mlxObjectTemp > occupancyThreshold);
  }

  const bool pirRecentMotion = (lastPirMotionMillis != 0) && ((now - lastPirMotionMillis) <= PIR_HOLD_MS);
  const bool anyDetected = mlxPresenceDetected || pirRawActive || pirRecentMotion;

  if (anyDetected) {
    lastPresenceDetectedMillis = now;
  }

  presenceDetected = (lastPresenceDetectedMillis != 0) && ((now - lastPresenceDetectedMillis) <= PIR_HOLD_MS);
  pushOccupancyIfChanged();
}

void disableSensorsAndOccupancyIfIdle() {
  if (presenceDetected) {
    presenceDetected = false;
    lastPresenceDetectedMillis = 0;
    pushOccupancyIfChanged();
  }
}

bool callRenderMLAndGetTarget(int& targetTempOut) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (isnan(lastTemperature) || isnan(lastHumidity)) return false;

  HTTPClient http;
  http.begin(renderURL);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> doc;
  JsonArray rooms = doc.createNestedArray("rooms");
  JsonObject room1 = rooms.createNestedObject();
  room1["id"] = assignedRoom.found ? assignedRoom.uid : String(DEVICE_ID);
  room1["temperature"] = lastTemperature;
  room1["humidity"] = lastHumidity;

  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  if (code <= 0) {
    http.end();
    return false;
  }

  String response = http.getString();
  http.end();

  DynamicJsonDocument res(1024);
  if (deserializeJson(res, response) != DeserializationError::Ok) return false;
  if (!res[0]["recommended_ac"].is<float>() && !res[0]["recommended_ac"].is<int>()) return false;

  targetTempOut = normalizeACTemp(res[0]["recommended_ac"].as<float>());
  return true;
}

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