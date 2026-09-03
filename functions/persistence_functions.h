// persistence_functions.h
#ifndef PERSISTENCE_FUNCTIONS_H
#define PERSISTENCE_FUNCTIONS_H

#include <Preferences.h>
#include "../core/structures.h"
#include "utility_functions.h"

static const char* DEVICE_STATE_NAMESPACE = "ocutemp";
static const uint32_t OVERRIDE_STATE_VERSION = 1;
static const uint32_t RUNTIME_BREADCRUMB_MAGIC = 0x4F435542UL;  // "OCUB"
static const uint32_t DIAGNOSTIC_HISTORY_MAGIC = 0x4F434448UL;  // "OCDH"
static const uint8_t DIAGNOSTIC_HISTORY_VERSION = 1;
static const uint32_t ENERGY_CHECKPOINT_MAGIC = 0x4F43454BUL;   // "OCEK"
static const uint8_t ENERGY_CHECKPOINT_VERSION = 1;
static const uint32_t OCCUPANCY_CHECKPOINT_MAGIC = 0x4F434F4BUL;  // "OCOK"
static const uint8_t OCCUPANCY_CHECKPOINT_VERSION = 1;
static const unsigned long DIAGNOSTIC_DUPLICATE_COOLDOWN_MS = 60UL * 1000UL;

static bool overridePersistenceCacheInitialized = false;
static bool persistedOverrideActive = false;
static bool persistedOverridePower = false;
static int persistedOverrideTemp = 24;
static String persistedOverrideUntil = "";
static DiagnosticHistory diagnosticHistoryCache;
static bool diagnosticHistoryLoaded = false;
static String lastDiagnosticEvent = "";
static unsigned long lastDiagnosticEventMillis = 0;

static void initializeDiagnosticHistory(DiagnosticHistory& history) {
  memset(&history, 0, sizeof(history));
  history.magic = DIAGNOSTIC_HISTORY_MAGIC;
  history.version = DIAGNOSTIC_HISTORY_VERSION;
}

static void loadDiagnosticHistoryCache() {
  if (diagnosticHistoryLoaded) return;
  diagnosticHistoryLoaded = true;
  initializeDiagnosticHistory(diagnosticHistoryCache);

  Preferences preferences;
  if (!preferences.begin(DEVICE_STATE_NAMESPACE, true)) return;
  const size_t storedLength = preferences.getBytesLength("diagHist");
  if (storedLength == sizeof(DiagnosticHistory)) {
    DiagnosticHistory stored;
    if (preferences.getBytes("diagHist", &stored, sizeof(stored)) == sizeof(stored) &&
        stored.magic == DIAGNOSTIC_HISTORY_MAGIC &&
        stored.version == DIAGNOSTIC_HISTORY_VERSION &&
        stored.count <= DIAGNOSTIC_HISTORY_CAPACITY &&
        stored.nextIndex < DIAGNOSTIC_HISTORY_CAPACITY) {
      diagnosticHistoryCache = stored;
    }
  }
  preferences.end();
}

static bool saveDiagnosticHistoryCache() {
  Preferences preferences;
  if (!preferences.begin(DEVICE_STATE_NAMESPACE, false)) return false;
  const bool saved = preferences.putBytes("diagHist", &diagnosticHistoryCache,
                                          sizeof(diagnosticHistoryCache)) ==
                     sizeof(diagnosticHistoryCache);
  preferences.end();
  return saved;
}

