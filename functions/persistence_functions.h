// persistence_functions.h
#ifndef PERSISTENCE_FUNCTIONS_H
#define PERSISTENCE_FUNCTIONS_H

#include <Preferences.h>
#include "../core/structures.h"

static const char* DEVICE_STATE_NAMESPACE = "ocutemp";
static const uint32_t OVERRIDE_STATE_VERSION = 1;
static const uint32_t RUNTIME_BREADCRUMB_MAGIC = 0x4F435542UL;  // "OCUB"
static const uint32_t DIAGNOSTIC_HISTORY_MAGIC = 0x4F434448UL;  // "OCDH"
static const uint8_t DIAGNOSTIC_HISTORY_VERSION = 1;
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
