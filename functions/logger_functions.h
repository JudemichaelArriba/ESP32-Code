// logger_functions.h
#ifndef LOGGER_FUNCTIONS_H
#define LOGGER_FUNCTIONS_H

#include "../core/structures.h"
#include "utility_functions.h"
#include "persistence_functions.h"

static const unsigned long DECISION_LOG_FAILURE_COOLDOWN_MS = 5UL * 60UL * 1000UL;

void drainBufferedDecisionLogs();

// Events that could not reach Firebase. Kept in RAM only: a reboot mid-outage
// is covered by the persistent diagnostic ring, not by this buffer.
static BufferedDecisionLog decisionLogBuffer[DECISION_LOG_BUFFER_CAPACITY];
static uint8_t decisionLogBufferHead = 0;
static uint8_t decisionLogBufferCount = 0;
static uint16_t droppedDecisionLogCount = 0;
// Set only when a replay write fails. Firebase.ready() reports token state, not
// the health of the socket a write just died on, so a failed drain must back off
// instead of reattempting a blocking write on the next pass.
static unsigned long lastDecisionLogDrainFailureMillis = 0;

static bool canWriteDecisionLogToFirebase() {
  return firebaseInitialized && WiFi.status() == WL_CONNECTED && Firebase.ready();
}

static String loggerRoomUid() {
  return assignedRoom.found ? assignedRoom.uid : String("");
}

static void fillCommonDecisionRecord(BufferedDecisionLog& record, const String& eventType) {
  memset(&record, 0, sizeof(record));
  copyToFixedBuffer(eventType, record.eventType, sizeof(record.eventType));
  copyToFixedBuffer(loggerRoomUid(), record.roomUid, sizeof(record.roomUid));
  copyToFixedBuffer(lastScheduleMode, record.mode, sizeof(record.mode));
  // Captured at event time so a replayed entry keeps its real wall clock.
  copyToFixedBuffer(nowIsoString(), record.updatedAt, sizeof(record.updatedAt));
  record.uptimeMs = (uint32_t)millis();
  record.suggestedTemp = -1;
  record.previousTemp = -1;
}

static void buildDecisionLogJson(const BufferedDecisionLog& record, bool buffered,
                                 FirebaseJson& json) {
  json.set("deviceId", String(DEVICE_ID));
  json.set("roomUid", String(record.roomUid));
  json.set("eventType", String(record.eventType));
  json.set("mode", String(record.mode));
  json.set("updatedAt", String(record.updatedAt));
  json.set("uptimeMs", (double)record.uptimeMs);
  json.set("source", String(record.source));
  json.set("power", record.power);
  json.set("targetTemp", (int)record.targetTemp);
  if (record.acStateChange) {
    json.set("previousPower", record.previousPower);
    json.set("previousTemp", (int)record.previousTemp);
    json.set("previousSource", String(record.previousSource));
    json.set("irSent", record.irSent);
  } else if (record.suggestedTemp >= AC_TEMP_MIN && record.suggestedTemp <= AC_TEMP_MAX) {
    json.set("suggestedTemp", (int)record.suggestedTemp);
  }
  json.set("aiAutoApply", record.aiAutoApply);
  json.set("applied", record.applied);
  json.set("reason", String(record.reason));
  if (buffered) {
    // Recorded locally during an outage and uploaded after recovery.
    json.set("bufferedDuringOutage", true);
  }
}

static bool writeDecisionLogRecord(const BufferedDecisionLog& record, bool buffered) {
  if (!canWriteDecisionLogToFirebase()) return false;

  FirebaseJson json;
  buildDecisionLogJson(record, buffered, json);

  markRuntimeOperation("firebase_decision_log");
  bool written = Firebase.RTDB.pushJSON(&fbdo, "/decisionLogs", &json);
  clearRuntimeOperation();
  if (written) {
    return true;
  }

  Serial.println("DecisionLog: write failed (" + fbdo.errorReason() + ")");
  return false;
}

static void enqueueDecisionLogRecord(const BufferedDecisionLog& record) {
  if (decisionLogBufferCount == DECISION_LOG_BUFFER_CAPACITY) {
    // Drop the oldest entry. The newest device state is the most useful after a
    // long outage, and the dropped total is reported once the backlog drains.
    decisionLogBufferHead = (decisionLogBufferHead + 1) % DECISION_LOG_BUFFER_CAPACITY;
    decisionLogBufferCount--;
    if (droppedDecisionLogCount < 65535) droppedDecisionLogCount++;
  }

  const uint8_t slot =
      (decisionLogBufferHead + decisionLogBufferCount) % DECISION_LOG_BUFFER_CAPACITY;
  decisionLogBuffer[slot] = record;
  decisionLogBufferCount++;
}

