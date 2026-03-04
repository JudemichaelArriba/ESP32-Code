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
  lastPresenceReported = presenceDetected;

  String basePath = "/devices/" + String(DEVICE_ID);
  Firebase.RTDB.setBool(&fbdo, basePath + "/occupancy", presenceDetected);
}

void refreshOccupancyOnly() {
  const unsigned long now = millis();

  if (pirMotionLatched) {
    pirMotionLatched = false;
    lastPirMotionMillis = now;
  }

  const bool pirRawActive = PIR_ACTIVE_HIGH ? (digitalRead(PIR_PIN) == HIGH) : (digitalRead(PIR_PIN) == LOW);
  pirMotionDetected = pirRawActive;

  if ((now - lastMlxReadMillis) >= MLX_INTERVAL_MS || lastMlxReadMillis == 0) {
    lastMlxReadMillis = now;
    mlxObjectTemp = mlx.readObjectTempC();
    mlxAmbientTemp = mlx.readAmbientTempC();
    mlxPresenceDetected = !isnan(mlxObjectTemp) && (mlxObjectTemp > occupancyThreshold);
  }

  const bool pirRecentMotion = (lastPirMotionMillis != 0) && ((now - lastPirMotionMillis) <= PIR_HOLD_MS);
  const bool anyDetected = mlxPresenceDetected || pirRawActive || pirRecentMotion;

  if (anyDetected) {
    lastPresenceDetectedMillis = now;
  }

  presenceDetected = (lastPresenceDetectedMillis != 0) && ((now - lastPresenceDetectedMillis) <= PIR_HOLD_MS);
  pushOccupancyIfChanged();
}

void refreshSensorsAndOccupancy() {
  const unsigned long now = millis();

  if (pirMotionLatched) {
    pirMotionLatched = false;
    lastPirMotionMillis = now;
  }

  const bool pirRawActive = PIR_ACTIVE_HIGH ? (digitalRead(PIR_PIN) == HIGH) : (digitalRead(PIR_PIN) == LOW);
  pirMotionDetected = pirRawActive;

  if ((now - lastDhtReadMillis) >= DHT_INTERVAL_MS || lastDhtReadMillis == 0) {
    lastDhtReadMillis = now;
    float humidity = dht.readHumidity();
    float temperature = dht.readTemperature();

    if (!isnan(humidity) && !isnan(temperature)) {
      lastHumidity = humidity;
      lastTemperature = temperature;

      String basePath = "/devices/" + String(DEVICE_ID);
      Firebase.RTDB.setFloat(&fbdo, basePath + "/temperature", lastTemperature);
      Firebase.RTDB.setFloat(&fbdo, basePath + "/humidity", lastHumidity);
    }
  }

  if ((now - lastMlxReadMillis) >= MLX_INTERVAL_MS || lastMlxReadMillis == 0) {
    lastMlxReadMillis = now;
    mlxObjectTemp = mlx.readObjectTempC();
    mlxAmbientTemp = mlx.readAmbientTempC();
    mlxPresenceDetected = !isnan(mlxObjectTemp) && (mlxObjectTemp > occupancyThreshold);
  }

  const bool pirRecentMotion = (lastPirMotionMillis != 0) && ((now - lastPirMotionMillis) <= PIR_HOLD_MS);
  const bool anyDetected = mlxPresenceDetected || pirRawActive || pirRecentMotion;

  if (anyDetected) {
    lastPresenceDetectedMillis = now;
  }

  presenceDetected = (lastPresenceDetectedMillis != 0) && ((now - lastPresenceDetectedMillis) <= PIR_HOLD_MS);
  pushOccupancyIfChanged();
}

void disableSensorsAndOccupancyIfIdle() {
  presenceDetected = false;
  lastPresenceDetectedMillis = 0;
  pushOccupancyIfChanged();
}

bool forceReadDhtNow() {
  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature();

  if (isnan(humidity) || isnan(temperature)) return false;

  lastDhtReadMillis = millis();
  lastHumidity = humidity;
  lastTemperature = temperature;

  String basePath = "/devices/" + String(DEVICE_ID);
  Firebase.RTDB.setFloat(&fbdo, basePath + "/temperature", lastTemperature);
  Firebase.RTDB.setFloat(&fbdo, basePath + "/humidity", lastHumidity);
  return true;
}

bool callRenderMLAndGetTarget(int& targetTempOut) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (isnan(lastTemperature) || isnan(lastHumidity)) return false;

  HTTPClient http;
  http.begin(renderURL);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> doc;
  JsonArray rooms = doc.createNestedArray("rooms");
  JsonObject room1 = rooms.createNestedObject();
  room1["id"] = assignedRoom.found ? assignedRoom.uid : String(DEVICE_ID);
  room1["temperature"] = lastTemperature;
  room1["humidity"] = lastHumidity;

  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  if (code <= 0) {
    http.end();
    return false;
  }

  String response = http.getString();
  http.end();

  DynamicJsonDocument res(1024);
  if (deserializeJson(res, response) != DeserializationError::Ok) return false;
  if (!res[0]["recommended_ac"].is<float>() && !res[0]["recommended_ac"].is<int>()) return false;

  targetTempOut = normalizeACTemp(res[0]["recommended_ac"].as<float>());
  return true;
}

#endif
