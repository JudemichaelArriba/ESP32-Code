// firebase_functions.h
#ifndef FIREBASE_FUNCTIONS_H
#define FIREBASE_FUNCTIONS_H

#include "../core/structures.h"
#include "utility_functions.h"

void setControlStateToFirebase(bool active);
bool fetchAssignedRoomFromFirebase();
void syncAcStateToFirebase();
void loadAcStateFromFirebase();
void applyControlJson(JsonVariant data);
void loadControlStateFromFirebase();
void ensureControlStream();
void pollControlStream();
void reconnectWiFiNonBlocking();
void initFirebaseIfNeeded();
bool isFirebaseAuthOrSslError(const String& err);
bool isFirebaseTokenPendingError(const String& err);
bool isFirebaseRevokedError(const String& err);
void requestFirebaseReinit(const String& reason);
void clearForcedOffPersistedInFirebase();
ScheduleStatus evaluateScheduleStatus(const struct tm& t);

static const uint8_t NA_WAIT_WIFI   = 0;
static const uint8_t NA_WAIT_STABLE = 1;
static const uint8_t NA_AUTH_INIT   = 2;
static const uint8_t NA_AUTH_WAIT   = 3;
static const uint8_t NA_READY       = 4;

// Global timer to debounce our own HTTP writes from the Realtime Stream
unsigned long lastEspControlWriteMillis = 0;

static void setNetAuthState(uint8_t s) {
  if (netAuthState == s) return;
  netAuthState      = s;
  netAuthStateSince = millis();
}

static bool resolveManualOverridePower(JsonVariant data, const bool overrideActive) {
  if (!data["power"].isNull()) return data["power"].as<bool>();
  return overrideActive;
}

static bool currentScheduleWindowIsActive() {
  return (currentScheduleStatus.inPreCool || currentScheduleStatus.inSchedule) &&
         currentScheduleStatus.windowKey.length() > 0;
}

static String currentForcedOffWindowKey() {
  if (!currentScheduleWindowIsActive()) return String("");
  return currentScheduleStatus.windowKey;
}

static void copyStringToBuffer(const String& value, char* buffer, size_t bufferSize) {
  if (bufferSize == 0) return;
  strncpy(buffer, value.c_str(), bufferSize - 1);
  buffer[bufferSize - 1] = '\0';
}

static void queueForcedOffPersistence(bool persisted, const String& windowKey) {
  streamPendingAction.writeForcedOffPersisted = true;
  streamPendingAction.forcedOffPersistedVal = persisted && windowKey.length() > 0;
  streamPendingAction.writeForcedOffWindowKey = true;
  copyStringToBuffer(windowKey, streamPendingAction.forcedOffWindowKey,
                     sizeof(streamPendingAction.forcedOffWindowKey));
}

static void setForcedOffForCurrentWindow() {
  forcedOffWindowKey = currentForcedOffWindowKey();
  forcedOffActive = forcedOffWindowKey.length() > 0;
}

static void clearForcedOffLocal() {
  forcedOffActive = false;
  forcedOffWindowKey = "";
}

static void refreshScheduleStatusForForcedOff() {
  if (!assignedRoom.found) return;

  struct tm nowTm;
  if (!timeIsValid(nowTm)) return;

  currentScheduleStatus = evaluateScheduleStatus(nowTm);
}

static void queueAcStreamAction(bool power, int temp, const char* source) {
  streamPendingAction.hasPending = true;
  streamPendingAction.power = power;
  streamPendingAction.temp = temp;
  copyStringToBuffer(String(source), streamPendingAction.source,
                     sizeof(streamPendingAction.source));
}

static void queueForcedOffTrigger(const String& logPrefix) {
  refreshScheduleStatusForForcedOff();
  setForcedOffForCurrentWindow();
  manualOverrideActive = false;
  manualOverrideUntil = "";

  queueAcStreamAction(false, acTempState, "forced_off");
  queueForcedOffPersistence(forcedOffActive, forcedOffWindowKey);
  streamPendingAction.writeForcedOffFalse = true;
  Serial.println(logPrefix + " forcedOff=true queued for window key: " + forcedOffWindowKey);
}

static bool updateControlPatch(FirebaseJson& patch) {
  String base = "/devices/" + String(DEVICE_ID) + "/control";
  lastEspControlWriteMillis = millis();

  if (Firebase.RTDB.updateNode(&fbdo, base, &patch)) {
    return true;
  }

  String err = fbdo.errorReason();
  Serial.println("Control patch failed: " + err);
  if (!isFirebaseTokenPendingError(err) && isFirebaseAuthOrSslError(err)) {
    requestFirebaseReinit(err);
  }
  return false;
}

