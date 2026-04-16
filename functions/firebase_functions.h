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
void streamCallback(FirebaseStream data);
void streamTimeoutCallback(bool timeout);
void ensureControlStream();
void reconnectWiFiNonBlocking();
void initFirebaseIfNeeded();
bool isFirebaseAuthOrSslError(const String& err);
bool isFirebaseTokenPendingError(const String& err);
bool isFirebaseRevokedError(const String& err);
void requestFirebaseReinit(const String& reason);
void clearForcedOffPersistedInFirebase();  

static const uint8_t NA_WAIT_WIFI   = 0;
static const uint8_t NA_WAIT_STABLE = 1;
static const uint8_t NA_AUTH_INIT   = 2;
static const uint8_t NA_AUTH_WAIT   = 3;
static const uint8_t NA_READY       = 4;

static void setNetAuthState(uint8_t s) {
  if (netAuthState == s) return;
  netAuthState = s;
  netAuthStateSince = millis();
}

bool isFirebaseTokenPendingError(const String& err) {
  return err.indexOf("token is not ready") >= 0;
}

bool isFirebaseRevokedError(const String& err) {
  if (err.indexOf("token is not ready") >= 0) return false;
  return err.indexOf("revoked") >= 0 || err.indexOf("expired") >= 0;
}

bool isFirebaseAuthOrSslError(const String& err) {
  return isFirebaseRevokedError(err) ||
         err.indexOf("ssl") >= 0 ||
         err.indexOf("SSL") >= 0;
}

void requestFirebaseReinit(const String& reason) {
  unsigned long now = millis();
  if ((now - lastStreamRetryMillis) < 10000) return;

  lastStreamRetryMillis = now;
  Serial.println("Firebase reinit requested: " + reason);
  streamAttached = false;
  firebaseInitialized = false;
  startupStateLoaded = false;
  Firebase.RTDB.endStream(&streamFbdo);
  setNetAuthState(NA_WAIT_STABLE);
}

void setControlStateToFirebase(bool active) {
  String controlPath = "/devices/" + String(DEVICE_ID) + "/control/overrideActive";
  Firebase.RTDB.setBool(&fbdo, controlPath, active);
}


// Clears the persistent forcedOff flag in Firebase.
// Call this from the main loop only (never from a stream callback).

void clearForcedOffPersistedInFirebase() {
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
  String base = "/devices/" + String(DEVICE_ID) + "/control";
  Firebase.RTDB.setBool(&fbdo, base + "/forcedOffPersisted", false);
  Serial.println("forcedOffPersisted cleared in Firebase.");
}

bool fetchAssignedRoomFromFirebase() {
  RoomConfig fetchedRoom;

  if (netAuthState != NA_READY) return false;
  if (!Firebase.ready()) return false;

  unsigned long now = millis();
  if ((now - lastFirebaseInitMillis) < FIREBASE_AUTH_SETTLE_MS) return false;
  if ((now - lastRoomsFetchAttemptMillis) < ROOMS_FETCH_RETRY_MS) return false;
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
    JsonObject room = kv.value().as<JsonObject>();
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
  String basePath = "/devices/" + String(DEVICE_ID) + "/acState";
  Firebase.RTDB.setBool(&fbdo,   basePath + "/power",       acPowerState);
  Firebase.RTDB.setInt(&fbdo,    basePath + "/currentTemp", acTempState);
  Firebase.RTDB.setString(&fbdo, basePath + "/source",      acSourceState);
  Firebase.RTDB.setString(&fbdo, basePath + "/updatedAt",   nowIsoString());
  if (assignedRoom.found) {
    Firebase.RTDB.setString(&fbdo, basePath + "/roomUid", assignedRoom.uid);
  }
}

void loadAcStateFromFirebase() {
  String path = "/devices/" + String(DEVICE_ID) + "/acState";
  if (!Firebase.RTDB.getJSON(&fbdo, path)) return;

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, fbdo.jsonString()) != DeserializationError::Ok) return;

  if (!doc["power"].isNull())       acPowerState = doc["power"].as<bool>();
  if (!doc["currentTemp"].isNull()) acTempState  = normalizeACTemp((float)doc["currentTemp"].as<int>());
}

void applyControlJson(JsonVariant data);  // Forward declaration

