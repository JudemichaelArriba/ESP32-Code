//sensor_functions.h
#ifndef SENSOR_FUNCTIONS_H
#define SENSOR_FUNCTIONS_H

#include "../core/structures.h"
#include "persistence_functions.h"

void pushOccupancyIfChanged();
bool pushOccupancyToFirebase(bool force);
void refreshOccupancyOnly();
void refreshSensorsAndOccupancy();
void disableSensorsAndOccupancyIfIdle();
void tickOccupancySerialDiagnostics();
bool forceReadDhtNow();
bool callRenderMLAndGetTarget(int& targetTempOut);

// Implementation
static bool canWriteSensorDataToFirebase() {
  return firebaseInitialized && WiFi.status() == WL_CONNECTED && Firebase.ready();
}

static void markSensorWindowActive() {
  sensorWindowActive = true;
  idleOccupancyPublished = false;
}

bool pushOccupancyToFirebase(bool force) {
  const unsigned long now = millis();
  const bool pirRecentMotion = (lastPirMotionMillis != 0) &&
                               ((now - lastPirMotionMillis) <= PIR_HOLD_MS);
  static bool lastPresenceLogged = true;
  if (lastPresenceLogged != presenceDetected) {
    Serial.printf("Occupancy: %s [pir=%d recent=%d mlx=%d obj=%.1f amb=%.1f delta=%.1f]\n",
                  presenceDetected ? "true" : "false",
                  pirMotionDetected,
                  pirRecentMotion,
                  mlxPresenceDetected,
                  mlxObjectTemp,
                  mlxAmbientTemp,
                  mlxDeltaTemp);
    lastPresenceLogged = presenceDetected;
  }

  if (lastPresenceReported != presenceDetected) {
    occupancyPublishPending = true;
  }
  if (!force && !occupancyPublishPending) return true;
  if (!canWriteSensorDataToFirebase()) return false;
  if (lastOccupancyPublishAttemptMillis != 0 &&
      (now - lastOccupancyPublishAttemptMillis) < OCCUPANCY_PUBLISH_RETRY_MS) {
    return false;
  }

  const bool occupancyToPublish = presenceDetected;
  lastOccupancyPublishAttemptMillis = now;

  String basePath = "/devices/" + String(DEVICE_ID);
  markRuntimeOperation("firebase_occupancy");
  bool written = Firebase.RTDB.setBool(&fbdo, basePath + "/occupancy", occupancyToPublish);
  clearRuntimeOperation();
  if (written) {
    // Only a confirmed Firebase write is considered reported. If the local
    // state ever changes during this operation, leave the new value pending.
    lastPresenceReported = occupancyToPublish;
    occupancyPublishPending = (presenceDetected != occupancyToPublish);
  } else {
    occupancyPublishPending = true;
  }
  return written;
}

void pushOccupancyIfChanged() {
  pushOccupancyToFirebase(false);
}

static void updateMlxPresenceFilter(const bool mlxHumanLikeNow) {
  if (mlxHumanLikeNow) {
    if (mlxPositiveReadStreak < 255) mlxPositiveReadStreak++;
    mlxNegativeReadStreak = 0;
    if (mlxPositiveReadStreak >= MLX_CONFIRM_READS) {
      mlxPresenceDetected = true;
    }
    return;
  }

  mlxPositiveReadStreak = 0;
  if (mlxNegativeReadStreak < 255) mlxNegativeReadStreak++;
  if (mlxNegativeReadStreak >= MLX_CLEAR_READS) {
    mlxPresenceDetected = false;
  }
}

static void refreshMlxPresenceIfDue(const unsigned long now) {
  if (!mlxAvailable) {
    if ((now - lastMlxInitAttemptMillis) < MLX_REINIT_INTERVAL_MS) return;
    lastMlxInitAttemptMillis = now;
    mlxAvailable = mlx.begin();
    Serial.println(mlxAvailable ? "MLX90614: recovered." : "MLX90614: retry failed.");
    if (!mlxAvailable) return;
  }

  if ((now - lastMlxReadMillis) >= MLX_INTERVAL_MS || lastMlxReadMillis == 0) {
    lastMlxReadMillis = now;
    mlxObjectTemp = mlx.readObjectTempC();
    mlxAmbientTemp = mlx.readAmbientTempC();

    if (!isnan(mlxObjectTemp) && !isnan(mlxAmbientTemp)) {
      mlxDeltaTemp = mlxObjectTemp - mlxAmbientTemp;
    } else {
      mlxDeltaTemp = NAN;
    }

    const bool mlxHumanLikeNow = !isnan(mlxObjectTemp) &&
                                 !isnan(mlxAmbientTemp) &&
                                 mlxObjectTemp >= MLX_HUMAN_OBJECT_MIN_C &&
                                 mlxObjectTemp <= MLX_HUMAN_OBJECT_MAX_C &&
                                 mlxDeltaTemp >= MLX_HUMAN_DELTA_MIN_C;
    updateMlxPresenceFilter(mlxHumanLikeNow);
  }
}