bool isFirebaseTokenPendingError(const String& err) {
  return err.indexOf("token is not ready") >= 0;
}

bool isFirebaseRevokedError(const String& err) {
  if (err.indexOf("token is not ready") >= 0) return false;
  return err.indexOf("revoked") >= 0 || err.indexOf("expired") >= 0;
}

// Catch a broader range of network/SSL failures so the client actually resets
bool isFirebaseAuthOrSslError(const String& err) {
  return isFirebaseRevokedError(err) ||
         err.indexOf("ssl") >= 0 ||
         err.indexOf("SSL") >= 0 ||
         err.indexOf("not connected") >= 0 ||
         err.indexOf("connection") >= 0 ||
         err.indexOf("TCP") >= 0 ||
         err.indexOf("closed") >= 0;
}

void requestFirebaseReinit(const String& reason) {
  unsigned long now = millis();
  if ((now - lastStreamRetryMillis) < 10000) return;

  lastStreamRetryMillis = now;
  Serial.println("Firebase reinit requested: " + reason);
  streamAttached      = false;
  firebaseInitialized = false;
  startupStateLoaded  = false;
  Firebase.RTDB.endStream(&streamFbdo);
  setNetAuthState(NA_WAIT_STABLE);
}

void setControlStateToFirebase(bool active) {
  String controlPath = "/devices/" + String(DEVICE_ID) + "/control/overrideActive";
  lastEspControlWriteMillis = millis();
  Firebase.RTDB.setBool(&fbdo, controlPath, active);
}

void clearForcedOffPersistedInFirebase() {
  clearForcedOffLocal();
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
  FirebaseJson patch;
  patch.set("forcedOffPersisted", false);
  patch.set("forcedOffWindowKey", "");
  updateControlPatch(patch);
  Serial.println("forcedOff persistence cleared in Firebase.");
}

bool fetchAssignedRoomFromFirebase() {
  RoomConfig fetchedRoom;

  if (netAuthState != NA_READY) return false;
  if (!Firebase.ready()) return false;

  unsigned long now = millis();
  if ((now - lastFirebaseInitMillis)      < FIREBASE_AUTH_SETTLE_MS) return false;
  if ((now - lastRoomsFetchAttemptMillis) < ROOMS_FETCH_RETRY_MS)    return false;
  lastRoomsFetchAttemptMillis = now;

  if (!Firebase.RTDB.getJSON(&fbdo, "/rooms")) {
    String err = fbdo.errorReason();
    Serial.println("Failed to read /rooms: " + err);
    if (isFirebaseTokenPendingError(err)) return false;
    if (isFirebaseAuthOrSslError(err)) requestFirebaseReinit(err);
    return false;
  }

  DynamicJsonDocument doc(16384);
  if (deserializeJson(doc, fbdo.jsonString()) != DeserializationError::Ok) {
    Serial.println("Failed to parse /rooms JSON");
    return false;
  }

  JsonObject rooms = doc.as<JsonObject>();
  for (JsonPair kv : rooms) {
    JsonObject room   = kv.value().as<JsonObject>();
    const char* device = room["device"] | "";
    if (String(device) != String(DEVICE_ID)) continue;

    fetchedRoom.found    = true;
    fetchedRoom.uid      = String(kv.key().c_str());
    fetchedRoom.roomName = String((const char*)(room["roomName"] | ""));
    fetchedRoom.device   = String(device);

    JsonArray schedules = room["schedules"].as<JsonArray>();
    if (!schedules.isNull()) {
      for (JsonObject item : schedules) {
        if (fetchedRoom.scheduleCount >= 16) break;
        String day       = String((const char*)(item["day"]       | ""));
        String startTime = String((const char*)(item["startTime"] | ""));
        String endTime   = String((const char*)(item["endTime"]   | ""));
        int startMin = parseTimeToMinute(startTime);
        int endMin   = parseTimeToMinute(endTime);
        if (day.length() == 0 || startMin < 0 || endMin < 0) continue;
        fetchedRoom.schedules[fetchedRoom.scheduleCount++] = {day, startMin, endMin};
      }
    }

    assignedRoom = fetchedRoom;
    Serial.printf("Assigned room: %s (%s), schedules: %d\n",
                  assignedRoom.roomName.c_str(),
                  assignedRoom.uid.c_str(),
                  assignedRoom.scheduleCount);
    return true;
  }

  assignedRoom = RoomConfig();
  Serial.println("No room matched this device ID.");
  return true;
}