void loadControlStateFromFirebase() {
  String path = "/devices/" + String(DEVICE_ID) + "/control";
  if (!Firebase.RTDB.getJSON(&fbdo, path)) return;

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, fbdo.jsonString()) != DeserializationError::Ok) return;

  bool active = false;
  if (!doc["overrideActive"].isNull()) active = doc["overrideActive"].as<bool>();
  else if (!doc["active"].isNull())    active = doc["active"].as<bool>();
  manualOverrideActive = active;

  if (!doc["targetTemp"].isNull())
    manualOverrideTargetTemp = normalizeACTemp((float)doc["targetTemp"].as<int>());
  else if (!doc["temp"].isNull())
    manualOverrideTargetTemp = normalizeACTemp((float)doc["temp"].as<int>());

  if (!doc["overrideUntil"].isNull())
    manualOverrideUntil = doc["overrideUntil"].as<String>();
  else
    manualOverrideUntil = "";

  if (!doc["power"].isNull()) manualOverridePower = doc["power"].as<bool>();


  if (!doc["forcedOff"].isNull() && doc["forcedOff"].as<bool>()) {
    forcedOffActive     = true;
    forcedOffSeenWindow = currentScheduleStatus.inPreCool || currentScheduleStatus.inSchedule;
    manualOverrideActive = false;
    manualOverrideUntil  = "";

    String base = "/devices/" + String(DEVICE_ID) + "/control";
    Firebase.RTDB.setBool(&fbdo, base + "/forcedOffPersisted", true);
    Firebase.RTDB.setBool(&fbdo, base + "/forcedOff", false);         
    Serial.println("Startup: forcedOff=true — persisted and cleared trigger.");
    return;  
  }

  // Persistent forcedOff: survives ESP restarts even after the one-shot trigger was cleared.
  // forcedOffSeenWindow=true is conservative — keeps AC off for the current window;
  // clears naturally when the window ends and a new one begins.
  if (!doc["forcedOffPersisted"].isNull() && doc["forcedOffPersisted"].as<bool>()) {
    if (!forcedOffActive) {
      forcedOffActive     = true;
      forcedOffSeenWindow = true;
      manualOverrideActive = false;
      manualOverrideUntil  = "";
      Serial.println("Startup: forcedOffPersisted=true — AC held off until new schedule window.");
    }
  }
}


// Forward declarations needed inside streamCallback
void streamCallback(FirebaseStream data);
bool applyAcState(bool targetPower, int targetTemp, const String& source);

