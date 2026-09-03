// firebase_functions.h
#ifndef FIREBASE_FUNCTIONS_H
#define FIREBASE_FUNCTIONS_H

#include "../core/structures.h"
#include "wifi_functions.h"
#include "utility_functions.h"
#include "persistence_functions.h"
#include "logger_functions.h"

void setControlStateToFirebase(bool active);
bool fetchAssignedRoomFromFirebase();
void syncAcStateToFirebase();
bool loadAcStateFromFirebase();
void applyControlJson(JsonVariant data);
bool loadControlStateFromFirebase();
void ensureControlStream();
void pollControlStream();
void reconnectWiFiNonBlocking();
void serviceNetworkTime();
void serviceNetworkRecovery();
void initFirebaseIfNeeded();
bool isFirebaseAuthOrSslError(const String& err);
bool isFirebaseTokenPendingError(const String& err);
bool isFirebaseRevokedError(const String& err);
void requestFirebaseReinit(const String& reason);
void noteFirebaseDataSuccess();
void noteFirebaseDataFailure(const String& operation, const String& error, int httpCode = 0);
void noteFirebaseHeartbeatSuccess();
void clearForcedOffPersistedInFirebase();
ScheduleStatus evaluateScheduleStatus(const struct tm& t);

static const uint8_t NA_WAIT_WIFI   = 0;
static const uint8_t NA_WAIT_STABLE = 1;
static const uint8_t NA_AUTH_INIT   = 2;
static const uint8_t NA_AUTH_WAIT   = 3;
static const uint8_t NA_READY       = 4;

// Global timer to debounce our own HTTP writes from the Realtime Stream
unsigned long lastEspControlWriteMillis = 0;

static uint8_t firebaseDataFailureCount = 0;
static uint8_t firebaseStreamFailureCount = 0;
static unsigned long firebaseRetryNotBeforeMillis = 0;
static unsigned long firebaseReinitBackoffMillis = FIREBASE_REINIT_BACKOFF_MIN_MS;
static unsigned long lastForcedWiFiRecoveryMillis = 0;
static bool tokenDiagnosticPending = false;
static void setNetAuthState(uint8_t s);

static const char* firebaseTokenStatusName(firebase_auth_token_status status) {
  switch (status) {
    case token_status_uninitialized: return "uninitialized";
    case token_status_on_initialize: return "initializing";
    case token_status_on_signing: return "signing";
    case token_status_on_request: return "requesting";
    case token_status_on_refresh: return "refreshing";
    case token_status_ready: return "ready";
    case token_status_error: return "error";
    default: return "unknown";
  }
}

void firebaseTokenStatusCallback(TokenInfo info) {
  const String status = firebaseTokenStatusName(info.status);
  const bool statusChanged = lastFirebaseTokenStatus != status;
  lastFirebaseTokenStatus = status;
  lastFirebaseTokenErrorCode = info.error.code;
  lastFirebaseTokenError = info.error.message.c_str();

  if (statusChanged || info.status == token_status_error) {
    Serial.printf("Firebase token: %s", status.c_str());
    if (info.status == token_status_error) {
      Serial.printf(" (code %d: %s)", info.error.code,
                    info.error.message.c_str());
      tokenDiagnosticPending = true;
    }
    Serial.println();
  }
}

static bool deadlineReached(unsigned long now, unsigned long deadline) {
  return deadline == 0 || (long)(now - deadline) >= 0;
}

static void noteNetworkOutageStarted() {
  if (networkOutageSinceMillis == 0) {
    const unsigned long now = millis();
    networkOutageSinceMillis = now == 0 ? 1 : now;
    // Reboot-durable marker: proves the outage window even if a power cycle
    // discards the RAM-buffered decision logs recorded inside it.
    recordPersistentDiagnostic("network_outage_start", "firebase or ntp unavailable");
  }
}

static unsigned long firebaseRecoveryJitterMs() {
  if (RECOVERY_JITTER_MAX_MS == 0) return 0;
  const uint64_t mac = ESP.getEfuseMac();
  return (unsigned long)(mac % (RECOVERY_JITTER_MAX_MS + 1));
}

