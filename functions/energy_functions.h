//energy_functions.h
#ifndef ENERGY_FUNCTIONS_H
#define ENERGY_FUNCTIONS_H

#include "../core/structures.h"
#include "utility_functions.h"

void loadEnergyProfileFromFirebase();
void syncEnergyProfileToFirebase();
void loadEnergyStateFromFirebase();
void initializeEnergyTrackingForCurrentState();
void tickEnergyTracking();
void handleEnergyPowerTransition(bool previousPower, bool newPower, const String& source);

static String energyBasePath() {
  return "/devices/" + String(DEVICE_ID);
}

static bool canSyncEnergyToFirebase() {
  return firebaseInitialized && WiFi.status() == WL_CONNECTED && Firebase.ready();
}

static float computeEstimatedKwh(unsigned long runtimeSeconds) {
  return ((float)runtimeSeconds * (float)estimatedWattsOn) / 3600000.0f;
}

static void resetEnergyDailyCache(const String& dateKey) {
  energyDailyCache = EnergyDailyCache();
  energyDailyCache.loaded = true;
  energyDailyCache.dateKey = dateKey;
  if (assignedRoom.found) {
    energyDailyCache.roomUid = assignedRoom.uid;
  } else if (energyRuntimeState.roomUid.length() > 0) {
    energyDailyCache.roomUid = energyRuntimeState.roomUid;
  }
}

static bool loadEnergyDailyCache(const String& dateKey) {
  if (energyDailyCache.loaded && energyDailyCache.dateKey == dateKey) {
    return true;
  }

  resetEnergyDailyCache(dateKey);

  if (!canSyncEnergyToFirebase()) {
    return true;
  }

  String path = energyBasePath() + "/energyDaily/" + dateKey;
  if (!Firebase.RTDB.getJSON(&fbdo, path)) {
    return true;
  }

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, fbdo.jsonString()) != DeserializationError::Ok) {
    return true;
  }

  if (!doc["roomUid"].isNull()) {
    energyDailyCache.roomUid = doc["roomUid"].as<String>();
  }
  if (!doc["runtimeSeconds"].isNull()) {
    energyDailyCache.runtimeSeconds = doc["runtimeSeconds"].as<unsigned long>();
  }
  if (!doc["sessionCount"].isNull()) {
    energyDailyCache.sessionCount = doc["sessionCount"].as<unsigned long>();
  }

  energyDailyCache.estimatedKwh = computeEstimatedKwh(energyDailyCache.runtimeSeconds);
  return true;
}

static bool syncEnergyDailyCache() {
  if (!energyDailyCache.loaded || !canSyncEnergyToFirebase()) {
    return false;
  }

  FirebaseJson json;
  json.set("roomUid", energyDailyCache.roomUid);
  json.set("runtimeSeconds", (int)energyDailyCache.runtimeSeconds);
  json.set("estimatedWattsOn", estimatedWattsOn);
  json.set("estimatedKwh", computeEstimatedKwh(energyDailyCache.runtimeSeconds));
  json.set("sessionCount", (int)energyDailyCache.sessionCount);
  json.set("updatedAt", nowIsoString());

  String path = energyBasePath() + "/energyDaily/" + energyDailyCache.dateKey;
  return Firebase.RTDB.setJSON(&fbdo, path, &json);
}

static bool syncEnergyStateToFirebase() {
  if (!canSyncEnergyToFirebase()) {
    return false;
  }

  FirebaseJson json;
  json.set("active", energyRuntimeState.active);
  json.set("roomUid", energyRuntimeState.roomUid);
  json.set("sessionStartedAt", energyRuntimeState.sessionStartedAt);
  json.set("lastFlushAt", energyRuntimeState.lastFlushAt);
  json.set("lastSource", energyRuntimeState.lastSource);
  json.set("dateKey", energyRuntimeState.dateKey);
  json.set("updatedAt", nowIsoString());

  String path = energyBasePath() + "/energyState";
  return Firebase.RTDB.setJSON(&fbdo, path, &json);
}