static void emitDecisionLogRecord(const BufferedDecisionLog& record) {
  // While a backlog exists, queue behind it instead of overtaking it, so
  // /decisionLogs keeps the order the events actually happened in.
  if (decisionLogBufferCount == 0 && writeDecisionLogRecord(record, false)) return;
  enqueueDecisionLogRecord(record);
}

// Drains a bounded number of entries per loop pass. The Firebase client is
// synchronous, so a full backlog must never be flushed in one pass, and a drain
// that just failed must not reattempt a blocking write on the very next pass.
void drainBufferedDecisionLogs() {
  if (decisionLogBufferCount == 0 && droppedDecisionLogCount == 0) return;
  if (!canWriteDecisionLogToFirebase()) return;

  // Failure cooldown, mirroring HEARTBEAT_FAILURE_RETRY_MS. Successful drains
  // never set this, so a healthy backlog still clears at the per-cycle rate.
  const unsigned long now = millis();
  if (lastDecisionLogDrainFailureMillis != 0 &&
      (now - lastDecisionLogDrainFailureMillis) < DECISION_LOG_DRAIN_RETRY_MS) {
    return;
  }

  for (uint8_t sent = 0;
       sent < DECISION_LOG_DRAIN_PER_CYCLE && decisionLogBufferCount > 0;
       sent++) {
    if (!writeDecisionLogRecord(decisionLogBuffer[decisionLogBufferHead], true)) {
      lastDecisionLogDrainFailureMillis = now == 0 ? 1 : now;
      return;  // Still unavailable; retry after the cooldown.
    }
    decisionLogBufferHead = (decisionLogBufferHead + 1) % DECISION_LOG_BUFFER_CAPACITY;
    decisionLogBufferCount--;
    lastDecisionLogDrainFailureMillis = 0;
  }

  if (decisionLogBufferCount > 0 || droppedDecisionLogCount == 0) return;

  BufferedDecisionLog overflow;
  fillCommonDecisionRecord(overflow, "decision_log_overflow");
  copyToFixedBuffer("buffer", overflow.source, sizeof(overflow.source));
  copyToFixedBuffer("dropped=" + String(droppedDecisionLogCount), overflow.reason,
                    sizeof(overflow.reason));
  overflow.power = acPowerState;
  overflow.targetTemp = (int16_t)normalizeACTemp((float)acTempState);
  overflow.aiAutoApply = aiAutoApplyEnabled;
  if (writeDecisionLogRecord(overflow, true)) {
    droppedDecisionLogCount = 0;
    lastDecisionLogDrainFailureMillis = 0;
  } else {
    lastDecisionLogDrainFailureMillis = now == 0 ? 1 : now;
  }
}

void logDecisionEvent(const String& eventType,
                      const String& source,
                      bool power,
                      int targetTemp,
                      int suggestedTemp,
                      bool aiAutoApply,
                      bool applied,
                      const String& reason) {
  BufferedDecisionLog record;
  fillCommonDecisionRecord(record, eventType);
  copyToFixedBuffer(source, record.source, sizeof(record.source));
  copyToFixedBuffer(reason, record.reason, sizeof(record.reason));
  record.power = power;
  record.targetTemp = (int16_t)normalizeACTemp((float)targetTemp);
  record.suggestedTemp = (int16_t)suggestedTemp;
  record.aiAutoApply = aiAutoApply;
  record.applied = applied;
  emitDecisionLogRecord(record);
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
  BufferedDecisionLog record;
  fillCommonDecisionRecord(record, "ac_state_changed");
  copyToFixedBuffer(source, record.source, sizeof(record.source));
  copyToFixedBuffer(reason, record.reason, sizeof(record.reason));
  copyToFixedBuffer(previousSource, record.previousSource, sizeof(record.previousSource));
  record.acStateChange = true;
  record.power = newPower;
  record.targetTemp = (int16_t)normalizeACTemp((float)newTemp);
  record.previousPower = previousPower;
  record.previousTemp = (int16_t)normalizeACTemp((float)previousTemp);
  record.irSent = irSent;
  record.aiAutoApply = aiAutoApplyEnabled;
  record.applied = true;
  emitDecisionLogRecord(record);
}

#endif