static void clearFirebaseSessionObjects() {
  if (streamAttached) {
    Firebase.RTDB.endStream(&streamFbdo);
  }
  streamAttached = false;
  streamFbdo.clear();
  fbdo.clear();
  Firebase.reset(&config);
}

static void forceWiFiReconnectForRecovery(const String& reason) {
  const unsigned long now = millis();
  if (lastForcedWiFiRecoveryMillis != 0 &&
      (now - lastForcedWiFiRecoveryMillis) < NETWORK_RECOVERY_COOLDOWN_MS) {
    return;
  }

  lastForcedWiFiRecoveryMillis = now == 0 ? 1 : now;
  noteNetworkOutageStarted();
  Serial.println("Network recovery: forcing WiFi reconnect (" + reason + ").");
  recordPersistentDiagnostic("wifi_recovery", reason);
  clearFirebaseSessionObjects();
  firebaseInitialized = false;
  startupStateLoaded = false;
  wifiLinkUp = false;
  setNetAuthState(NA_WAIT_WIFI);
  WiFi.disconnect(false, false);  // Preserve provisioned credentials.
  lastWiFiReconnectAttempt = 0;
}

static void servicePendingTokenDiagnostic() {
  if (!tokenDiagnosticPending) return;
  tokenDiagnosticPending = false;
  recordPersistentDiagnostic(
      "firebase_token",
      String(lastFirebaseTokenErrorCode) + ":" + lastFirebaseTokenError);
}

static void setNetAuthState(uint8_t s) {
  if (netAuthState == s) return;
  netAuthState      = s;
  netAuthStateSince = millis();
}

static bool resolveManualOverridePower(JsonVariant data, const bool overrideActive) {
  if (!data["power"].isNull()) return data["power"].as<bool>();
  return overrideActive;
}

static void setAiAutoApplyEnabled(bool enabled, const String& reason) {
  bool changed = aiAutoApplyEnabled != enabled;
  aiAutoApplyEnabled = enabled;
  if (changed) {
    logDecisionEvent("ai_toggle_changed", "control", acPowerState, acTempState,
                     -1, aiAutoApplyEnabled, false, reason);
  }
}

static void loadAiAutoApplyFromControl(JsonVariant data,
                                       const String& reason,
                                       bool missingDefaultsFalse) {
  if (data["aiAutoApply"].isNull()) {
    if (missingDefaultsFalse) {
      setAiAutoApplyEnabled(false, reason + "_missing_default_false");
    }
    return;
  }

  setAiAutoApplyEnabled(data["aiAutoApply"].as<bool>(), reason);
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
  persistManualOverrideState();

  queueAcStreamAction(false, acTempState, "forced_off");
  queueForcedOffPersistence(forcedOffActive, forcedOffWindowKey);
  streamPendingAction.writeForcedOffFalse = true;
  Serial.println(logPrefix + " forcedOff=true queued for window key: " + forcedOffWindowKey);
  logDecisionEvent("forced_off_trigger", "forced_off", false, acTempState,
                   -1, aiAutoApplyEnabled, true,
                   forcedOffActive ? "window_locked" : "no_active_window");
}

static bool updateControlPatch(FirebaseJson& patch) {
  String base = "/devices/" + String(DEVICE_ID) + "/control";
  lastEspControlWriteMillis = millis();

  markRuntimeOperation("firebase_control_patch");
  bool updated = Firebase.RTDB.updateNode(&fbdo, base, &patch);
  clearRuntimeOperation();
  if (updated) {
    noteFirebaseDataSuccess();
    return true;
  }

  String err = fbdo.errorReason();
  Serial.println("Control patch failed: " + err);
  noteFirebaseDataFailure("control_patch", err, fbdo.httpCode());
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
  String normalized = err;
  normalized.toLowerCase();
  return isFirebaseRevokedError(normalized) ||
         normalized.indexOf("ssl") >= 0 ||
         normalized.indexOf("handshake") >= 0 ||
         normalized.indexOf("not connected") >= 0 ||
         normalized.indexOf("connection") >= 0 ||
         normalized.indexOf("tcp") >= 0 ||
         normalized.indexOf("closed") >= 0 ||
         normalized.indexOf("timeout") >= 0 ||
         normalized.indexOf("timed out") >= 0 ||
         normalized.indexOf("unauthorized") >= 0 ||
         normalized.indexOf("permission denied") >= 0;
}