void recordPersistentDiagnostic(const char* event, const String& detail) {
  if (event == nullptr || event[0] == '\0') return;

  const unsigned long now = millis();
  if (lastDiagnosticEvent == event && lastDiagnosticEventMillis != 0 &&
      (now - lastDiagnosticEventMillis) < DIAGNOSTIC_DUPLICATE_COOLDOWN_MS) {
    return;
  }

  loadDiagnosticHistoryCache();
  DiagnosticRecord& record =
      diagnosticHistoryCache.records[diagnosticHistoryCache.nextIndex];
  memset(&record, 0, sizeof(record));
  record.bootNumber = bootCount;
  record.uptimeMs = now;
  strncpy(record.event, event, sizeof(record.event) - 1);
  strncpy(record.detail, detail.c_str(), sizeof(record.detail) - 1);

  diagnosticHistoryCache.nextIndex =
      (diagnosticHistoryCache.nextIndex + 1) % DIAGNOSTIC_HISTORY_CAPACITY;
  if (diagnosticHistoryCache.count < DIAGNOSTIC_HISTORY_CAPACITY) {
    diagnosticHistoryCache.count++;
  }

  if (!saveDiagnosticHistoryCache()) {
    Serial.println("Diagnostics: unable to persist event.");
  }
  lastDiagnosticEvent = event;
  lastDiagnosticEventMillis = now;
}

void appendPersistentDiagnosticsToJson(FirebaseJson& json) {
  loadDiagnosticHistoryCache();
  json.set("diagnostics/count", diagnosticHistoryCache.count);

  const uint8_t start =
      (diagnosticHistoryCache.nextIndex + DIAGNOSTIC_HISTORY_CAPACITY -
       diagnosticHistoryCache.count) % DIAGNOSTIC_HISTORY_CAPACITY;
  for (uint8_t i = 0; i < diagnosticHistoryCache.count; i++) {
    const uint8_t recordIndex = (start + i) % DIAGNOSTIC_HISTORY_CAPACITY;
    const DiagnosticRecord& record = diagnosticHistoryCache.records[recordIndex];
    const String base = "diagnostics/items/" + String(i);
    json.set(base + "/event", String(record.event));
    json.set(base + "/detail", String(record.detail));
    json.set(base + "/bootNumber", (int)record.bootNumber);
    json.set(base + "/uptimeMs", (int)record.uptimeMs);
  }
}

void clearUploadedPersistentDiagnostics() {
  loadDiagnosticHistoryCache();
  if (diagnosticHistoryCache.count == 0) return;
  initializeDiagnosticHistory(diagnosticHistoryCache);
  if (!saveDiagnosticHistoryCache()) {
    Serial.println("Diagnostics: unable to clear uploaded events.");
  }
}

static void updateOverridePersistenceCache() {
  persistedOverrideActive = manualOverrideActive;
  persistedOverridePower = manualOverridePower;
  persistedOverrideTemp = normalizeACTemp((float)manualOverrideTargetTemp);
  persistedOverrideUntil = manualOverrideUntil;
  overridePersistenceCacheInitialized = true;
}

bool restoreManualOverrideFromPreferences() {
  Preferences preferences;
  if (!preferences.begin(DEVICE_STATE_NAMESPACE, true)) {
    Serial.println("Override restore: unable to open NVS.");
    return false;
  }

  const uint32_t version = preferences.getUInt("ovVersion", 0);
  if (version != OVERRIDE_STATE_VERSION) {
    preferences.end();
    return false;
  }

  manualOverrideActive = preferences.getBool("ovActive", false);
  manualOverridePower = preferences.getBool("ovPower", false);
  manualOverrideTargetTemp = normalizeACTemp((float)preferences.getInt("ovTemp", 24));
  manualOverrideTemp = manualOverrideTargetTemp;
  manualOverrideUntil = preferences.getString("ovUntil", "");
  preferences.end();

  updateOverridePersistenceCache();
  restoredManualOverridePendingApply = manualOverrideActive;
  Serial.printf("Override restored from NVS: active=%d power=%d temp=%d\n",
                manualOverrideActive, manualOverridePower, manualOverrideTargetTemp);
  return true;
}