void syncAcStateToFirebase() {
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;

  String basePath = "/devices/" + String(DEVICE_ID) + "/acState";
  FirebaseJson json;
  json.set("power", acPowerState);
  json.set("currentTemp", acTempState);
  json.set("source", acSourceState);
  json.set("updatedAt", nowIsoString());
  if (assignedRoom.found) {
    json.set("roomUid", assignedRoom.uid);
  }

  if (!Firebase.RTDB.setJSON(&fbdo, basePath, &json)) {
    Serial.println("Failed to sync acState: " + fbdo.errorReason());
  }
}

void loadAcStateFromFirebase() {
  String path = "/devices/" + String(DEVICE_ID) + "/acState";
  if (!Firebase.RTDB.getJSON(&fbdo, path)) return;

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, fbdo.jsonString()) != DeserializationError::Ok) return;

  if (!doc["power"].isNull())       acPowerState = doc["power"].as<bool>();
  if (!doc["currentTemp"].isNull()) acTempState  = normalizeACTemp((float)doc["currentTemp"].as<int>());
  acIrStateTrusted = false;
}

void loadControlStateFromFirebase() {
  String path = "/devices/" + String(DEVICE_ID) + "/control";
  if (!Firebase.RTDB.getJSON(&fbdo, path)) return;

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, fbdo.jsonString()) != DeserializationError::Ok) return;
  JsonVariant control = doc.as<JsonVariant>();

  bool active = false;
  if (!control["overrideActive"].isNull()) active = control["overrideActive"].as<bool>();
  else if (!control["active"].isNull())    active = control["active"].as<bool>();
  manualOverrideActive = active;

  if (!control["targetTemp"].isNull())
    manualOverrideTargetTemp = normalizeACTemp((float)control["targetTemp"].as<int>());
  else if (!control["temp"].isNull())
    manualOverrideTargetTemp = normalizeACTemp((float)control["temp"].as<int>());

  if (!control["overrideUntil"].isNull())
    manualOverrideUntil = control["overrideUntil"].as<String>();
  else
    manualOverrideUntil = "";

  manualOverridePower = resolveManualOverridePower(control, manualOverrideActive);

  String persistedWindowKey = "";
  if (!control["forcedOffWindowKey"].isNull()) {
    persistedWindowKey = control["forcedOffWindowKey"].as<String>();
  }

  const bool hasForcedOffTrigger = !control["forcedOff"].isNull() &&
                                   control["forcedOff"].as<bool>();
  const bool hasForcedOffPersisted = !control["forcedOffPersisted"].isNull() &&
                                     control["forcedOffPersisted"].as<bool>();

  if (hasForcedOffTrigger) {
    refreshScheduleStatusForForcedOff();
    setForcedOffForCurrentWindow();
    manualOverrideActive = false;
    manualOverrideUntil = "";

    FirebaseJson patch;
    patch.set("forcedOffPersisted", forcedOffActive);
    patch.set("forcedOffWindowKey", forcedOffWindowKey);
    patch.set("forcedOff", false);
    patch.set("overrideActive", false);
    updateControlPatch(patch);
    Serial.println("Startup: forcedOff=true processed for window key: " + forcedOffWindowKey);
    return;
  }

  if (hasForcedOffPersisted) {
    String currentKey = currentForcedOffWindowKey();
    if (persistedWindowKey.length() > 0 && persistedWindowKey == currentKey) {
      forcedOffActive = true;
      forcedOffWindowKey = persistedWindowKey;
      manualOverrideActive = false;
      manualOverrideUntil = "";
      Serial.println("Startup: forcedOff persisted for current window.");
    } else {
      clearForcedOffPersistedInFirebase();
      Serial.println("Startup: stale forcedOff persistence cleared.");
    }
    return;
  }

  if (persistedWindowKey.length() > 0) {
    clearForcedOffPersistedInFirebase();
  }
}