static bool isFirebaseCredentialError(const String& err, int httpCode) {
  String normalized = err;
  normalized.toLowerCase();
  return httpCode == 401 || httpCode == 403 ||
         isFirebaseRevokedError(normalized) ||
         normalized.indexOf("unauthorized") >= 0 ||
         normalized.indexOf("permission denied") >= 0;
}

void requestFirebaseReinit(const String& reason) {
  unsigned long now = millis();
  if (!firebaseInitialized && netAuthState == NA_WAIT_STABLE &&
      !deadlineReached(now, firebaseRetryNotBeforeMillis)) return;

  firebaseRecoveryCount++;
  if (firebaseSessionRecoveryStreak < 255) firebaseSessionRecoveryStreak++;
  noteNetworkOutageStarted();
  Serial.println("Firebase reinit requested: " + reason);
  recordPersistentDiagnostic("firebase_reinit", reason);
  clearFirebaseSessionObjects();
  firebaseInitialized = false;
  startupStateLoaded  = false;
  firebaseUnavailableSinceMillis = 0;
  lastStreamRetryMillis = now;
  lastWiFiConnectedMillis = now;
  firebaseRetryNotBeforeMillis =
      now + firebaseReinitBackoffMillis + firebaseRecoveryJitterMs();
  if (firebaseReinitBackoffMillis < FIREBASE_REINIT_BACKOFF_MAX_MS) {
    firebaseReinitBackoffMillis *= 2;
    if (firebaseReinitBackoffMillis > FIREBASE_REINIT_BACKOFF_MAX_MS) {
      firebaseReinitBackoffMillis = FIREBASE_REINIT_BACKOFF_MAX_MS;
    }
  }
  setNetAuthState(NA_WAIT_STABLE);

  if (firebaseSessionRecoveryStreak >=
      FIREBASE_RECOVERIES_BEFORE_WIFI_RECOVERY) {
    forceWiFiReconnectForRecovery("repeated Firebase recovery failures");
  }
}

void noteFirebaseDataSuccess() {
  firebaseDataFailureCount = 0;
}

void noteFirebaseDataFailure(const String& operation, const String& error, int httpCode) {
  if (isFirebaseTokenPendingError(error)) return;
  noteNetworkOutageStarted();
  if (firebaseDataFailureCount < 255) firebaseDataFailureCount++;

  Serial.printf("Firebase %s failure %u/%u (HTTP %d): %s\n",
                operation.c_str(), firebaseDataFailureCount,
                FIREBASE_FAILURES_BEFORE_REINIT, httpCode, error.c_str());

  const bool fatal = isFirebaseCredentialError(error, httpCode);
  if (fatal || firebaseDataFailureCount >= FIREBASE_FAILURES_BEFORE_REINIT) {
    requestFirebaseReinit(operation + ": " + error);
  }
}

static void noteFirebaseStreamFailure(const String& operation,
                                      const String& error,
                                      int httpCode) {
  if (isFirebaseTokenPendingError(error)) return;
  noteNetworkOutageStarted();

  streamAttached = false;
  Firebase.RTDB.endStream(&streamFbdo);
  lastStreamRetryMillis = millis();
  if (firebaseStreamFailureCount < 255) firebaseStreamFailureCount++;

  Serial.printf("Firebase %s failure %u/%u (HTTP %d): %s\n",
                operation.c_str(), firebaseStreamFailureCount,
                FIREBASE_FAILURES_BEFORE_REINIT, httpCode, error.c_str());

  if (isFirebaseCredentialError(error, httpCode) ||
      firebaseStreamFailureCount >= FIREBASE_FAILURES_BEFORE_REINIT) {
    requestFirebaseReinit(operation + ": " + error);
  }
}