bool persistManualOverrideState() {
  const int normalizedTemp = normalizeACTemp((float)manualOverrideTargetTemp);
  if (overridePersistenceCacheInitialized &&
      persistedOverrideActive == manualOverrideActive &&
      persistedOverridePower == manualOverridePower &&
      persistedOverrideTemp == normalizedTemp &&
      persistedOverrideUntil == manualOverrideUntil) {
    return true;
  }

  Preferences preferences;
  if (!preferences.begin(DEVICE_STATE_NAMESPACE, false)) {
    Serial.println("Override persistence: unable to open NVS.");
    return false;
  }

  bool ok = true;
  ok = preferences.putUInt("ovVersion", OVERRIDE_STATE_VERSION) > 0 && ok;
  ok = preferences.putBool("ovActive", manualOverrideActive) > 0 && ok;
  ok = preferences.putBool("ovPower", manualOverridePower) > 0 && ok;
  ok = preferences.putInt("ovTemp", normalizedTemp) > 0 && ok;
  // putString legitimately returns zero for an empty value; verify by reading it back.
  preferences.putString("ovUntil", manualOverrideUntil);
  ok = preferences.getString("ovUntil", "__missing__") == manualOverrideUntil && ok;
  preferences.end();

  if (ok) {
    updateOverridePersistenceCache();
  } else {
    Serial.println("Override persistence: NVS write verification failed.");
  }
  return ok;
}

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

static void copyToFixedBuffer(const String& value, char* buffer, size_t bufferSize) {
  if (buffer == nullptr || bufferSize == 0) return;
  strncpy(buffer, value.c_str(), bufferSize - 1);
  buffer[bufferSize - 1] = '\0';
}

// Wall-clock epoch, only when NTP time is actually trustworthy. Every restore
// path depends on this: an unverified clock must never resurrect state.
static bool currentEpochIfValid(time_t& outEpoch) {
  struct tm nowTm;
  if (!timeIsValid(nowTm)) return false;

  const time_t now = time(nullptr);
  if (now <= 0) return false;
  outEpoch = now;
  return true;
}

// ---------------------------------------------------------------------------
// Energy checkpoint: survives a reboot that happens while Firebase is offline.
// ---------------------------------------------------------------------------

static EnergyCheckpoint energyCheckpointCache;
static bool energyCheckpointCacheValid = false;
// Tracks whether NVS currently holds a checkpoint, so the routine clear on a
// healthy flush cycle never has to open flash.
static bool energyCheckpointStored = false;

bool loadEnergyCheckpoint(EnergyCheckpoint& out) {
  Preferences preferences;
  if (!preferences.begin(DEVICE_STATE_NAMESPACE, true)) return false;

  bool loaded = false;
  if (preferences.getBytesLength("engCkpt") == sizeof(EnergyCheckpoint)) {
    EnergyCheckpoint stored;
    if (preferences.getBytes("engCkpt", &stored, sizeof(stored)) == sizeof(stored) &&
        stored.magic == ENERGY_CHECKPOINT_MAGIC &&
        stored.version == ENERGY_CHECKPOINT_VERSION) {
      stored.dateKey[sizeof(stored.dateKey) - 1] = '\0';
      stored.sessionStartedAt[sizeof(stored.sessionStartedAt) - 1] = '\0';
      stored.lastFlushAt[sizeof(stored.lastFlushAt) - 1] = '\0';
      out = stored;
      loaded = true;
      energyCheckpointStored = true;
    }
  }
  preferences.end();
  return loaded;
}

