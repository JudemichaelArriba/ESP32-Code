// heartbeat_functions.h
#ifndef HEARTBEAT_FUNCTIONS_H
#define HEARTBEAT_FUNCTIONS_H

#include "../core/structures.h"
#include "utility_functions.h"


extern FirebaseData fbdo;

void tickHeartbeat();


static unsigned long lastHeartbeatMillis = 0;

void tickHeartbeat() {

  if (!firebaseInitialized) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (!Firebase.ready()) return;

  struct tm t;
  if (!timeIsValid(t)) return;


  unsigned long now = millis();
  if (lastHeartbeatMillis != 0 && (now - lastHeartbeatMillis) < HEARTBEAT_INTERVAL_MS) return;

  FirebaseJson json;
  json.set("lastSeen", nowIsoString());
  json.set("ip", WiFi.localIP().toString());

  String path = "/devices/" + String(DEVICE_ID) + "/status";

  if (Firebase.RTDB.setJSON(&fbdo, path, &json)) {
    Serial.println("Heartbeat: OK");
  } else {

    Serial.println("Heartbeat: write failed (" + fbdo.errorReason() + ")");
  }

  lastHeartbeatMillis = now;
}

#endif