void setControlStateToFirebase(bool active) {
  String controlPath = "/devices/" + String(DEVICE_ID) + "/control/overrideActive";
  lastEspControlWriteMillis = millis();
  markRuntimeOperation("firebase_control_write");
  bool written = Firebase.RTDB.setBool(&fbdo, controlPath, active);
  clearRuntimeOperation();
  if (written) {
    noteFirebaseDataSuccess();
  } else {
    noteFirebaseDataFailure("control_write", fbdo.errorReason(), fbdo.httpCode());
  }
}

void clearForcedOffPersistedInFirebase() {
  String previousWindowKey = forcedOffWindowKey;
  clearForcedOffLocal();
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
  FirebaseJson patch;
  patch.set("forcedOffPersisted", false);
  patch.set("forcedOffWindowKey", "");
  updateControlPatch(patch);
  Serial.println("forcedOff persistence cleared in Firebase.");
  logDecisionEvent("forced_off_cleared", "forced_off", acPowerState, acTempState,
                   -1, aiAutoApplyEnabled, false,
                   previousWindowKey.length() > 0 ? "stale_window_cleared" : "cleared");
}

bool fetchAssignedRoomFromFirebase() {
  RoomConfig fetchedRoom;

  if (netAuthState != NA_READY) return false;
  if (!Firebase.ready()) return false;

  unsigned long now = millis();
  if ((now - lastFirebaseInitMillis)      < FIREBASE_AUTH_SETTLE_MS) return false;
  if ((now - lastRoomsFetchAttemptMillis) < ROOMS_FETCH_RETRY_MS)    return false;
  lastRoomsFetchAttemptMillis = now;

  markRuntimeOperation("firebase_rooms_read");
  bool roomsRead = Firebase.RTDB.getJSON(&fbdo, "/rooms");
  clearRuntimeOperation();
  if (!roomsRead) {
    String err = fbdo.errorReason();
    Serial.println("Failed to read /rooms: " + err);
    noteFirebaseDataFailure("rooms_read", err, fbdo.httpCode());
    return false;
  }
  noteFirebaseDataSuccess();

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

  markRuntimeOperation("firebase_ac_write");
  bool written = Firebase.RTDB.setJSON(&fbdo, basePath, &json);
  clearRuntimeOperation();
  if (!written) {
    String err = fbdo.errorReason();
    Serial.println("Failed to sync acState: " + err);
    noteFirebaseDataFailure("ac_state_write", err, fbdo.httpCode());
  } else {
    noteFirebaseDataSuccess();
  }
}

bool loadAcStateFromFirebase() {
  String path = "/devices/" + String(DEVICE_ID) + "/acState";
  markRuntimeOperation("firebase_ac_read");
  bool read = Firebase.RTDB.getJSON(&fbdo, path);
  clearRuntimeOperation();
  if (!read) {
    String err = fbdo.errorReason();
    Serial.println("Failed to load acState: " + err);
    noteFirebaseDataFailure("ac_state_read", err, fbdo.httpCode());
    return false;
  }

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, fbdo.jsonString()) != DeserializationError::Ok) {
    Serial.println("Failed to parse acState JSON.");
    return false;
  }

  if (!doc["power"].isNull())       acPowerState = doc["power"].as<bool>();
  if (!doc["currentTemp"].isNull()) acTempState  = normalizeACTemp((float)doc["currentTemp"].as<int>());
  acIrStateTrusted = false;
  noteFirebaseDataSuccess();
  return true;
}