static void handleControlJsonSnapshot(const String& payload) {
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, payload) != DeserializationError::Ok) return;
  JsonVariant v = doc.as<JsonVariant>();

  if (!v["forcedOff"].isNull() && v["forcedOff"].as<bool>()) {
    queueForcedOffTrigger("Stream (json):");
    return;
  }

  const bool previousOverrideActive = manualOverrideActive;

  bool newActive = manualOverrideActive;
  if (!v["overrideActive"].isNull()) newActive = v["overrideActive"].as<bool>();
  else if (!v["active"].isNull())    newActive = v["active"].as<bool>();
  manualOverrideActive = newActive;

  if (!v["targetTemp"].isNull())
    manualOverrideTargetTemp = normalizeACTemp((float)v["targetTemp"].as<int>());
  else if (!v["temp"].isNull())
    manualOverrideTargetTemp = normalizeACTemp((float)v["temp"].as<int>());

  if (!v["overrideUntil"].isNull())
    manualOverrideUntil = v["overrideUntil"].as<String>();

  manualOverridePower = resolveManualOverridePower(v, manualOverrideActive);

  if (manualOverrideActive) {
    if (forcedOffActive) {
      clearForcedOffLocal();
      queueForcedOffPersistence(false, "");
      Serial.println("Stream (json): manual override clearing forcedOff.");
    }
    queueAcStreamAction(manualOverridePower, manualOverrideTargetTemp, "manual");
    Serial.println("Stream (json): override ON queued.");

  } else if (previousOverrideActive) {
    queueAcStreamAction(false, acTempState, "override_cleared");
    refreshScheduleStatusForForcedOff();
    setForcedOffForCurrentWindow();
    queueForcedOffPersistence(forcedOffActive, forcedOffWindowKey);
    Serial.println("Stream (json): manual override off queued forcedOff key: " + forcedOffWindowKey);
  }
}

static void handleControlStreamEvent(const String& path, const String& type) {
  Serial.println("Stream update: " + path + " [" + type + "]");

  if (lastEspControlWriteMillis > 0 &&
      (millis() - lastEspControlWriteMillis) < 4000) {
    Serial.println("Stream: Ignoring echo from ESP's own write.");
    return;
  }

  streamPendingAction = StreamPendingAction();

  if (type == "json") {
    handleControlJsonSnapshot(streamFbdo.jsonString());
    return;
  }

  if (path == "/forcedOff" && type == "boolean") {
    if (streamFbdo.boolData()) queueForcedOffTrigger("Stream:");
    return;
  }

  if ((path == "/overrideActive" || path == "/active") && type == "boolean") {
    const bool previousOverrideActive = manualOverrideActive;
    manualOverrideActive = streamFbdo.boolData();

    if (manualOverrideActive) {
      manualOverridePower = true;
      if (forcedOffActive) {
        clearForcedOffLocal();
        queueForcedOffPersistence(false, "");
        Serial.println("Stream: manual override=true clearing forcedOff.");
      }
      queueAcStreamAction(manualOverridePower, manualOverrideTargetTemp, "manual");
      Serial.println("Stream: overrideActive=true queued.");

    } else if (previousOverrideActive) {
      manualOverrideUntil = "";
      queueAcStreamAction(false, acTempState, "override_cleared");
      refreshScheduleStatusForForcedOff();
      setForcedOffForCurrentWindow();
      queueForcedOffPersistence(forcedOffActive, forcedOffWindowKey);
      Serial.println("Stream: manual override off queued forcedOff key: " + forcedOffWindowKey);

    }
    return;
  }

  if ((path == "/targetTemp" || path == "/temp") &&
      (type == "int" || type == "float" || type == "double")) {
    manualOverrideTargetTemp = normalizeACTemp((float)streamFbdo.floatData());
    if (manualOverrideActive) {
      if (!manualOverridePower) manualOverridePower = true;
      queueAcStreamAction(manualOverridePower, manualOverrideTargetTemp, "manual");
    }
    return;
  }

  if (path == "/overrideUntil" && type == "string") {
    manualOverrideUntil = streamFbdo.stringData();
    return;
  }

  if (path == "/power" && type == "boolean") {
    manualOverridePower = streamFbdo.boolData();
    if (manualOverrideActive) {
      queueAcStreamAction(manualOverridePower, manualOverrideTargetTemp, "manual");
    }
    return;
  }
}

