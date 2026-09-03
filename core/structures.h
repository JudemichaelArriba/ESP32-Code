// structures.h
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
  String windowKey;
};

struct EnergyRuntimeState {
  bool active = false;
  String roomUid;
  String sessionStartedAt;
  String lastFlushAt;
  String lastSource;
  String dateKey;
};

struct EnergyDailyCache {
  bool loaded = false;
  String dateKey;
  String roomUid;
  unsigned long runtimeSeconds = 0;
  float estimatedKwh = 0.0f;
  unsigned long sessionCount = 0;
};

// ---------------------------------------------------------------------------
// Deferred action set by the polled control stream and consumed by loop().
// Firebase writes and IR sends stay outside stream parsing.
// ---------------------------------------------------------------------------
struct StreamPendingAction {
  bool hasPending              = false;
  bool power                   = false;
  int  temp                    = 24;
  char source[32]              = "";
  bool writeForcedOffFalse     = false;  // write control/forcedOff = false
  bool writeForcedOffPersisted = false;  // write control/forcedOffPersisted
  bool forcedOffPersistedVal   = false;  // value for above
  bool writeForcedOffWindowKey = false;  // write control/forcedOffWindowKey
  char forcedOffWindowKey[96]  = "";
};

struct RuntimeBreadcrumb {
  uint32_t magic;
  uint32_t uptimeMs;
  uint32_t bootNumber;
  char operation[40];
};

const uint8_t DIAGNOSTIC_HISTORY_CAPACITY = 6;

struct DiagnosticRecord {
  uint32_t bootNumber;
  uint32_t uptimeMs;
  char event[28];
  char detail[68];
};

struct DiagnosticHistory {
  uint32_t magic;
  uint8_t version;
  uint8_t count;
  uint8_t nextIndex;
  DiagnosticRecord records[DIAGNOSTIC_HISTORY_CAPACITY];
};

// ---------------------------------------------------------------------------
// One decision log event held locally while Firebase is unreachable. Fixed-size
// fields keep the ring allocation static; String members would fragment the
// heap on a device that stays up for weeks.
// ---------------------------------------------------------------------------
struct BufferedDecisionLog {
  char eventType[24];
  char source[24];
  char reason[40];
  char mode[24];
  char updatedAt[24];
  char roomUid[32];
  char previousSource[24];
  int16_t targetTemp;
  int16_t suggestedTemp;
  int16_t previousTemp;
  uint32_t uptimeMs;
  bool power;
  bool previousPower;
  bool aiAutoApply;
  bool applied;
  bool irSent;
  bool acStateChange;
};

// ---------------------------------------------------------------------------
// Energy accounting checkpoint. Absolute daily totals (never increments) so a
// merge after reboot is idempotent and can never double count.
// ---------------------------------------------------------------------------
struct EnergyCheckpoint {
  uint32_t magic;
  uint8_t version;
  char dateKey[12];
  char sessionStartedAt[24];
  char lastFlushAt[24];
  uint32_t runtimeSeconds;
  uint32_t sessionCount;
};

// ---------------------------------------------------------------------------
// Occupancy and schedule-window references anchored to wall clock, so a reboot
// can restore the real grace baseline instead of restarting it from boot time.
// ---------------------------------------------------------------------------
struct OccupancyCheckpoint {
  uint32_t magic;
  uint8_t version;
  char scheduleWindowKey[96];
  int64_t scheduleWindowStartEpoch;
  int64_t lastPresenceEpoch;
  bool presenceHeld;
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
extern EnergyRuntimeState energyRuntimeState;
extern EnergyDailyCache energyDailyCache;

extern float lastHumidity;
extern float lastTemperature;
extern float mlxObjectTemp;
extern float mlxAmbientTemp;
extern float mlxDeltaTemp;

extern bool pirMotionDetected;
extern bool mlxPresenceDetected;
extern bool presenceDetected;
extern bool lastPresenceReported;
extern bool occupancyPublishPending;
extern unsigned long lastOccupancyPublishAttemptMillis;
extern uint8_t mlxPositiveReadStreak;
extern uint8_t mlxNegativeReadStreak;

extern volatile bool pirMotionLatched;
extern volatile unsigned long lastPirInterruptMillis;
extern unsigned long lastPirMotionMillis;
extern unsigned long lastPresenceDetectedMillis;

extern bool acPowerState;
extern int acTempState;
extern String acSourceState;
extern bool acIrStateTrusted;

extern bool manualOverrideActive;
extern bool manualOverridePower;
extern int manualOverrideTemp;
extern bool aiAutoApplyEnabled;

extern bool streamAttached;
extern bool firebaseInitialized;
extern bool startupStateLoaded;
extern bool restoredManualOverridePendingApply;

extern unsigned long lastDhtReadMillis;
extern unsigned long lastMlxReadMillis;
extern unsigned long lastMLCallMillis;
extern unsigned long lastWiFiReconnectAttempt;
extern unsigned long lastNtpSyncMillis;
extern unsigned long lastNtpValidMillis;
extern unsigned long lastNtpCheckMillis;
extern bool ntpTimeValid;
extern uint8_t consecutiveNtpFailures;
extern unsigned long lastWiFiConnectedMillis;
extern unsigned long lastFirebaseInitMillis;
extern unsigned long lastStreamRetryMillis;
extern unsigned long lastRoomsFetchAttemptMillis;
extern int lastCheckedMinuteStamp;
extern bool minuteGateInitialized;
extern bool wifiLinkUp;
extern bool wifiHasConnectedOnce;
extern String lastScheduleMode;
extern String lastScheduleWindowKey;

extern uint8_t netAuthState;
extern unsigned long netAuthStateSince;

extern String manualOverrideUntil;
extern int manualOverrideTargetTemp;
extern int estimatedWattsOn;

extern bool forcedOffActive;
extern String forcedOffWindowKey;
extern bool idleOccupancyPublished;
extern bool sensorWindowActive;
extern bool mlxAvailable;
extern unsigned long lastMlxInitAttemptMillis;
extern uint32_t bootCount;
extern uint32_t firebaseRecoveryCount;
extern unsigned long lastFirebaseReadyMillis;
extern unsigned long firebaseUnavailableSinceMillis;
extern unsigned long networkOutageSinceMillis;
extern uint8_t firebaseSessionRecoveryStreak;
extern unsigned long lastHeartbeatSuccessMillis;
extern uint8_t consecutiveHeartbeatFailures;
extern String lastFirebaseTokenStatus;
extern int lastFirebaseTokenErrorCode;
extern String lastFirebaseTokenError;
extern RuntimeBreadcrumb runtimeBreadcrumb;
extern bool previousResetBreadcrumbAvailable;
extern uint32_t previousResetUptimeMs;
extern uint32_t previousResetBootNumber;
extern char previousResetOperation[40];

// Deferred stream action (defined in main.ino)
extern StreamPendingAction streamPendingAction;

#endif