// Called only while Firebase is unreachable, at the existing energy flush
// cadence. Redundant writes are skipped so continuous operation does not churn
// the flash sector.
bool persistEnergyCheckpoint() {
  if (!energyDailyCache.loaded || energyDailyCache.dateKey.length() == 0) return false;

  EnergyCheckpoint next;
  memset(&next, 0, sizeof(next));
  next.magic = ENERGY_CHECKPOINT_MAGIC;
  next.version = ENERGY_CHECKPOINT_VERSION;
  copyToFixedBuffer(energyDailyCache.dateKey, next.dateKey, sizeof(next.dateKey));
  copyToFixedBuffer(energyRuntimeState.sessionStartedAt, next.sessionStartedAt,
                    sizeof(next.sessionStartedAt));
  copyToFixedBuffer(energyRuntimeState.lastFlushAt, next.lastFlushAt,
                    sizeof(next.lastFlushAt));
  next.runtimeSeconds = (uint32_t)energyDailyCache.runtimeSeconds;
  next.sessionCount = (uint32_t)energyDailyCache.sessionCount;

  if (energyCheckpointCacheValid &&
      memcmp(&energyCheckpointCache, &next, sizeof(next)) == 0) {
    return true;
  }

  Preferences preferences;
  if (!preferences.begin(DEVICE_STATE_NAMESPACE, false)) {
    Serial.println("Energy checkpoint: unable to open NVS.");
    return false;
  }
  const bool saved =
      preferences.putBytes("engCkpt", &next, sizeof(next)) == sizeof(next);
  preferences.end();

  if (saved) {
    energyCheckpointCache = next;
    energyCheckpointCacheValid = true;
    energyCheckpointStored = true;
    // Confirms the offline accounting actually reached flash. The unchanged-content
    // path above returns before this, so a stable session does not spam the log.
    Serial.printf("Energy checkpoint: saved %s runtime=%lus sessions=%lu.\n",
                  next.dateKey, (unsigned long)next.runtimeSeconds,
                  (unsigned long)next.sessionCount);
  } else {
    Serial.println("Energy checkpoint: NVS write failed.");
  }
  return saved;
}

// Cleared once the accumulated totals are safely back in Firebase. Called on
// every healthy flush cycle, so it must be free when nothing is stored.
void clearEnergyCheckpoint() {
  // No-op branch: nothing stored, so stay silent and touch no flash.
  if (!energyCheckpointStored) return;

  Preferences preferences;
  if (!preferences.begin(DEVICE_STATE_NAMESPACE, false)) {
    Serial.println("Energy checkpoint: unable to open NVS to clear.");
    return;
  }
  preferences.remove("engCkpt");
  preferences.end();
  memset(&energyCheckpointCache, 0, sizeof(energyCheckpointCache));
  energyCheckpointCacheValid = false;
  energyCheckpointStored = false;
  Serial.println("Energy checkpoint: cleared after confirmed Firebase write.");
}

// ---------------------------------------------------------------------------
// Occupancy / schedule-window checkpoint.
// ---------------------------------------------------------------------------

static OccupancyCheckpoint occupancyCheckpointCache;
static bool occupancyCheckpointLoaded = false;
static unsigned long lastOccupancyEvidencePersistMillis = 0;

static void initializeOccupancyCheckpoint(OccupancyCheckpoint& checkpoint) {
  memset(&checkpoint, 0, sizeof(checkpoint));
  checkpoint.magic = OCCUPANCY_CHECKPOINT_MAGIC;
  checkpoint.version = OCCUPANCY_CHECKPOINT_VERSION;
}

static void loadOccupancyCheckpointCache() {
  if (occupancyCheckpointLoaded) return;
  occupancyCheckpointLoaded = true;
  initializeOccupancyCheckpoint(occupancyCheckpointCache);

  Preferences preferences;
  if (!preferences.begin(DEVICE_STATE_NAMESPACE, true)) return;
  if (preferences.getBytesLength("occCkpt") == sizeof(OccupancyCheckpoint)) {
    OccupancyCheckpoint stored;
    if (preferences.getBytes("occCkpt", &stored, sizeof(stored)) == sizeof(stored) &&
        stored.magic == OCCUPANCY_CHECKPOINT_MAGIC &&
        stored.version == OCCUPANCY_CHECKPOINT_VERSION) {
      stored.scheduleWindowKey[sizeof(stored.scheduleWindowKey) - 1] = '\0';
      occupancyCheckpointCache = stored;
    }
  }
  preferences.end();
}

static bool saveOccupancyCheckpointCache() {
  Preferences preferences;
  if (!preferences.begin(DEVICE_STATE_NAMESPACE, false)) {
    Serial.println("Occupancy checkpoint: unable to open NVS.");
    return false;
  }
  const bool saved = preferences.putBytes("occCkpt", &occupancyCheckpointCache,
                                          sizeof(occupancyCheckpointCache)) ==
                     sizeof(occupancyCheckpointCache);
  preferences.end();
  if (!saved) {
    Serial.println("Occupancy checkpoint: NVS write failed.");
  }
  return saved;
}