void ensureControlStream() {
  if (!firebaseInitialized || WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
  if (netAuthState != NA_READY) return;
  if (streamAttached) return;

  unsigned long now = millis();
  if ((now - lastStreamRetryMillis)  < 5000)                  return;
  if ((now - lastFirebaseInitMillis) < FIREBASE_AUTH_SETTLE_MS) return;

  String streamPath = "/devices/" + String(DEVICE_ID) + "/control";
  if (!Firebase.RTDB.beginStream(&streamFbdo, streamPath)) {
    String err = streamFbdo.errorReason();
    Serial.println("Control stream begin failed: " + err);
    lastStreamRetryMillis = now;
    if (isFirebaseTokenPendingError(err)) return;
    if (isFirebaseAuthOrSslError(err)) requestFirebaseReinit(err);
    return;
  }

  streamAttached = true;
}

void pollControlStream() {
  if (!streamAttached || !firebaseInitialized || WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
  if (netAuthState != NA_READY) return;

  if (!Firebase.RTDB.readStream(&streamFbdo)) {
    String err = streamFbdo.errorReason();
    if (err.length() > 0) {
      Serial.println("Control stream read failed: " + err);
    }
    if (isFirebaseTokenPendingError(err)) return;
    if (isFirebaseAuthOrSslError(err)) requestFirebaseReinit(err);
    return;
  }

  if (streamFbdo.streamTimeout()) {
    Serial.println("Firebase stream timeout, auto-resuming...");
    if (!streamFbdo.httpConnected()) {
      String err = streamFbdo.errorReason();
      if (!isFirebaseTokenPendingError(err) && isFirebaseAuthOrSslError(err)) {
        requestFirebaseReinit(err);
      }
    }
  }

  if (!streamFbdo.streamAvailable()) return;

  handleControlStreamEvent(streamFbdo.dataPath(), streamFbdo.dataType());
}

void reconnectWiFiNonBlocking() {
  wl_status_t status = WiFi.status();

  if (status == WL_CONNECTED) {
    if (!wifiLinkUp) {
      wifiLinkUp              = true;
      lastWiFiConnectedMillis = millis();
      streamAttached          = false;
      startupStateLoaded      = false;
      firebaseInitialized     = false;
      setNetAuthState(NA_WAIT_STABLE);

      if (!wifiHasConnectedOnce) {
        wifiHasConnectedOnce = true;
      } else {
        wifiReconnectRestartPending = true;
        wifiReconnectStableSince    = millis();
        Serial.println("WiFi restored, waiting stable then restarting...");
      }
    }

    if (wifiReconnectRestartPending &&
        (millis() - wifiReconnectStableSince) >= WIFI_RECONNECT_RESTART_STABLE_MS) {
      Serial.println("WiFi stable after reconnect, restarting ESP...");
      delay(100);
      ESP.restart();
    }
    return;
  }

  if (wifiLinkUp) wifiReconnectRestartPending = false;

  wifiLinkUp = false;
  setNetAuthState(NA_WAIT_WIFI);

  if (status == WL_IDLE_STATUS) return;

  unsigned long now = millis();
  if (now - lastWiFiReconnectAttempt < WIFI_RECONNECT_MS) return;

  lastWiFiReconnectAttempt = now;
  streamAttached           = false;
  firebaseInitialized      = false;
  startupStateLoaded       = false;
  Serial.println("WiFi disconnected, reconnecting...");
  WiFi.reconnect();
}

void initFirebaseIfNeeded() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!wifiLinkUp) return;

  struct tm t;
  if (!timeIsValid(t)) return;

  unsigned long now = millis();

  if (netAuthState == NA_WAIT_STABLE) {
    if ((now - lastWiFiConnectedMillis) < WIFI_STABLE_BEFORE_FB_MS) return;
    setNetAuthState(NA_AUTH_INIT);
  }

  if (netAuthState == NA_AUTH_INIT) {
    // Bump BSSL buffer slightly to give the SSL engine more breathing room
    fbdo.setBSSLBufferSize(8192, 2048);
    streamFbdo.setBSSLBufferSize(8192, 2048);
    config.timeout.socketConnection = 10000;

    Firebase.begin(&config, &auth);
    Firebase.reconnectNetwork(true);
    firebaseInitialized    = true;
    streamAttached         = false;
    startupStateLoaded     = false;
    lastFirebaseInitMillis = millis();
    Serial.println("Firebase initialized.");
    setNetAuthState(NA_AUTH_WAIT);
    return;
  }

  if (netAuthState == NA_AUTH_WAIT) {
    if (!Firebase.ready()) return;
    if ((now - lastFirebaseInitMillis) < FIREBASE_AUTH_SETTLE_MS) return;
    setNetAuthState(NA_READY);
    return;
  }
}

void clearOverrideInFirebase() {
  String base = "/devices/" + String(DEVICE_ID) + "/control";
  lastEspControlWriteMillis = millis();
  Firebase.RTDB.setBool(&fbdo, base + "/overrideActive", false);

  String acBase = "/devices/" + String(DEVICE_ID) + "/acState";
  Firebase.RTDB.setString(&fbdo, acBase + "/source", "override_expired");
}

#endif