bool loadControlStateFromFirebase() {
  String path = "/devices/" + String(DEVICE_ID) + "/control";
  markRuntimeOperation("firebase_control_read");
  bool read = Firebase.RTDB.getJSON(&fbdo, path);
  clearRuntimeOperation();
  if (!read) {
    String err = fbdo.errorReason();
    Serial.println("Failed to load control state: " + err);
    noteFirebaseDataFailure("control_state_read", err, fbdo.httpCode());
    return false;
  }

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, fbdo.jsonString()) != DeserializationError::Ok) {
    Serial.println("Failed to parse control state JSON.");
    return false;
  }
  JsonVariant control = doc.as<JsonVariant>();

  loadAiAutoApplyFromControl(control, "startup_load", true);

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
  manualOverrideTemp = manualOverrideTargetTemp;

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
    persistManualOverrideState();

    FirebaseJson patch;
    patch.set("forcedOffPersisted", forcedOffActive);
    patch.set("forcedOffWindowKey", forcedOffWindowKey);
    patch.set("forcedOff", false);
    patch.set("overrideActive", false);
    updateControlPatch(patch);
    Serial.println("Startup: forcedOff=true processed for window key: " + forcedOffWindowKey);
    logDecisionEvent("forced_off_trigger", "forced_off", false, acTempState,
                     -1, aiAutoApplyEnabled, true,
                     forcedOffActive ? "startup_window_locked" : "startup_no_active_window");
    noteFirebaseDataSuccess();
    return true;
  }

  if (hasForcedOffPersisted) {
    String currentKey = currentForcedOffWindowKey();
    if (persistedWindowKey.length() > 0 && persistedWindowKey == currentKey) {
      forcedOffActive = true;
      forcedOffWindowKey = persistedWindowKey;
      manualOverrideActive = false;
      manualOverrideUntil = "";
      Serial.println("Startup: forcedOff persisted for current window.");
      logDecisionEvent("forced_off_trigger", "forced_off", false, acTempState,
                       -1, aiAutoApplyEnabled, true, "startup_persisted");
    } else {
      clearForcedOffPersistedInFirebase();
      Serial.println("Startup: stale forcedOff persistence cleared.");
    }
    persistManualOverrideState();
    noteFirebaseDataSuccess();
    return true;
  }

  if (persistedWindowKey.length() > 0) {
    clearForcedOffPersistedInFirebase();
  }
  persistManualOverrideState();
  noteFirebaseDataSuccess();
  return true;
}

static void handleControlJsonSnapshot(const String& payload) {
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, payload) != DeserializationError::Ok) return;
  JsonVariant v = doc.as<JsonVariant>();

  loadAiAutoApplyFromControl(v, "stream_json", false);

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
  manualOverrideTemp = manualOverrideTargetTemp;

  if (manualOverrideActive) {
    if (forcedOffActive) {
      clearForcedOffLocal();
      queueForcedOffPersistence(false, "");
      Serial.println("Stream (json): manual override clearing forcedOff.");
    }
    queueAcStreamAction(manualOverridePower, manualOverrideTargetTemp, "manual");
    Serial.println("Stream (json): override ON queued.");
    logDecisionEvent("manual_override", "manual", manualOverridePower,
                     manualOverrideTargetTemp, -1, aiAutoApplyEnabled,
                     true, previousOverrideActive ? "stream_json_updated" : "stream_json_on");

  } else if (previousOverrideActive) {
    queueAcStreamAction(false, acTempState, "override_cleared");
    refreshScheduleStatusForForcedOff();
    setForcedOffForCurrentWindow();
    queueForcedOffPersistence(forcedOffActive, forcedOffWindowKey);
    Serial.println("Stream (json): manual override off queued forcedOff key: " + forcedOffWindowKey);
    logDecisionEvent("manual_override", "manual", false, acTempState,
                     -1, aiAutoApplyEnabled, false, "stream_json_off");
  }
  persistManualOverrideState();
}