bool readOccupancyCheckpoint(OccupancyCheckpoint& out) {
  loadOccupancyCheckpointCache();
  if (occupancyCheckpointCache.magic != OCCUPANCY_CHECKPOINT_MAGIC) return false;
  out = occupancyCheckpointCache;
  return true;
}

// Records the wall-clock moment this device genuinely entered a schedule
// window. Only called on a real window transition, so writes stay rare.
void noteScheduleWindowEnteredForPersistence(const String& windowKey) {
  if (windowKey.length() == 0) return;

  time_t nowEpoch;
  if (!currentEpochIfValid(nowEpoch)) return;

  loadOccupancyCheckpointCache();
  copyToFixedBuffer(windowKey, occupancyCheckpointCache.scheduleWindowKey,
                    sizeof(occupancyCheckpointCache.scheduleWindowKey));
  occupancyCheckpointCache.scheduleWindowStartEpoch = (int64_t)nowEpoch;
  // A new window starts without inherited occupancy evidence.
  occupancyCheckpointCache.lastPresenceEpoch = 0;
  occupancyCheckpointCache.presenceHeld = false;
  saveOccupancyCheckpointCache();
}

// Direct PIR/MLX evidence only, throttled to bound flash wear. The throttle is
// consumed before the clock check so an invalid clock cannot force a
// getLocalTime() call on every loop pass.
void noteOccupancyEvidenceForPersistence() {
  const unsigned long now = millis();
  if (lastOccupancyEvidencePersistMillis != 0 &&
      (now - lastOccupancyEvidencePersistMillis) < OCCUPANCY_PERSIST_MIN_INTERVAL_MS) {
    return;
  }
  lastOccupancyEvidencePersistMillis = now == 0 ? 1 : now;

  time_t nowEpoch;
  if (!currentEpochIfValid(nowEpoch)) return;

  loadOccupancyCheckpointCache();
  if (occupancyCheckpointCache.scheduleWindowKey[0] == '\0') return;

  occupancyCheckpointCache.lastPresenceEpoch = (int64_t)nowEpoch;
  occupancyCheckpointCache.presenceHeld = true;
  saveOccupancyCheckpointCache();
}

void capturePreviousResetBreadcrumb() {
  previousResetBreadcrumbAvailable = runtimeBreadcrumb.magic == RUNTIME_BREADCRUMB_MAGIC &&
                                     runtimeBreadcrumb.operation[0] != '\0';
  if (previousResetBreadcrumbAvailable) {
    strncpy(previousResetOperation, runtimeBreadcrumb.operation,
            sizeof(previousResetOperation) - 1);
    previousResetOperation[sizeof(previousResetOperation) - 1] = '\0';
    previousResetUptimeMs = runtimeBreadcrumb.uptimeMs;
    previousResetBootNumber = runtimeBreadcrumb.bootNumber;
    Serial.printf("Previous reset breadcrumb: %s at %lu ms\n",
                  previousResetOperation, (unsigned long)previousResetUptimeMs);
    recordPersistentDiagnostic("previous_reset", String(previousResetOperation));
  }

  runtimeBreadcrumb.magic = 0;
  runtimeBreadcrumb.operation[0] = '\0';
}

void markRuntimeOperation(const char* operation) {
  runtimeBreadcrumb.magic = RUNTIME_BREADCRUMB_MAGIC;
  runtimeBreadcrumb.uptimeMs = millis();
  runtimeBreadcrumb.bootNumber = bootCount;
  strncpy(runtimeBreadcrumb.operation, operation,
          sizeof(runtimeBreadcrumb.operation) - 1);
  runtimeBreadcrumb.operation[sizeof(runtimeBreadcrumb.operation) - 1] = '\0';
}

void clearRuntimeOperation() {
  runtimeBreadcrumb.magic = 0;
  runtimeBreadcrumb.operation[0] = '\0';
}

#endif
