#ifndef CONFIG_H
#define CONFIG_H

#define DHTPIN  18
#define DHTTYPE DHT22
#define PIR_PIN 4
#define IR_LED_PIN 14

const char* NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_SEC = 8 * 3600;
const int DAYLIGHT_OFFSET_SEC = 0;

const float occupancyThreshold = 30.0f;
const bool PIR_ACTIVE_HIGH = true;
const unsigned long PIR_RETRIGGER_GUARD_MS = 80;
const unsigned long PIR_HOLD_MS = 3UL * 60UL * 1000UL;
const unsigned long OCCUPANCY_EMPTY_OFF_MS = 20UL * 60UL * 1000UL;
const unsigned long DHT_INTERVAL_MS = 7000;
const unsigned long MLX_INTERVAL_MS = 3000;
const unsigned long ML_INTERVAL_MS = 30UL * 60UL * 1000UL;
const unsigned long WIFI_RECONNECT_MS = 5000;
const unsigned long NTP_RESYNC_MS = 60000;

const int AC_TEMP_MIN = 17;
const int AC_TEMP_MAX = 30;
const int PRECOOL_MINUTES = 15;
const int PRECOOL_TEMP = 24;

#endif