static void refreshOccupancyState() {
  const unsigned long now = millis();
  bool pirMotionEvent = false;

  if (pirMotionLatched) {
    pirMotionLatched = false;
    lastPirMotionMillis = now;
    pirMotionEvent = true;
  }

  const bool pirRawActive = PIR_ACTIVE_HIGH ? (digitalRead(PIR_PIN) == HIGH) : (digitalRead(PIR_PIN) == LOW);
  pirMotionDetected = pirRawActive;
  refreshMlxPresenceIfDue(now);

  const bool anyDetectedNow = pirMotionEvent || pirRawActive || mlxPresenceDetected;

  if (anyDetectedNow) {
    lastPresenceDetectedMillis = now;
  }

  // Hold the combined result once from the latest real sensor detection. The
  // schedule uses the same duration from lastPresenceDetectedMillis, so this
  // does not stack another delay before the empty-room shutdown.
  presenceDetected = (lastPresenceDetectedMillis != 0) &&
                     ((now - lastPresenceDetectedMillis) <= OCCUPANCY_HOLD_MS);
}

void refreshOccupancyOnly() {
  markSensorWindowActive();
  refreshOccupancyState();
  pushOccupancyIfChanged();
}

void refreshSensorsAndOccupancy() {
  markSensorWindowActive();
  const unsigned long now = millis();

  if ((now - lastDhtReadMillis) >= DHT_INTERVAL_MS || lastDhtReadMillis == 0) {
    lastDhtReadMillis = now;
    float humidity = dht.readHumidity();
    float temperature = dht.readTemperature();

    if (!isnan(humidity) && !isnan(temperature)) {
      // Only push to Firebase if the temperature changed by 0.2C or humidity by 1.0%.
      // This stops the ESP32 from spamming the network and overflowing the SSL buffer!
      bool shouldPublish = isnan(lastTemperature) || isnan(lastHumidity) ||
                           abs(lastTemperature - temperature) > 0.2 ||
                           abs(lastHumidity - humidity) > 1.0;

      lastHumidity = humidity;
      lastTemperature = temperature;

      if (shouldPublish && canWriteSensorDataToFirebase()) {
        String basePath = "/devices/" + String(DEVICE_ID);
        markRuntimeOperation("firebase_sensor_write");
        Firebase.RTDB.setFloat(&fbdo, basePath + "/temperature", lastTemperature);
        Firebase.RTDB.setFloat(&fbdo, basePath + "/humidity", lastHumidity);
        clearRuntimeOperation();
      }
    }
  }

  refreshOccupancyState();
  pushOccupancyIfChanged();
}

void disableSensorsAndOccupancyIfIdle() {
  pirMotionLatched = false;
  pirMotionDetected = false;
  mlxPresenceDetected = false;
  mlxPositiveReadStreak = 0;
  mlxNegativeReadStreak = 0;
  mlxObjectTemp = NAN;
  mlxAmbientTemp = NAN;
  mlxDeltaTemp = NAN;
  presenceDetected = false;
  lastPirMotionMillis = 0;
  lastPresenceDetectedMillis = 0;
  lastDhtReadMillis = 0;
  lastMlxReadMillis = 0;
  sensorWindowActive = false;
  if (idleOccupancyPublished) return;
  if (pushOccupancyToFirebase(true)) {
    idleOccupancyPublished = true;
  }
}

