//config.h
#ifndef CONFIG_H
#define CONFIG_H

#define DHTPIN  13
#define DHTTYPE DHT22
#define PIR_PIN 14
#define IR_LED_PIN 12

// Hold this GPIO low for WIFI_RESET_HOLD_MS to clear saved Wi-Fi credentials.
// Wire the button between GPIO27 and GND, or change this pin for your PCB.
#define WIFI_RESET_PIN 27

const char* NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_SEC = 8 * 3600;
const int DAYLIGHT_OFFSET_SEC = 0;

const char* WIFI_PORTAL_SSID = "OcuTemp-Setup";

const float MLX_HUMAN_OBJECT_MIN_C = 30.0f;
const float MLX_HUMAN_OBJECT_MAX_C = 40.0f;
const float MLX_HUMAN_DELTA_MIN_C = 2.0f;
const uint8_t MLX_CONFIRM_READS = 2;
const uint8_t MLX_CLEAR_READS = 2;
const bool PIR_ACTIVE_HIGH = true;
const unsigned long PIR_RETRIGGER_GUARD_MS = 80;
const unsigned long PIR_HOLD_MS = 3UL * 60UL * 1000UL;
const unsigned long OCCUPANCY_EMPTY_OFF_MS = 20UL * 60UL * 1000UL;
const unsigned long DHT_INTERVAL_MS = 7000;
const unsigned long MLX_INTERVAL_MS = 3000;
const unsigned long ML_INTERVAL_MS = 15UL * 60UL * 1000UL;
const unsigned long WIFI_CONNECT_TIMEOUT_SEC = 20;
const unsigned long WIFI_PORTAL_TIMEOUT_SEC = 300;
const unsigned long WIFI_RECONNECT_MS = 5000;
const uint8_t WIFI_MAX_RECONNECT_FAILURES = 36;
const unsigned long WIFI_RECONNECT_RESTART_STABLE_MS = 8000;
const unsigned long WIFI_RESET_HOLD_MS = 5UL * 1000UL;
const unsigned long NTP_RESYNC_MS = 60000;
const unsigned long WIFI_STABLE_BEFORE_FB_MS = 5000;
const unsigned long FIREBASE_AUTH_SETTLE_MS = 20000;
const unsigned long FIREBASE_AUTH_TIMEOUT_MS = 90UL * 1000UL;
const unsigned long FIREBASE_STREAM_RETRY_MS = 5000;
const unsigned long FIREBASE_REINIT_BACKOFF_MIN_MS = 5000;
const unsigned long FIREBASE_REINIT_BACKOFF_MAX_MS = 60UL * 1000UL;
const uint8_t FIREBASE_FAILURES_BEFORE_REINIT = 3;
const uint16_t FIREBASE_SOCKET_TIMEOUT_MS = 10000;
const uint16_t FIREBASE_SSL_HANDSHAKE_TIMEOUT_MS = 15000;
const uint16_t FIREBASE_SERVER_RESPONSE_TIMEOUT_MS = 10000;
const uint16_t FIREBASE_STREAM_KEEPALIVE_MS = 45000;
const uint16_t FIREBASE_STREAM_RECONNECT_MS = 3000;
const uint16_t FIREBASE_STREAM_ERROR_MS = 5000;
const unsigned long ROOMS_FETCH_RETRY_MS = 5000;
const unsigned long ENERGY_FLUSH_INTERVAL_SEC = 60UL;
const int WIFI_MIN_SIGNAL_QUALITY = 8;
const bool WIFI_MANAGER_DEBUG_OUTPUT = false;

const int AC_TEMP_MIN = 17;
const int AC_TEMP_MAX = 30;
const int PRECOOL_MINUTES = 10;
const int PRECOOL_TEMP = 17;
const int DEFAULT_ESTIMATED_WATTS_ON = 950;
const uint8_t  IR_SEND_REPEAT_COUNT    = 5;
const uint16_t IR_SEND_REPEAT_DELAY_MS = 200;  
const unsigned long HEARTBEAT_INTERVAL_MS = 60UL * 1000UL;
const unsigned long HEARTBEAT_FAILURE_RETRY_MS = 10UL * 1000UL;
const unsigned long MLX_REINIT_INTERVAL_MS = 30UL * 1000UL;
const uint32_t LOOP_WATCHDOG_TIMEOUT_MS = 45UL * 1000UL;
const char* FIRMWARE_VERSION = "2026.08.27-recovery1";
#endif