static void startEnergySession(const String& source) {
  struct tm nowTm;
  if (!timeIsValid(nowTm)) return;

  String nowIso = nowIsoString();
  String dateKey = dateKeyFromTm(nowTm);

  loadEnergyDailyCache(dateKey);
  if (assignedRoom.found) {
    energyDailyCache.roomUid = assignedRoom.uid;
  }
  energyDailyCache.sessionCount += 1;
  energyDailyCache.estimatedKwh = computeEstimatedKwh(energyDailyCache.runtimeSeconds);

  energyRuntimeState.active = true;
  energyRuntimeState.roomUid = assignedRoom.found ? assignedRoom.uid : energyRuntimeState.roomUid;
  energyRuntimeState.sessionStartedAt = nowIso;
  energyRuntimeState.lastFlushAt = nowIso;
  energyRuntimeState.lastSource = source;
  energyRuntimeState.dateKey = dateKey;

  syncEnergyDailyCache();
  syncEnergyStateToFirebase();
}

static void closeEnergySession(const String& source) {
  struct tm nowTm;
  if (timeIsValid(nowTm)) {
    energyRuntimeState.lastFlushAt = nowIsoString();
    energyRuntimeState.dateKey = dateKeyFromTm(nowTm);
  }

  if (assignedRoom.found) {
    energyRuntimeState.roomUid = assignedRoom.uid;
  }

  energyRuntimeState.active = false;
  energyRuntimeState.sessionStartedAt = "";
  energyRuntimeState.lastSource = source;

  syncEnergyStateToFirebase();
}

static void flushEnergyRuntime(bool force) {
  if (!energyRuntimeState.active) return;

  struct tm nowTm;
  if (!timeIsValid(nowTm)) return;

  time_t lastFlushEpoch;
  if (!parseLocalIsoStringToEpoch(energyRuntimeState.lastFlushAt, lastFlushEpoch)) {
    energyRuntimeState.lastFlushAt = nowIsoString();
    energyRuntimeState.dateKey = dateKeyFromTm(nowTm);
    syncEnergyStateToFirebase();
    return;
  }

  time_t nowEpoch = time(nullptr);
  if (nowEpoch <= lastFlushEpoch) {
    return;
  }

  unsigned long elapsedSeconds = (unsigned long)(nowEpoch - lastFlushEpoch);
  if (!force && elapsedSeconds < ENERGY_FLUSH_INTERVAL_SEC) {
    return;
  }

  time_t cursorEpoch = lastFlushEpoch;
  while (cursorEpoch < nowEpoch) {
    struct tm cursorTm;
    localtime_r(&cursorEpoch, &cursorTm);

    struct tm nextMidnight = cursorTm;
    nextMidnight.tm_mday += 1;
    nextMidnight.tm_hour = 0;
    nextMidnight.tm_min = 0;
    nextMidnight.tm_sec = 0;
    nextMidnight.tm_isdst = -1;

    time_t nextMidnightEpoch = mktime(&nextMidnight);
    if (nextMidnightEpoch == (time_t)-1 || nextMidnightEpoch <= cursorEpoch) {
      break;
    }

    time_t segmentEndEpoch = (nowEpoch < nextMidnightEpoch) ? nowEpoch : nextMidnightEpoch;
    unsigned long segmentSeconds = (unsigned long)(segmentEndEpoch - cursorEpoch);
    if (segmentSeconds == 0) {
      break;
    }

    String dateKey = dateKeyFromTm(cursorTm);
    loadEnergyDailyCache(dateKey);
    if (assignedRoom.found) {
      energyDailyCache.roomUid = assignedRoom.uid;
    } else if (energyRuntimeState.roomUid.length() > 0) {
      energyDailyCache.roomUid = energyRuntimeState.roomUid;
    }

    energyDailyCache.runtimeSeconds += segmentSeconds;
    energyDailyCache.estimatedKwh = computeEstimatedKwh(energyDailyCache.runtimeSeconds);
    syncEnergyDailyCache();

    cursorEpoch = segmentEndEpoch;
  }

  energyRuntimeState.lastFlushAt = nowIsoString();
  energyRuntimeState.dateKey = dateKeyFromTm(nowTm);
  if (assignedRoom.found) {
    energyRuntimeState.roomUid = assignedRoom.uid;
  }
  syncEnergyStateToFirebase();
}

