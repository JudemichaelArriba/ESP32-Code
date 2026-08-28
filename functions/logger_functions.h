// logger_functions.h
#ifndef LOGGER_FUNCTIONS_H
#define LOGGER_FUNCTIONS_H

#include "../core/structures.h"
#include "utility_functions.h"
#include "persistence_functions.h"

static const unsigned long DECISION_LOG_FAILURE_COOLDOWN_MS = 5UL * 60UL * 1000UL;

static bool canWriteDecisionLogToFirebase() {
  return firebaseInitialized && WiFi.status() == WL_CONNECTED && Firebase.ready();
}

static String loggerRoomUid() {
  return assignedRoom.found ? assignedRoom.uid : String("");
}

static void addCommonDecisionFields(FirebaseJson& json, const String& eventType) {
  json.set("deviceId", String(DEVICE_ID));
  json.set("roomUid", loggerRoomUid());
  json.set("eventType", eventType);
  json.set("mode", lastScheduleMode);
  json.set("updatedAt", nowIsoString());
  json.set("uptimeMs", (double)millis());
}

static bool pushDecisionLog(FirebaseJson& json) {
  if (!canWriteDecisionLogToFirebase()) return false;

  markRuntimeOperation("firebase_decision_log");
  bool written = Firebase.RTDB.pushJSON(&fbdo, "/decisionLogs", &json);
  clearRuntimeOperation();
  if (written) {
    return true;
  }

  Serial.println("DecisionLog: write failed (" + fbdo.errorReason() + ")");
  return false;
}

void logDecisionEvent(const String& eventType,
                      const String& source,
                      bool power,
                      int targetTemp,
                      int suggestedTemp,
                      bool aiAutoApply,
                      bool applied,
                      const String& reason) {
  FirebaseJson json;
  addCommonDecisionFields(json, eventType);
  json.set("source", source);
  json.set("power", power);
  json.set("targetTemp", normalizeACTemp((float)targetTemp));
  if (suggestedTemp >= AC_TEMP_MIN && suggestedTemp <= AC_TEMP_MAX) {
    json.set("suggestedTemp", suggestedTemp);
  }
  json.set("aiAutoApply", aiAutoApply);
  json.set("applied", applied);
  json.set("reason", reason);
  pushDecisionLog(json);
}

void logDecisionEventRateLimited(const String& eventType,
                                 const String& source,
                                 bool power,
                                 int targetTemp,
                                 int suggestedTemp,
                                 bool aiAutoApply,
                                 bool applied,
                                 const String& reason) {
  static String lastRateLimitedKey = "";
  static unsigned long lastRateLimitedLogMillis = 0;

  String key = eventType + "|" + reason;
  unsigned long now = millis();
  if (key == lastRateLimitedKey &&
      lastRateLimitedLogMillis != 0 &&
      (now - lastRateLimitedLogMillis) < DECISION_LOG_FAILURE_COOLDOWN_MS) {
    return;
  }

  lastRateLimitedKey = key;
  lastRateLimitedLogMillis = now;
  logDecisionEvent(eventType, source, power, targetTemp, suggestedTemp,
                   aiAutoApply, applied, reason);
}

bool writeMlSuggestion(int suggestedTemp, bool applied, const String& reason) {
  if (!canWriteDecisionLogToFirebase()) return false;

  FirebaseJson json;
  json.set("suggestedTemp", normalizeACTemp((float)suggestedTemp));
  json.set("currentRoomTemp", lastTemperature);
  json.set("humidity", lastHumidity);
  json.set("roomUid", loggerRoomUid());
  json.set("source", "ml");
  json.set("autoApplyEnabled", aiAutoApplyEnabled);
  json.set("applied", applied);
  json.set("reason", reason);
  json.set("updatedAt", nowIsoString());

  String path = "/devices/" + String(DEVICE_ID) + "/mlSuggestion";
  markRuntimeOperation("firebase_ml_log");
  bool written = Firebase.RTDB.setJSON(&fbdo, path, &json);
  clearRuntimeOperation();
  if (written) {
    return true;
  }

  Serial.println("ML suggestion: write failed (" + fbdo.errorReason() + ")");
  return false;
}

void logMlSuggestion(int suggestedTemp, bool applied, const String& reason) {
  writeMlSuggestion(suggestedTemp, applied, reason);
  logDecisionEvent("ml_suggestion", "ml", acPowerState,
                   applied ? suggestedTemp : acTempState,
                   suggestedTemp, aiAutoApplyEnabled, applied, reason);
}

void logMlFailure(const String& reason) {
  logDecisionEventRateLimited("ml_failure", "ml", acPowerState, acTempState,
                              -1, aiAutoApplyEnabled, false, reason);
}

void logAcStateChange(bool previousPower,
                      int previousTemp,
                      const String& previousSource,
                      bool newPower,
                      int newTemp,
                      const String& source,
                      bool irSent,
                      const String& reason) {
  FirebaseJson json;
  addCommonDecisionFields(json, "ac_state_changed");
  json.set("source", source);
  json.set("power", newPower);
  json.set("targetTemp", normalizeACTemp((float)newTemp));
  json.set("previousPower", previousPower);
  json.set("previousTemp", normalizeACTemp((float)previousTemp));
  json.set("previousSource", previousSource);
  json.set("irSent", irSent);
  json.set("aiAutoApply", aiAutoApplyEnabled);
  json.set("applied", true);
  json.set("reason", reason);
  pushDecisionLog(json);
}

#endif
