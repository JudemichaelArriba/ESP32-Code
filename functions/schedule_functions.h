//schedule_functions.h
#ifndef SCHEDULE_FUNCTIONS_H
#define SCHEDULE_FUNCTIONS_H

#include "../core/structures.h"
#include "utility_functions.h"

ScheduleStatus evaluateScheduleStatus(const struct tm& t);
bool shouldPollSensors();

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

#endif