void loadEnergyProfileFromFirebase() {
  estimatedWattsOn = DEFAULT_ESTIMATED_WATTS_ON;

  if (!canSyncEnergyToFirebase()) {
    return;
  }

  String path = energyBasePath() + "/energyProfile";
  if (!Firebase.RTDB.getJSON(&fbdo, path)) {
    return;
  }

  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, fbdo.jsonString()) != DeserializationError::Ok) {
    return;
  }

  if (!doc["estimatedWattsOn"].isNull()) {
    int watts = doc["estimatedWattsOn"].as<int>();
    if (watts > 0) {
      estimatedWattsOn = watts;
    }
  }
}

void syncEnergyProfileToFirebase() {
  if (!canSyncEnergyToFirebase()) {
    return;
  }

  FirebaseJson json;
  json.set("estimatedWattsOn", estimatedWattsOn);
  json.set("estimateMode", "fixed_runtime");
  json.set("updatedAt", nowIsoString());

  String path = energyBasePath() + "/energyProfile";
  Firebase.RTDB.setJSON(&fbdo, path, &json);
}

void loadEnergyStateFromFirebase() {
  energyRuntimeState = EnergyRuntimeState();

  if (!canSyncEnergyToFirebase()) {
    return;
  }

  String path = energyBasePath() + "/energyState";
  if (!Firebase.RTDB.getJSON(&fbdo, path)) {
    return;
  }

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, fbdo.jsonString()) != DeserializationError::Ok) {
    return;
  }

  if (!doc["active"].isNull()) {
    energyRuntimeState.active = doc["active"].as<bool>();
  }
  if (!doc["roomUid"].isNull()) {
    energyRuntimeState.roomUid = doc["roomUid"].as<String>();
  }
  if (!doc["sessionStartedAt"].isNull()) {
    energyRuntimeState.sessionStartedAt = doc["sessionStartedAt"].as<String>();
  }
  if (!doc["lastFlushAt"].isNull()) {
    energyRuntimeState.lastFlushAt = doc["lastFlushAt"].as<String>();
  }
  if (!doc["lastSource"].isNull()) {
    energyRuntimeState.lastSource = doc["lastSource"].as<String>();
  }
  if (!doc["dateKey"].isNull()) {
    energyRuntimeState.dateKey = doc["dateKey"].as<String>();
  }
}

void initializeEnergyTrackingForCurrentState() {
  struct tm nowTm;
  bool hasValidTime = timeIsValid(nowTm);
  if (hasValidTime) {
    loadEnergyDailyCache(dateKeyFromTm(nowTm));
  }

  if (acPowerState) {
    if (!energyRuntimeState.active) {
      startEnergySession(acSourceState);
      return;
    }

    if (energyRuntimeState.lastFlushAt.length() == 0) {
      energyRuntimeState.lastFlushAt = nowIsoString();
    }
    if (energyRuntimeState.sessionStartedAt.length() == 0) {
      energyRuntimeState.sessionStartedAt = energyRuntimeState.lastFlushAt;
    }
    if (assignedRoom.found) {
      energyRuntimeState.roomUid = assignedRoom.uid;
    }
    if (hasValidTime) {
      energyRuntimeState.dateKey = dateKeyFromTm(nowTm);
    }
    syncEnergyStateToFirebase();
    return;
  }

  if (energyRuntimeState.active) {
    closeEnergySession(acSourceState);
  }
}

void tickEnergyTracking() {
  if (!energyRuntimeState.active) return;
  flushEnergyRuntime(false);
}

void handleEnergyPowerTransition(bool previousPower, bool newPower, const String& source) {
  if (previousPower == newPower) {
    return;
  }

  if (!previousPower && newPower) {
    startEnergySession(source);
    return;
  }

  if (previousPower && !newPower) {
    flushEnergyRuntime(true);
    closeEnergySession(source);
  }
}

#endif
