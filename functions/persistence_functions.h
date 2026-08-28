// persistence_functions.h
#ifndef PERSISTENCE_FUNCTIONS_H
#define PERSISTENCE_FUNCTIONS_H

#include <Preferences.h>
#include "../core/structures.h"

static const char* DEVICE_STATE_NAMESPACE = "ocutemp";
static const uint32_t OVERRIDE_STATE_VERSION = 1;
static const uint32_t RUNTIME_BREADCRUMB_MAGIC = 0x4F435542UL;  // "OCUB"

static bool overridePersistenceCacheInitialized = false;
static bool persistedOverrideActive = false;
static bool persistedOverridePower = false;
static int persistedOverrideTemp = 24;
static String persistedOverrideUntil = "";

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
