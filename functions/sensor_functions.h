//sensor_functions.h
#ifndef SENSOR_FUNCTIONS_H
#define SENSOR_FUNCTIONS_H

#include "../core/structures.h"

void pushOccupancyIfChanged();
void refreshOccupancyOnly();
void refreshSensorsAndOccupancy();
void disableSensorsAndOccupancyIfIdle();
bool forceReadDhtNow();
bool callRenderMLAndGetTarget(int& targetTempOut);

// Implementation
void pushOccupancyIfChanged() {
  if (lastPresenceReported == presenceDetected) return;

  const unsigned long now = millis();
  const bool pirRecentMotion = (lastPirMotionMillis != 0) &&
                               ((now - lastPirMotionMillis) <= PIR_HOLD_MS);
  Serial.printf("Occupancy: %s [pir=%d recent=%d mlx=%d obj=%.1f amb=%.1f delta=%.1f]\n",
                presenceDetected ? "true" : "false",
                pirMotionDetected,
                pirRecentMotion,
                mlxPresenceDetected,
                mlxObjectTemp,
                mlxAmbientTemp,
                mlxDeltaTemp);

  lastPresenceReported = presenceDetected;

  String basePath = "/devices/" + String(DEVICE_ID);
  Firebase.RTDB.setBool(&fbdo, basePath + "/occupancy", presenceDetected);
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

  if (pirMotionLatched) {
    pirMotionLatched = false;
    lastPirMotionMillis = now;
  }

  const bool pirRawActive = PIR_ACTIVE_HIGH ? (digitalRead(PIR_PIN) == HIGH) : (digitalRead(PIR_PIN) == LOW);
  pirMotionDetected = pirRawActive;
  refreshMlxPresenceIfDue(now);

  const bool pirRecentMotion = (lastPirMotionMillis != 0) && ((now - lastPirMotionMillis) <= PIR_HOLD_MS);
  const bool anyDetected = pirRawActive || pirRecentMotion || mlxPresenceDetected;

  if (anyDetected) {
    lastPresenceDetectedMillis = now;
  }

  presenceDetected = (lastPresenceDetectedMillis != 0) && ((now - lastPresenceDetectedMillis) <= PIR_HOLD_MS);
}

void refreshOccupancyOnly() {
  refreshOccupancyState();
  pushOccupancyIfChanged();
}

void refreshSensorsAndOccupancy() {
  const unsigned long now = millis();

  if ((now - lastDhtReadMillis) >= DHT_INTERVAL_MS || lastDhtReadMillis == 0) {
    lastDhtReadMillis = now;
    float humidity = dht.readHumidity();
    float temperature = dht.readTemperature();

    if (!isnan(humidity) && !isnan(temperature)) {
      // Only push to Firebase if the temperature changed by 0.2C or humidity by 1.0%.
      // This stops the ESP32 from spamming the network and overflowing the SSL buffer!
      if (isnan(lastTemperature) || isnan(lastHumidity) || 
          abs(lastTemperature - temperature) > 0.2 || 
          abs(lastHumidity - humidity) > 1.0) {
          
        lastHumidity = humidity;
        lastTemperature = temperature;

        String basePath = "/devices/" + String(DEVICE_ID);
        Firebase.RTDB.setFloat(&fbdo, basePath + "/temperature", lastTemperature);
        Firebase.RTDB.setFloat(&fbdo, basePath + "/humidity", lastHumidity);
      }
    }
  }

  refreshOccupancyState();
  pushOccupancyIfChanged();
}

void disableSensorsAndOccupancyIfIdle() {
  pirMotionDetected = false;
  mlxPresenceDetected = false;
  mlxPositiveReadStreak = 0;
  mlxNegativeReadStreak = 0;
  mlxDeltaTemp = NAN;
  presenceDetected = false;
  lastPresenceDetectedMillis = 0;
  pushOccupancyIfChanged();
}

bool forceReadDhtNow() {
  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature();

  if (isnan(humidity) || isnan(temperature)) return false;

  lastDhtReadMillis = millis();
  
  // [NEW FIX]: Same bandwidth-saving logic applied here to prevent SSL crashes.
  if (isnan(lastTemperature) || isnan(lastHumidity) || 
      abs(lastTemperature - temperature) > 0.2 || 
      abs(lastHumidity - humidity) > 1.0) {
        
    lastHumidity = humidity;
    lastTemperature = temperature;

    String basePath = "/devices/" + String(DEVICE_ID);
    Firebase.RTDB.setFloat(&fbdo, basePath + "/temperature", lastTemperature);
    Firebase.RTDB.setFloat(&fbdo, basePath + "/humidity", lastHumidity);
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

  int code = http.POST(body);
  if (code <= 0) {
    Serial.printf("ML: HTTP POST failed (%d)\n", code);
    http.end();
    return false;
  }

  String response = http.getString();
  http.end();
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