static void handleControlStreamEvent(const String& path, const String& type) {
  Serial.println("Stream update: " + path + " [" + type + "]");

  streamPendingAction = StreamPendingAction();

  if (type == "json") {
    handleControlJsonSnapshot(streamFbdo.jsonString());
    return;
  }

  if (path == "/aiAutoApply" && type == "boolean") {
    setAiAutoApplyEnabled(streamFbdo.boolData(), "stream_boolean");
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
      logDecisionEvent("manual_override", "manual", manualOverridePower,
                       manualOverrideTargetTemp, -1, aiAutoApplyEnabled,
                       true, previousOverrideActive ? "stream_updated" : "stream_on");

    } else if (previousOverrideActive) {
      manualOverrideUntil = "";
      queueAcStreamAction(false, acTempState, "override_cleared");
      refreshScheduleStatusForForcedOff();
      setForcedOffForCurrentWindow();
      queueForcedOffPersistence(forcedOffActive, forcedOffWindowKey);
      Serial.println("Stream: manual override off queued forcedOff key: " + forcedOffWindowKey);
      logDecisionEvent("manual_override", "manual", false, acTempState,
                       -1, aiAutoApplyEnabled, false, "stream_off");

    }
    persistManualOverrideState();
    return;
  }

  if ((path == "/targetTemp" || path == "/temp") &&
      (type == "int" || type == "float" || type == "double")) {
    manualOverrideTargetTemp = normalizeACTemp((float)streamFbdo.floatData());
    manualOverrideTemp = manualOverrideTargetTemp;
    if (manualOverrideActive) {
      if (!manualOverridePower) manualOverridePower = true;
      queueAcStreamAction(manualOverridePower, manualOverrideTargetTemp, "manual");
      logDecisionEvent("manual_override", "manual", manualOverridePower,
                       manualOverrideTargetTemp, -1, aiAutoApplyEnabled,
                       true, "stream_temp_changed");
    }
    persistManualOverrideState();
    return;
  }

  if (path == "/overrideUntil" && type == "string") {
    manualOverrideUntil = streamFbdo.stringData();
    persistManualOverrideState();
    return;
  }

  if (path == "/power" && type == "boolean") {
    manualOverridePower = streamFbdo.boolData();
    if (manualOverrideActive) {
      queueAcStreamAction(manualOverridePower, manualOverrideTargetTemp, "manual");
      logDecisionEvent("manual_override", "manual", manualOverridePower,
                       manualOverrideTargetTemp, -1, aiAutoApplyEnabled,
                       true, "stream_power_changed");
    }
    persistManualOverrideState();
    return;
  }
}