// streamCallback — MUST NOT call Firebase RTDB operations or applyAcState.
// Calling Firebase from inside readStream() re-enters the SSL layer and
// causes the "SSL Internal error / Guru Meditation" crash seen in the logs.
//
// Instead: update in-memory state only, then populate streamPendingAction.
// The main loop processes it after readStream() has fully returned.
void streamCallback(FirebaseStream data) {
  const String path = data.dataPath();
  const String type = data.dataType();

  Serial.println("Stream update: " + path + " [" + type + "]");

  // Always start with a clean pending slot for this event.
  streamPendingAction = StreamPendingAction();


  if (type == "json") {
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, data.stringData()) != DeserializationError::Ok) return;
    JsonVariant v = doc.as<JsonVariant>();

    // forcedOff takes priority over everything else in the object
    if (!v["forcedOff"].isNull() && v["forcedOff"].as<bool>()) {
      forcedOffActive     = true;
      forcedOffSeenWindow = currentScheduleStatus.inPreCool || currentScheduleStatus.inSchedule;
      manualOverrideActive = false;
      manualOverrideUntil  = "";

      streamPendingAction.hasPending              = true;
      streamPendingAction.power                   = false;
      streamPendingAction.temp                    = acTempState;
      strncpy(streamPendingAction.source, "forced_off", sizeof(streamPendingAction.source) - 1);
      streamPendingAction.writeForcedOffPersisted = true;
      streamPendingAction.forcedOffPersistedVal   = true;   // persist BEFORE clearing trigger
      streamPendingAction.writeForcedOffFalse     = true;
      Serial.println("Stream (json): forcedOff=true — deferred apply + Firebase writes queued.");
      return;
    }

    // Parse the override fields into memory
    bool newActive = false;
    if (!v["overrideActive"].isNull()) newActive = v["overrideActive"].as<bool>();
    else if (!v["active"].isNull())    newActive = v["active"].as<bool>();
    manualOverrideActive = newActive;

    if (!v["targetTemp"].isNull())
      manualOverrideTargetTemp = normalizeACTemp((float)v["targetTemp"].as<int>());
    else if (!v["temp"].isNull())
      manualOverrideTargetTemp = normalizeACTemp((float)v["temp"].as<int>());

    if (!v["overrideUntil"].isNull())
      manualOverrideUntil = v["overrideUntil"].as<String>();
    else
      manualOverrideUntil = "";

    if (!v["power"].isNull()) manualOverridePower = v["power"].as<bool>();

    streamPendingAction.hasPending = true;

    if (manualOverrideActive) {
      if (forcedOffActive) {
        // New manual override always clears forcedOff
        forcedOffActive     = false;
        forcedOffSeenWindow = false;
        streamPendingAction.writeForcedOffPersisted = true;
        streamPendingAction.forcedOffPersistedVal   = false;
        Serial.println("Stream (json): manual override — clearing forcedOff.");
      }
      streamPendingAction.power = true;
      streamPendingAction.temp  = manualOverrideTargetTemp;
      strncpy(streamPendingAction.source, "manual", sizeof(streamPendingAction.source) - 1);
      Serial.println("Stream: override ON — deferred apply queued.");
    } else {
      streamPendingAction.power = false;
      streamPendingAction.temp  = acTempState;
      strncpy(streamPendingAction.source, "override_cleared", sizeof(streamPendingAction.source) - 1);
      Serial.println("Stream: override OFF — deferred apply queued.");
    }
    return;
  }

  // Individual field updates
  if (path == "/forcedOff" && type == "boolean") {
    if (data.boolData()) {
      forcedOffActive     = true;
      forcedOffSeenWindow = currentScheduleStatus.inPreCool || currentScheduleStatus.inSchedule;
      manualOverrideActive = false;
      manualOverrideUntil  = "";

      streamPendingAction.hasPending              = true;
      streamPendingAction.power                   = false;
      streamPendingAction.temp                    = acTempState;
      strncpy(streamPendingAction.source, "forced_off", sizeof(streamPendingAction.source) - 1);
      streamPendingAction.writeForcedOffPersisted = true;
      streamPendingAction.forcedOffPersistedVal   = true;
      streamPendingAction.writeForcedOffFalse     = true;
      Serial.println("Stream: /forcedOff=true — deferred apply + Firebase writes queued.");
    }
    return;
  }

  if ((path == "/overrideActive" || path == "/active") && type == "boolean") {
    manualOverrideActive = data.boolData();
    streamPendingAction.hasPending = true;

    if (!manualOverrideActive) {
      manualOverrideUntil = "";
      streamPendingAction.power = false;
      streamPendingAction.temp  = acTempState;
      strncpy(streamPendingAction.source, "override_cleared", sizeof(streamPendingAction.source) - 1);
      Serial.println("Stream: overrideActive=false — deferred apply queued.");
    } else {
      if (forcedOffActive) {
        forcedOffActive     = false;
        forcedOffSeenWindow = false;
        streamPendingAction.writeForcedOffPersisted = true;
        streamPendingAction.forcedOffPersistedVal   = false;
        Serial.println("Stream: manual override=true — clearing forcedOff.");
      }
      streamPendingAction.power = true;
      streamPendingAction.temp  = manualOverrideTargetTemp;
      strncpy(streamPendingAction.source, "manual", sizeof(streamPendingAction.source) - 1);
      Serial.println("Stream: overrideActive=true — deferred apply queued.");
    }
    return;
  }

  if (path == "/targetTemp" && (type == "int" || type == "float" || type == "double")) {
    manualOverrideTargetTemp = normalizeACTemp((float)data.floatData());
    if (manualOverrideActive) {
      streamPendingAction.hasPending = true;
      streamPendingAction.power = true;
      streamPendingAction.temp  = manualOverrideTargetTemp;
      strncpy(streamPendingAction.source, "manual", sizeof(streamPendingAction.source) - 1);
    }
    return;
  }

  if (path == "/overrideUntil" && type == "string") {
    manualOverrideUntil = data.stringData();
    return;
  }

  if (path == "/power" && type == "boolean") {
    manualOverridePower = data.boolData();
    if (manualOverrideActive) {
      streamPendingAction.hasPending = true;
      streamPendingAction.power = manualOverridePower;
      streamPendingAction.temp  = manualOverrideTargetTemp;
      strncpy(streamPendingAction.source, "manual", sizeof(streamPendingAction.source) - 1);
    }
    return;
  }
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) {
    streamAttached = false;
    Firebase.RTDB.endStream(&streamFbdo);
    lastStreamRetryMillis = millis();
    Serial.println("Firebase stream timeout, retry pending.");
  }
}

void ensureControlStream() {
  if (!firebaseInitialized || WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
  if (netAuthState != NA_READY) return;
  if (streamAttached) return;

  unsigned long now = millis();
  if ((now - lastStreamRetryMillis)  < 5000) return;
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

  Firebase.RTDB.setStreamCallback(&streamFbdo, streamCallback, streamTimeoutCallback);
  streamAttached = true;
}

void reconnectWiFiNonBlocking() {
  wl_status_t status = WiFi.status();

  if (status == WL_CONNECTED) {
    if (!wifiLinkUp) {
      wifiLinkUp = true;
      lastWiFiConnectedMillis = millis();
      streamAttached      = false;
      startupStateLoaded  = false;
      firebaseInitialized = false;
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
  streamAttached      = false;
  firebaseInitialized = false;
  startupStateLoaded  = false;
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
    fbdo.setBSSLBufferSize(4096, 1024);
    streamFbdo.setBSSLBufferSize(4096, 1024);
    config.timeout.socketConnection = 10000;

    Firebase.begin(&config, &auth);
    Firebase.reconnectNetwork(true);
    firebaseInitialized = true;
    streamAttached      = false;
    startupStateLoaded  = false;
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
  Firebase.RTDB.setBool(&fbdo, base + "/overrideActive", false);

  String acBase = "/devices/" + String(DEVICE_ID) + "/acState";
  Firebase.RTDB.setString(&fbdo, acBase + "/source", "override_expired");
}

#endif