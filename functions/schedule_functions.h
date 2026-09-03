//schedule_functions.h
#ifndef SCHEDULE_FUNCTIONS_H
#define SCHEDULE_FUNCTIONS_H

#include "../core/structures.h"
#include "utility_functions.h"
#include "persistence_functions.h"

ScheduleStatus evaluateScheduleStatus(const struct tm& t);
bool shouldPollSensors();
bool restoreScheduleOccupancyCheckpoint(const String& scheduleWindowKey,
                                        unsigned long nowMs,
                                        unsigned long& scheduleWindowEnteredMillisOut);

// Implementation
static String normalizedScheduleDay(const String& scheduleDay) {
  String day = scheduleDay;
  day.trim();
  day.toLowerCase();
  return day;
}

static String makeScheduleWindowKey(const ScheduleSlot& slot, const struct tm& t) {
  String key = assignedRoom.uid;
  key += "|";
  key += dateKeyFromTm(t);
  key += "|";
  key += normalizedScheduleDay(slot.day);
  key += "|";
  key += String(slot.startMinute);
  key += "|";
  key += String(slot.endMinute);
  return key;
}

ScheduleStatus evaluateScheduleStatus(const struct tm& t) {
  ScheduleStatus status;

  if (!assignedRoom.found) return status;

  int nowMinute = t.tm_hour * 60 + t.tm_min;
  for (int i = 0; i < assignedRoom.scheduleCount; i++) {
    ScheduleSlot& s = assignedRoom.schedules[i];
    if (!sameDay(s.day, t.tm_wday)) continue;

    status.hasScheduleToday = true;

    int preCoolStart = s.startMinute - PRECOOL_MINUTES;
    if (preCoolStart < 0) preCoolStart += 1440;

    if (isWithinMinuteRange(nowMinute, preCoolStart, s.startMinute)) {
      status.inPreCool = true;
      if (status.windowKey.length() == 0) {
        status.windowKey = makeScheduleWindowKey(s, t);
      }
    }

    if (isWithinMinuteRange(nowMinute, s.startMinute, s.endMinute)) {
      status.inSchedule = true;
      status.windowKey = makeScheduleWindowKey(s, t);
    }
  }

  return status;
}

bool shouldPollSensors() {
  return manualOverrideActive || currentScheduleStatus.inPreCool || currentScheduleStatus.inSchedule;
}

// Warm recovery after a reboot that landed inside the same schedule window the
// device was already serving. Restores the real empty-room grace baseline and,
// when the unified hold has not expired, the occupancy it was holding.
//
// Returns true only when the persisted window matches the active one, so a cold
// start into a new window keeps its normal fresh-entry behaviour. This path can
// only bring the empty-room shutdown forward or extend an existing hold by at
// most OCCUPANCY_HOLD_MS; it never powers the AC on by itself.
bool restoreScheduleOccupancyCheckpoint(const String& scheduleWindowKey,
                                        unsigned long nowMs,
                                        unsigned long& scheduleWindowEnteredMillisOut) {
  if (scheduleWindowKey.length() == 0) return false;

  OccupancyCheckpoint checkpoint;
  if (!readOccupancyCheckpoint(checkpoint)) return false;
  if (scheduleWindowKey != String(checkpoint.scheduleWindowKey)) return false;
  if (checkpoint.scheduleWindowStartEpoch <= 0) return false;

  // Never restore on an unverified clock: the reconstruction is wall-clock math.
  time_t nowEpochValue;
  if (!currentEpochIfValid(nowEpochValue)) return false;
  const int64_t nowEpoch = (int64_t)nowEpochValue;

  const int64_t windowAgeSeconds = nowEpoch - checkpoint.scheduleWindowStartEpoch;
  if (windowAgeSeconds < 0) return false;  // Clock moved backwards.
  if (windowAgeSeconds > (int64_t)(OCCUPANCY_PERSIST_MAX_AGE_MS / 1000UL)) {
    Serial.println("Recovery: schedule checkpoint too old, ignored.");
    return false;
  }

  // Unsigned subtraction keeps the reconstructed reference correct even though
  // millis() restarted at boot; every consumer measures it as a difference.
  scheduleWindowEnteredMillisOut = nowMs - (unsigned long)(windowAgeSeconds * 1000);
  Serial.printf("Recovery: schedule window resumed (entered %ld s ago).\n",
                (long)windowAgeSeconds);

  if (checkpoint.presenceHeld && checkpoint.lastPresenceEpoch > 0) {
    const int64_t presenceAgeSeconds = nowEpoch - checkpoint.lastPresenceEpoch;
    if (presenceAgeSeconds >= 0 &&
        presenceAgeSeconds <= (int64_t)(OCCUPANCY_HOLD_MS / 1000UL)) {
      unsigned long restoredPresenceMillis =
          nowMs - (unsigned long)(presenceAgeSeconds * 1000);
      // Zero is the "no evidence recorded" sentinel.
      if (restoredPresenceMillis == 0) restoredPresenceMillis = 1;
      lastPresenceDetectedMillis = restoredPresenceMillis;
      presenceDetected = true;
      Serial.printf("Recovery: occupancy hold resumed (evidence %ld s ago).\n",
                    (long)presenceAgeSeconds);
    }
  }

  return true;
}

#endif
