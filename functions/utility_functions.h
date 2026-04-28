//utility_functions.h
#ifndef UTILITY_FUNCTIONS_H
#define UTILITY_FUNCTIONS_H

#include "../core/structures.h"

void IRAM_ATTR onPirMotion();
int normalizeACTemp(float recommendedTemp);
String nowIsoString();
String dateKeyFromTm(const struct tm& t);
bool timeIsValid(struct tm& t);
bool parseLocalIsoString(const String& iso, struct tm& t);
bool parseLocalIsoStringToEpoch(const String& iso, time_t& outEpoch);
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

String dateKeyFromTm(const struct tm& t) {
  char buf[16];
  strftime(buf, sizeof(buf), "%Y-%m-%d", &t);
  return String(buf);
}

bool timeIsValid(struct tm& t) {
  if (!getLocalTime(&t, 50)) return false;
  return (t.tm_year + 1900) >= 2024;
}

bool parseLocalIsoString(const String& iso, struct tm& t) {
  String s = iso;
  s.trim();
  if (s.length() == 0) return false;

  if (s.endsWith("Z")) {
    s.remove(s.length() - 1);
  }

  int dotIdx = s.indexOf('.');
  if (dotIdx > 0) {
    s = s.substring(0, dotIdx);
  }

  int yr, mo, dy, hr, mn, sc;
  if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &yr, &mo, &dy, &hr, &mn, &sc) != 6) {
    return false;
  }

  memset(&t, 0, sizeof(t));
  t.tm_year = yr - 1900;
  t.tm_mon = mo - 1;
  t.tm_mday = dy;
  t.tm_hour = hr;
  t.tm_min = mn;
  t.tm_sec = sc;
  t.tm_isdst = -1;

  time_t normalized = mktime(&t);
  return normalized != (time_t)-1;
}

bool parseLocalIsoStringToEpoch(const String& iso, time_t& outEpoch) {
  struct tm parsed;
  if (!parseLocalIsoString(iso, parsed)) return false;

  outEpoch = mktime(&parsed);
  return outEpoch != (time_t)-1;
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
