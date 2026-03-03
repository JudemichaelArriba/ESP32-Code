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

bool isFirebaseTokenPendingError(const String& err) {
  return err.indexOf("token is not ready") >= 0;
}

bool isFirebaseRevokedError(const String& err) {
  // "token is not ready (revoked or expired)" appears during token mint;
  // treat that as pending unless the explicit pending phrase is absent.
  return err.indexOf("revoked or expired") >= 0 && !isFirebaseTokenPendingError(err);
}

bool isFirebaseAuthOrSslError(const String& err) {
  return isFirebaseRevokedError(err) ||
         err.indexOf("ssl") >= 0 ||
         err.indexOf("SSL") >= 0;
}

void requestFirebaseReinit(const String& reason) {
  unsigned long now = millis();
  if ((now - lastStreamRetryMillis) < 5000) return;

  lastStreamRetryMillis = now;
  Serial.println("Firebase reinit requested: " + reason);
  streamAttached = false;
  firebaseInitialized = false;
  startupStateLoaded = false;
  Firebase.RTDB.endStream(&streamFbdo);
}

// Implementation
void setControlStateToFirebase(bool active) {
  String controlPath = "/devices/" + String(DEVICE_ID) + "/control/overrideActive";
  Firebase.RTDB.setBool(&fbdo, controlPath, active);
}

bool fetchAssignedRoomFromFirebase() {
  RoomConfig fetchedRoom;

  if (!Firebase.ready()) {
    return false;
  }

  if (!Firebase.RTDB.getJSON(&fbdo, "/rooms")) {
    String err = fbdo.errorReason();
    Serial.println("Failed to read /rooms: " + err);
    if (isFirebaseTokenPendingError(err)) {
      // Token generation is async; avoid reinit loops while waiting.
      return false;
    }
    if (isFirebaseAuthOrSslError(err)) {
      requestFirebaseReinit(err);
    }
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

    fetchedRoom.found = true;
    fetchedRoom.uid = String(kv.key().c_str());
    fetchedRoom.roomName = String((const char*)(room["roomName"] | ""));
    fetchedRoom.device = String(device);

    JsonArray schedules = room["schedules"].as<JsonArray>();
    if (!schedules.isNull()) {
      for (JsonObject item : schedules) {
        if (fetchedRoom.scheduleCount >= 16) break;

        String day = String((const char*)(item["day"] | ""));
        String startTime = String((const char*)(item["startTime"] | ""));
        String endTime = String((const char*)(item["endTime"] | ""));

        int startMin = parseTimeToMinute(startTime);
        int endMin = parseTimeToMinute(endTime);
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
  Firebase.RTDB.setBool(&fbdo, basePath + "/power", acPowerState);
  Firebase.RTDB.setInt(&fbdo, basePath + "/currentTemp", acTempState);
  Firebase.RTDB.setString(&fbdo, basePath + "/source", acSourceState);
  Firebase.RTDB.setString(&fbdo, basePath + "/updatedAt", nowIsoString());
  if (assignedRoom.found) {
    Firebase.RTDB.setString(&fbdo, basePath + "/roomUid", assignedRoom.uid);
  }
}

void loadAcStateFromFirebase() {
  String path = "/devices/" + String(DEVICE_ID) + "/acState";
  if (!Firebase.RTDB.getJSON(&fbdo, path)) return;

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, fbdo.jsonString()) != DeserializationError::Ok) return;

  if (!doc["power"].isNull()) acPowerState = doc["power"].as<bool>();
  if (!doc["currentTemp"].isNull()) acTempState = normalizeACTemp((float)doc["currentTemp"].as<int>());
}

void applyControlJson(JsonVariant data);  // Forward declaration for use in streamCallback

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

void streamCallback(FirebaseStream data);  // Forward declaration
bool applyAcState(bool targetPower, int targetTemp, const String& source);  // Forward declaration

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
    Firebase.RTDB.endStream(&streamFbdo);
    Serial.println("Firebase stream timeout, retry pending.");
  }
}

void ensureControlStream() {
  if (!firebaseInitialized || WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
  if (streamAttached) return;

  unsigned long now = millis();
  if ((now - lastStreamRetryMillis) < 2000) return;

  String streamPath = "/devices/" + String(DEVICE_ID) + "/control";
  if (!Firebase.RTDB.beginStream(&streamFbdo, streamPath)) {
    String err = streamFbdo.errorReason();
    Serial.println("Control stream begin failed: " + err);
    lastStreamRetryMillis = now;
    if (isFirebaseTokenPendingError(err)) {
      return;
    }
    if (isFirebaseAuthOrSslError(err)) {
      requestFirebaseReinit(err);
    }
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
  struct tm t;
  if (!timeIsValid(t)) return;

  Firebase.begin(&config, &auth);
  Firebase.reconnectNetwork(true);
  firebaseInitialized = true;
  streamAttached = false;
  startupStateLoaded = false;
  Serial.println("Firebase initialized.");
}

#endif
