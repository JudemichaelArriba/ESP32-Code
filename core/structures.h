#ifndef STRUCTURES_H
#define STRUCTURES_H

#include <Arduino.h>
#include "DHT.h"
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_MLX90614.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <ir_Coolix.h>
#include <time.h>
#include "../config/secrets.h"
#include "../config/config.h"

struct ScheduleSlot {
  String day;
  int startMinute;
  int endMinute;
};

struct RoomConfig {
  bool found = false;
  String uid;
  String roomName;
  String device;
  ScheduleSlot schedules[16];
  int scheduleCount = 0;
};

struct ScheduleStatus {
  bool hasScheduleToday = false;
  bool inPreCool = false;
  bool inSchedule = false;
};

// Global objects
extern DHT dht;
extern FirebaseData fbdo;
extern FirebaseData streamFbdo;
extern FirebaseAuth auth;
extern FirebaseConfig config;
extern Adafruit_MLX90614 mlx;
extern IRCoolixAC coolixAc;

// Global variables
extern RoomConfig assignedRoom;
extern ScheduleStatus currentScheduleStatus;

extern float lastHumidity;
extern float lastTemperature;
extern float mlxObjectTemp;
extern float mlxAmbientTemp;

extern bool pirMotionDetected;
extern bool mlxPresenceDetected;
extern bool presenceDetected;
extern bool lastPresenceReported;

extern volatile bool pirMotionLatched;
extern volatile unsigned long lastPirInterruptMillis;
extern unsigned long lastPirMotionMillis;
extern unsigned long lastPresenceDetectedMillis;

extern bool acPowerState;
extern int acTempState;
extern String acSourceState;

extern bool manualOverrideActive;
extern bool manualOverridePower;
extern int manualOverrideTemp;

extern bool streamAttached;
extern bool firebaseInitialized;
extern bool startupStateLoaded;

extern unsigned long lastDhtReadMillis;
extern unsigned long lastMlxReadMillis;
extern unsigned long lastMLCallMillis;
extern unsigned long lastWiFiReconnectAttempt;
extern unsigned long lastNtpSyncMillis;
extern unsigned long lastWiFiConnectedMillis;
extern unsigned long lastFirebaseInitMillis;
extern unsigned long lastStreamRetryMillis;
extern unsigned long lastRoomsFetchAttemptMillis;
extern int lastCheckedMinuteStamp;
extern bool minuteGateInitialized;
extern bool wifiLinkUp;
extern bool wifiHasConnectedOnce;
extern bool wifiReconnectRestartPending;
extern unsigned long wifiReconnectStableSince;
extern String lastScheduleMode;

extern uint8_t netAuthState;
extern unsigned long netAuthStateSince;

#endif