void ensureControlStream() {
  if (!firebaseInitialized || WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
  if (netAuthState != NA_READY) return;
  if (streamAttached) return;

  unsigned long now = millis();
  if ((now - lastStreamRetryMillis) < FIREBASE_STREAM_RETRY_MS) return;
  if ((now - lastFirebaseInitMillis) < FIREBASE_AUTH_SETTLE_MS) return;

  String streamPath = "/devices/" + String(DEVICE_ID) + "/control";
  markRuntimeOperation("firebase_stream_begin");
  bool began = Firebase.RTDB.beginStream(&streamFbdo, streamPath);
  clearRuntimeOperation();
  if (!began) {
    String err = streamFbdo.errorReason();
    Serial.println("Control stream begin failed: " + err);
    noteFirebaseStreamFailure("stream_begin", err, streamFbdo.httpCode());
    return;
  }

  streamAttached = true;
}

void pollControlStream() {
  if (!streamAttached || !firebaseInitialized || WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
  if (netAuthState != NA_READY) return;

  markRuntimeOperation("firebase_stream_read");
  bool streamRead = Firebase.RTDB.readStream(&streamFbdo);
  clearRuntimeOperation();
  if (!streamRead) {
    String err = streamFbdo.errorReason();
    if (err.length() > 0) {
      Serial.println("Control stream read failed: " + err);
    }
    if (isFirebaseTokenPendingError(err)) return;

    noteFirebaseStreamFailure("stream_read", err, streamFbdo.httpCode());
    return;
  }

  firebaseStreamFailureCount = 0;

  if (streamFbdo.streamTimeout()) {
    Serial.println("Firebase stream timeout, auto-resuming...");
    if (!streamFbdo.httpConnected()) {
      String err = streamFbdo.errorReason();
      noteFirebaseStreamFailure("stream_timeout", err, streamFbdo.httpCode());
      return;
    }
  }

  if (!streamFbdo.streamAvailable()) return;

  handleControlStreamEvent(streamFbdo.dataPath(), streamFbdo.dataType());
}

void reconnectWiFiNonBlocking() {
  wl_status_t status = WiFi.status();

  if (status == WL_CONNECTED) {
    resetWiFiReconnectFailures();

    if (!wifiLinkUp) {
      wifiLinkUp              = true;
      lastWiFiConnectedMillis = millis();
      streamAttached          = false;
      startupStateLoaded      = false;
      firebaseInitialized     = false;
      setNetAuthState(NA_WAIT_STABLE);
      configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
      lastNtpSyncMillis = millis();

      const bool restoredConnection = wifiHasConnectedOnce;
      wifiHasConnectedOnce = true;
      if (restoredConnection) {
        Serial.println("WiFi restored; rebuilding Firebase session without rebooting.");
      }
    }
    return;
  }

  wifiLinkUp = false;
  setNetAuthState(NA_WAIT_WIFI);

  unsigned long now = millis();
  if (now - lastWiFiReconnectAttempt < WIFI_RECONNECT_MS) return;

  lastWiFiReconnectAttempt = now;
  streamAttached           = false;
  firebaseInitialized      = false;
  startupStateLoaded       = false;
  Serial.println("WiFi disconnected, reconnecting...");
  WiFi.reconnect();
  noteWiFiReconnectFailure();
}

void serviceNetworkTime() {
  servicePendingTokenDiagnostic();
  if (WiFi.status() != WL_CONNECTED) {
    // A lost network does not invalidate an already-synchronized local clock.
    return;
  }

  const unsigned long now = millis();
  if (lastNtpCheckMillis != 0 &&
      (now - lastNtpCheckMillis) < NTP_VALID_CHECK_MS) {
    return;
  }
  lastNtpCheckMillis = now;

  struct tm currentTime;
  if (timeIsValid(currentTime)) {
    if (!ntpTimeValid) Serial.println("NTP: system time is valid.");
    ntpTimeValid = true;
    consecutiveNtpFailures = 0;
    lastNtpValidMillis = now;

    if (lastNtpSyncMillis == 0 ||
        (now - lastNtpSyncMillis) >= NTP_RECONFIG_INTERVAL_MS) {
      configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
      lastNtpSyncMillis = now;
      Serial.println("NTP: periodic resync requested.");
    }
    return;
  }

  ntpTimeValid = false;
  noteNetworkOutageStarted();
  if (lastNtpSyncMillis != 0 &&
      (now - lastNtpSyncMillis) < NTP_RETRY_MS) {
    return;
  }

  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  lastNtpSyncMillis = now;
  if (consecutiveNtpFailures < 255) consecutiveNtpFailures++;
  Serial.printf("NTP: time unavailable, retry %u/%u.\n",
                consecutiveNtpFailures,
                NTP_FAILURES_BEFORE_WIFI_RECOVERY);

  if (consecutiveNtpFailures == NTP_FAILURES_BEFORE_WIFI_RECOVERY) {
    recordPersistentDiagnostic("ntp_unavailable", "time synchronization failed");
  }
  if (consecutiveNtpFailures >= NTP_FAILURES_BEFORE_WIFI_RECOVERY) {
    forceWiFiReconnectForRecovery("NTP unavailable");
    consecutiveNtpFailures = 0;
  }
}

void serviceNetworkRecovery() {
  servicePendingTokenDiagnostic();
  if (networkOutageSinceMillis == 0) return;

  const unsigned long now = millis();
  if ((now - networkOutageSinceMillis) < NETWORK_OUTAGE_RESTART_MS) return;

  Serial.println("Network recovery: outage limit reached, restarting ESP safely.");
  persistManualOverrideState();
  recordPersistentDiagnostic("network_restart", "outage limit reached");
  delay(100);
  ESP.restart();
}

void noteFirebaseHeartbeatSuccess() {
  lastHeartbeatSuccessMillis = millis();
  consecutiveHeartbeatFailures = 0;
  networkOutageSinceMillis = 0;
  firebaseSessionRecoveryStreak = 0;
}

void initFirebaseIfNeeded() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!wifiLinkUp) return;

  // serviceNetworkTime() performs the bounded 30-second validity check. Avoid
  // calling getLocalTime() on every loop while a boot-time NTP request is down.
  if (!ntpTimeValid) return;

  unsigned long now = millis();

  if (!deadlineReached(now, firebaseRetryNotBeforeMillis)) return;
  firebaseRetryNotBeforeMillis = 0;

  if (netAuthState == NA_WAIT_STABLE) {
    if ((now - lastWiFiConnectedMillis) < WIFI_STABLE_BEFORE_FB_MS) return;
    setNetAuthState(NA_AUTH_INIT);
  }

  if (netAuthState == NA_AUTH_INIT) {
    // Bump BSSL buffer slightly to give the SSL engine more breathing room
    fbdo.setBSSLBufferSize(8192, 2048);
    streamFbdo.setBSSLBufferSize(8192, 2048);
    config.timeout.socketConnection = FIREBASE_SOCKET_TIMEOUT_MS;
    config.timeout.sslHandshake = FIREBASE_SSL_HANDSHAKE_TIMEOUT_MS;
    config.timeout.serverResponse = FIREBASE_SERVER_RESPONSE_TIMEOUT_MS;
    config.timeout.rtdbKeepAlive = FIREBASE_STREAM_KEEPALIVE_MS;
    config.timeout.rtdbStreamReconnect = FIREBASE_STREAM_RECONNECT_MS;
    config.timeout.rtdbStreamError = FIREBASE_STREAM_ERROR_MS;

    markRuntimeOperation("firebase_auth");
    Firebase.begin(&config, &auth);
    Firebase.reconnectNetwork(true);
    clearRuntimeOperation();
    firebaseInitialized    = true;
    streamAttached         = false;
    startupStateLoaded     = false;
    lastFirebaseInitMillis = millis();
    Serial.println("Firebase initialized.");
    setNetAuthState(NA_AUTH_WAIT);
    return;
  }

  if (netAuthState == NA_AUTH_WAIT) {
    if (!Firebase.ready()) {
      if ((now - netAuthStateSince) >= FIREBASE_AUTH_TIMEOUT_MS) {
        requestFirebaseReinit("authentication timeout");
      }
      return;
    }
    if ((now - lastFirebaseInitMillis) < FIREBASE_AUTH_SETTLE_MS) return;
    setNetAuthState(NA_READY);
    lastFirebaseReadyMillis = now;
    firebaseUnavailableSinceMillis = 0;
    firebaseDataFailureCount = 0;
    firebaseStreamFailureCount = 0;
    firebaseReinitBackoffMillis = FIREBASE_REINIT_BACKOFF_MIN_MS;
    logDecisionEvent("firebase_ready", "network", acPowerState, acTempState,
                     -1, aiAutoApplyEnabled, false, "auth_ready");
    return;
  }

  if (netAuthState == NA_READY) {
    if (Firebase.ready()) {
      lastFirebaseReadyMillis = now;
      if (firebaseUnavailableSinceMillis != 0) {
        Serial.println("Firebase readiness recovered before timeout.");
      }
      firebaseUnavailableSinceMillis = 0;
      return;
    }

    noteNetworkOutageStarted();
    if (firebaseUnavailableSinceMillis == 0) {
      firebaseUnavailableSinceMillis = now == 0 ? 1 : now;
      Serial.println("Firebase unavailable; supervising token/session recovery.");
      recordPersistentDiagnostic("firebase_unavailable", lastFirebaseTokenStatus);
      return;
    }

    if ((now - firebaseUnavailableSinceMillis) >=
        FIREBASE_READY_LOSS_TIMEOUT_MS) {
      requestFirebaseReinit("ready state unavailable beyond timeout");
    }
  }
}

void clearOverrideInFirebase() {
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
  String base = "/devices/" + String(DEVICE_ID) + "/control";
  lastEspControlWriteMillis = millis();
  markRuntimeOperation("firebase_override_clear");
  bool controlCleared = Firebase.RTDB.setBool(&fbdo, base + "/overrideActive", false);

  String acBase = "/devices/" + String(DEVICE_ID) + "/acState";
  bool sourceWritten = Firebase.RTDB.setString(&fbdo, acBase + "/source", "override_expired");
  clearRuntimeOperation();
  if (!controlCleared || !sourceWritten) {
    noteFirebaseDataFailure("override_clear", fbdo.errorReason(), fbdo.httpCode());
  } else {
    noteFirebaseDataSuccess();
  }
}

#endif
