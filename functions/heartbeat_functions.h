// heartbeat_functions.h
#ifndef HEARTBEAT_FUNCTIONS_H
#define HEARTBEAT_FUNCTIONS_H

#include "../core/structures.h"
#include "utility_functions.h"
#include "persistence_functions.h"
#include "firebase_functions.h"
#include <esp_system.h>


extern FirebaseData fbdo;

void tickHeartbeat();


static unsigned long lastHeartbeatMillis = 0;
static unsigned long lastHeartbeatAttemptMillis = 0;

void tickHeartbeat() {

  if (!firebaseInitialized) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (!Firebase.ready()) return;

  struct tm t;
  if (!timeIsValid(t)) return;


  unsigned long now = millis();
  if (lastHeartbeatMillis != 0 && (now - lastHeartbeatMillis) < HEARTBEAT_INTERVAL_MS) return;
  if (lastHeartbeatAttemptMillis != 0 &&
      (now - lastHeartbeatAttemptMillis) < HEARTBEAT_FAILURE_RETRY_MS) return;
  lastHeartbeatAttemptMillis = now;

  FirebaseJson json;
  json.set("lastSeen", nowIsoString());
  json.set("lastSeenServer/.sv", "timestamp");
  json.set("ip", WiFi.localIP().toString());
  json.set("firmwareVersion", FIRMWARE_VERSION);
  json.set("bootCount", (int)bootCount);
  json.set("resetReason", (int)esp_reset_reason());
  json.set("freeHeap", (int)ESP.getFreeHeap());
  json.set("minimumFreeHeap", (int)ESP.getMinFreeHeap());
  json.set("firebaseRecoveryCount", (int)firebaseRecoveryCount);
  json.set("firebaseRecoveryStreak", (int)firebaseSessionRecoveryStreak);
  json.set("heartbeatFailures", (int)consecutiveHeartbeatFailures);
  json.set("lastHeartbeatSuccessUptimeMs", (int)lastHeartbeatSuccessMillis);
  json.set("ntpTimeValid", ntpTimeValid);
  json.set("ntpFailures", (int)consecutiveNtpFailures);
  json.set("lastNtpValidUptimeMs", (int)lastNtpValidMillis);
  json.set("lastFirebaseReadyUptimeMs", (int)lastFirebaseReadyMillis);
  json.set("tokenStatus", lastFirebaseTokenStatus);
  if (lastFirebaseTokenErrorCode != 0) {
    json.set("tokenErrorCode", lastFirebaseTokenErrorCode);
  }
  json.set("mlxAvailable", mlxAvailable);
  appendPersistentDiagnosticsToJson(json);
  if (previousResetBreadcrumbAvailable) {
    json.set("previousResetOperation", String(previousResetOperation));
    json.set("previousResetUptimeMs", (int)previousResetUptimeMs);
    json.set("previousResetBootNumber", (int)previousResetBootNumber);
  }

  String path = "/devices/" + String(DEVICE_ID) + "/status";

  markRuntimeOperation("firebase_heartbeat");
  bool written = Firebase.RTDB.setJSON(&fbdo, path, &json);
  clearRuntimeOperation();
  if (written) {
    Serial.println("Heartbeat: OK");
    lastHeartbeatMillis = now;
    lastHeartbeatAttemptMillis = 0;
    noteFirebaseHeartbeatSuccess();
    noteFirebaseDataSuccess();
    clearUploadedPersistentDiagnostics();
    previousResetBreadcrumbAvailable = false;
  } else {
    String err = fbdo.errorReason();
    const int httpCode = fbdo.httpCode();
    Serial.println("Heartbeat: write failed (" + err + ")");
    if (consecutiveHeartbeatFailures < 255) consecutiveHeartbeatFailures++;
    if (consecutiveHeartbeatFailures == FIREBASE_FAILURES_BEFORE_REINIT) {
      recordPersistentDiagnostic("heartbeat_failed", err);
    }
    noteFirebaseDataFailure("heartbeat", err, httpCode);
    if (consecutiveHeartbeatFailures >= FIREBASE_FAILURES_BEFORE_REINIT) {
      requestFirebaseReinit("consecutive heartbeat failures: " + err);
    }
  }
}

#endif
