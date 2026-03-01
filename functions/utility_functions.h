#ifndef UTILITY_FUNCTIONS_H
#define UTILITY_FUNCTIONS_H

#include "../core/structures.h"

void IRAM_ATTR onPirMotion();
int normalizeACTemp(float recommendedTemp);
String nowIsoString();
bool timeIsValid(struct tm& t);
int parseTimeToMinute(const String& hhmm);
const char* weekdayName(int wday);
bool sameDay(const String& scheduleDay, int wday);
bool isWithinMinuteRange(int nowMin, int startMin, int endMin);

// Implementation
void IRAM_ATTR onPirMotion() {
  const unsigned long nowMs = millis();
  if ((nowMs - lastPirInterruptMillis) >= PIR_RETRIGGER_GUARD_MS) {
    pirMotionLatched = true;
    lastPirInterruptMillis = nowMs;
  }
}

int normalizeACTemp(float recommendedTemp) {
  int target = (int)roundf(recommendedTemp);
  if (target < AC_TEMP_MIN) target = AC_TEMP_MIN;
  if (target > AC_TEMP_MAX) target = AC_TEMP_MAX;
  return target;
}

String nowIsoString() {
  struct tm t;
  if (!getLocalTime(&t, 50)) return String("1970-01-01T00:00:00");

  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &t);
  return String(buf);
}

bool timeIsValid(struct tm& t) {
  if (!getLocalTime(&t, 50)) return false;
  return (t.tm_year + 1900) >= 2024;
}

int parseTimeToMinute(const String& hhmm) {
  int sep = hhmm.indexOf(':');
  if (sep <= 0) return -1;

  int hh = hhmm.substring(0, sep).toInt();
  int mm = hhmm.substring(sep + 1).toInt();
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return -1;
  return hh * 60 + mm;
}

const char* weekdayName(int wday) {
  static const char* names[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
  };
  if (wday < 0 || wday > 6) return "";
  return names[wday];
}

bool sameDay(const String& scheduleDay, int wday) {
  String d = scheduleDay;
  d.trim();
  d.toLowerCase();

  String cur = String(weekdayName(wday));
  cur.toLowerCase();

  return d == cur;
}

bool isWithinMinuteRange(int nowMin, int startMin, int endMin) {
  if (endMin >= startMin) {
    return nowMin >= startMin && nowMin < endMin;
  }
  return (nowMin >= startMin || nowMin < endMin);
}

#endif