void tickOccupancySerialDiagnostics() {
  static unsigned long lastDiagnosticMillis = 0;
  const unsigned long now = millis();
  if (lastDiagnosticMillis != 0 &&
      (now - lastDiagnosticMillis) < OCCUPANCY_SERIAL_DIAGNOSTIC_INTERVAL_MS) {
    return;
  }
  lastDiagnosticMillis = now;

  const bool pirRecentMotion = (lastPirMotionMillis != 0) &&
                               ((now - lastPirMotionMillis) <= PIR_HOLD_MS);
  const bool occupancyHoldActive = presenceDetected &&
                                   !pirMotionDetected &&
                                   !mlxPresenceDetected;
  const bool mlxReadingValid = !isnan(mlxObjectTemp) && !isnan(mlxAmbientTemp) &&
                               !isnan(mlxDeltaTemp);
  const bool presenceAgeKnown = lastPresenceDetectedMillis != 0;
  const unsigned long presenceAgeMs = presenceAgeKnown
                                          ? now - lastPresenceDetectedMillis
                                          : 0;

  Serial.printf(
      "Occupancy diag: presence=%d hold=%d pir=%d recent=%d mlx=%d mlxOk=%d "
      "obj=%.1f amb=%.1f delta=%.1f ageMs=%lu ageKnown=%d pending=%d window=%d\n",
      presenceDetected, occupancyHoldActive, pirMotionDetected, pirRecentMotion,
      mlxPresenceDetected, mlxReadingValid, mlxObjectTemp, mlxAmbientTemp,
      mlxDeltaTemp, presenceAgeMs, presenceAgeKnown, occupancyPublishPending,
      sensorWindowActive);
}

bool forceReadDhtNow() {
  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature();

  if (isnan(humidity) || isnan(temperature)) return false;

  lastDhtReadMillis = millis();
  
  // [NEW FIX]: Same bandwidth-saving logic applied here to prevent SSL crashes.
  bool shouldPublish = isnan(lastTemperature) || isnan(lastHumidity) ||
                       abs(lastTemperature - temperature) > 0.2 ||
                       abs(lastHumidity - humidity) > 1.0;

  lastHumidity = humidity;
  lastTemperature = temperature;

  if (shouldPublish && canWriteSensorDataToFirebase()) {
    String basePath = "/devices/" + String(DEVICE_ID);
    markRuntimeOperation("firebase_sensor_write");
    Firebase.RTDB.setFloat(&fbdo, basePath + "/temperature", lastTemperature);
    Firebase.RTDB.setFloat(&fbdo, basePath + "/humidity", lastHumidity);
    clearRuntimeOperation();
  }
  
  return true;
}

bool callRenderMLAndGetTarget(int& targetTempOut) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("ML: skipped (WiFi not connected)");
    return false;
  }
  if (isnan(lastTemperature) || isnan(lastHumidity)) {
    Serial.println("ML: skipped (DHT data not ready)");
    return false;
  }

  HTTPClient http;
  http.begin(renderURL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-api-key", API_KEY);

  StaticJsonDocument<256> doc;
  JsonArray rooms = doc.createNestedArray("rooms");
  JsonObject room1 = rooms.createNestedObject();
  room1["id"] = assignedRoom.found ? assignedRoom.uid : String(DEVICE_ID);
  room1["temperature"] = lastTemperature;
  room1["humidity"] = lastHumidity;

  String body;
  serializeJson(doc, body);
  Serial.println("ML: sending request -> " + body);

  http.setConnectTimeout(FIREBASE_SOCKET_TIMEOUT_MS);
  http.setTimeout(FIREBASE_SERVER_RESPONSE_TIMEOUT_MS);
  markRuntimeOperation("ml_http_request");
  int code = http.POST(body);
  if (code <= 0) {
    Serial.printf("ML: HTTP POST failed (%d)\n", code);
    http.end();
    clearRuntimeOperation();
    return false;
  }

  String response = http.getString();
  http.end();
  clearRuntimeOperation();
  Serial.printf("ML: HTTP %d response: %s\n", code, response.c_str());

  DynamicJsonDocument res(1024);
  if (deserializeJson(res, response) != DeserializationError::Ok) {
    Serial.println("ML: parse failed");
    return false;
  }

  JsonVariant rec;
  if (res.is<JsonArray>() && !res.as<JsonArray>().isNull() && res.as<JsonArray>().size() > 0) {
    rec = res[0]["recommended_ac"];
  } else {
    rec = res["recommended_ac"];
  }

  if (!rec.is<float>() && !rec.is<int>()) {
    Serial.println("ML: recommended_ac missing/invalid");
    return false;
  }

  targetTempOut = normalizeACTemp(rec.as<float>());
  Serial.printf("ML: recommended temp = %d\n", targetTempOut);
  return true;
}

#endif
