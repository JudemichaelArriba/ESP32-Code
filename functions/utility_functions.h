//utility_functions.h
#ifndef UTILITY_FUNCTIONS_H
#define UTILITY_FUNCTIONS_H

#include "../core/structures.h"

void onPirMotion();
int normalizeACTemp(float recommendedTemp);
String nowIsoString();
String dateKeyFromTm(const struct tm& t);
bool timeIsValid(struct tm& t);
bool parseLocalIsoString(const String& iso, struct tm& t);
bool parseLocalIsoStringToEpoch(const String& iso, time_t& outEpoch);
bool parseIso8601ToEpoch(const String& iso, time_t& outEpoch);
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

static int64_t daysFromCivilDate(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = year / 400;
  const unsigned yearOfEra = (unsigned)(year - era * 400);
  const int adjustedMonth = (int)month + (month > 2 ? -3 : 9);
  const unsigned dayOfYear = (153 * (unsigned)adjustedMonth + 2) / 5 + day - 1;
  const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return (int64_t)era * 146097 + (int64_t)dayOfEra - 719468;
}

bool parseIso8601ToEpoch(const String& iso, time_t& outEpoch) {
  String s = iso;
  s.trim();
  if (s.length() == 0) return false;

  bool hasExplicitZone = false;
  int zoneOffsetSeconds = 0;
  if (s.endsWith("Z") || s.endsWith("z")) {
    hasExplicitZone = true;
  } else if (s.length() >= 6) {
    const int zoneIndex = s.length() - 6;
    const char sign = s.charAt(zoneIndex);
    if ((sign == '+' || sign == '-') && s.charAt(zoneIndex + 3) == ':') {
      const int zoneHour = s.substring(zoneIndex + 1, zoneIndex + 3).toInt();
      const int zoneMinute = s.substring(zoneIndex + 4, zoneIndex + 6).toInt();
      if (zoneHour > 23 || zoneMinute > 59) return false;
      zoneOffsetSeconds = (zoneHour * 3600 + zoneMinute * 60) * (sign == '-' ? -1 : 1);
      hasExplicitZone = true;
    }
  }

  int yr, mo, dy, hr, mn, sc;
  if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &yr, &mo, &dy, &hr, &mn, &sc) != 6) {
    return false;
  }
  if (yr < 2024 || mo < 1 || mo > 12 || dy < 1 || dy > 31 ||
      hr < 0 || hr > 23 || mn < 0 || mn > 59 || sc < 0 || sc > 60) {
    return false;
  }
  static const uint8_t daysPerMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int maxDay = daysPerMonth[mo - 1];
  const bool leapYear = (yr % 4 == 0 && yr % 100 != 0) || (yr % 400 == 0);
  if (mo == 2 && leapYear) maxDay = 29;
  if (dy > maxDay) return false;

  if (hasExplicitZone) {
    const int64_t epoch = daysFromCivilDate(yr, (unsigned)mo, (unsigned)dy) * 86400LL +
                          hr * 3600LL + mn * 60LL + sc - zoneOffsetSeconds;
    outEpoch = (time_t)epoch;
  } else {
    struct tm parsed = {};
    parsed.tm_year = yr - 1900;
    parsed.tm_mon = mo - 1;
    parsed.tm_mday = dy;
    parsed.tm_hour = hr;
    parsed.tm_min = mn;
    parsed.tm_sec = sc;
    parsed.tm_isdst = -1;
    outEpoch = mktime(&parsed);
  }
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
