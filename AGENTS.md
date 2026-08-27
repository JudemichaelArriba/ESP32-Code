# AGENTS.md — OcuTemp ESP32 Firmware

## Project Identity

OcuTemp is production-grade ESP32 firmware for room air-conditioner automation. It controls Coolix-compatible AC units through IR commands, integrates with Firebase Realtime Database for remote control and monitoring, evaluates room schedules with occupancy-aware cooling, and optionally requests ML-predicted temperature recommendations.

The device operates as a deployed room controller with WiFi provisioning separate from AC control logic, enabling network changes without reflashing firmware.

---

## Core Product Principles

1. Separate WiFi provisioning from AC control logic.
2. Never hardcode WiFi credentials in firmware.
3. Track occupancy accurately through PIR and MLX90614 sensors.
4. Follow schedule-based control with pre-cooling and occupancy grace periods.
5. Support manual overrides with expiry timestamps.
6. Track energy usage and runtime sessions.
7. Persist all state changes to Firebase.
8. Keep the device responsive through non-blocking operations.

---

## Tech Stack

- **Platform**: ESP32 (Arduino framework)
- **Language**: C++ (Arduino-style)
- **WiFi**: WiFiManager v2.0.17 (captive portal provisioning)
- **Backend**: Firebase Realtime Database + Firebase Auth
- **Sensors**: DHT22 (temp/humidity), PIR motion, Adafruit MLX90614 (IR temperature)
- **AC Control**: IRremoteESP8266 (Coolix protocol)
- **ML Endpoint**: Optional Render API for temperature recommendations
- **Time**: NTP (pool.ntp.org, GMT+8)
- **Data Format**: ArduinoJson

---

## File Structure

```
ESP32-Code/
├── ESP32-Code.ino              # Main sketch, setup(), loop(), global state
├── README.md
├── config/
│   ├── config.h                # Hardware pins, thresholds, timing constants
│   └── secrets.h               # Firebase credentials, device identity, ML endpoint
├── core/
│   └── structures.h            # Shared structs, global externs, library includes
└── functions/
    ├── wifi_functions.h        # WiFiManager provisioning, reset button, reconnect
    ├── firebase_functions.h    # Firebase init, room fetch, control stream, sync
    ├── schedule_functions.h    # Schedule evaluation, pre-cool logic
    ├── ac_control.h            # Coolix IR commands, AC state application
    ├── sensor_functions.h      # DHT22, PIR, MLX90614, occupancy, ML endpoint
    ├── energy_functions.h      # Runtime sessions, energy tracking, daily cache
    ├── heartbeat_functions.h   # Device status heartbeat
    ├── logger_functions.h      # Decision logs, ML logs, AC state logs
    └── utility_functions.h     # Time helpers, schedule parsing, PIR ISR
```

All function modules are header-only and included from `ESP32-Code.ino`.

---

## Architecture

### Global State (structures.h)

All global objects and state variables are declared in `structures.h` with `extern`, defined in `ESP32-Code.ino`:

- `RoomConfig assignedRoom`: Current room assignment with schedules
- `ScheduleStatus currentScheduleStatus`: Pre-cool, active schedule flags
- `EnergyRuntimeState energyRuntimeState`: Current energy tracking session
- `EnergyDailyCache energyDailyCache`: Daily runtime and kWh accumulator
- `acPowerState`, `acTempState`, `acSourceState`: AC state mirror
- `manualOverrideActive`, `manualOverridePower`, `manualOverrideTemp`: Manual control state
- `forcedOffActive`, `forcedOffWindowKey`: Schedule window lock for forced-off
- `presenceDetected`, `pirMotionDetected`, `mlxPresenceDetected`: Occupancy state
- `streamPendingAction`: Deferred Firebase control stream actions

### Modules

**wifi_functions.h**
- WiFiManager captive portal provisioning
- Saved credentials in ESP32 flash/NVS
- Reset button held 5s clears credentials and restarts
- Reconnect failure escalation (36 failures → restart into provisioning)

**firebase_functions.h**
- Firebase init with auth settle delay
- Room assignment fetch
- AC state sync
- Control stream (polling Firebase Realtime Database for remote commands)
- Manual override, forced-off, AI toggle handling

**schedule_functions.h**
- `evaluateScheduleStatus()`: checks daily schedule, pre-cool window, active window
- `shouldPollSensors()`: returns true during manual/pre-cool/schedule windows

**ac_control.h**
- `applyAcState()`: sends Coolix IR commands, updates global AC state, syncs Firebase, logs
- IR burst repeat (5 times with 200ms delay)
- Temperature normalization (17-30°C)

**sensor_functions.h**
- DHT22 polling (7s interval)
- PIR motion latch with 3-minute hold
- MLX90614 presence detection (consecutive read filtering)
- Occupancy Firebase push
- ML endpoint call for temperature suggestions

**energy_functions.h**
- Runtime session tracking (start/stop based on AC power transitions)
- Daily energy cache (runtimeSeconds, estimatedKwh, sessionCount)
- Periodic flush to Firebase (60s)

**heartbeat_functions.h**
- Periodic status heartbeat (60s interval)
- Writes `lastSeen` timestamp and IP to Firebase

**logger_functions.h**
- Decision event logs (`/decisionLogs`)
- ML suggestion logs
- AC state change logs

**utility_functions.h**
- `onPirMotion()`: PIR interrupt handler
- `normalizeACTemp()`: clamp AC temp to 17-30°C
- `nowIsoString()`, `dateKeyFromTm()`: time formatting
- `timeIsValid()`: NTP time validation
- `sameDay()`, `isWithinMinuteRange()`: schedule matching

---

## Boot Flow

1. Serial, sensors, PIR interrupt, IR transmitter init
2. Load device identity and Firebase credentials from `secrets.h`
3. Run WiFiManager provisioning (captive portal if no saved credentials)
4. Sync NTP time
5. Enter main loop

---

## Main Loop

1. Service WiFi reset button
2. Reconnect WiFi if disconnected (non-blocking)
3. Initialize Firebase if needed (non-blocking, auth settle delay)
4. Load startup state once Firebase ready:
   - Fetch assigned room
   - Load AC state, control state, energy profile, energy state from Firebase
   - Sync AC state to Firebase
   - Initialize energy tracking
5. Attach and poll control stream (Firebase Realtime Database polling)
6. Process deferred stream actions (IR sends stay outside stream parsing)
7. Run minute-based schedule control
8. Check manual override expiry
9. Tick energy tracking (periodic flush)
10. Tick heartbeat
11. Poll sensors if in sensor window (manual/pre-cool/schedule)

---

## Control Priority

Schedule logic follows this priority:

1. **No assigned room**: AC off
2. **Forced off**: AC off for the active schedule window (window-locked)
3. **Manual override**: follow `manualOverridePower` and `manualOverrideTemp`
4. **No schedule today**: AC off
5. **Outside schedule window**: AC off
6. **Pre-cool window**: AC on at `PRECOOL_TEMP` (17°C)
7. **Active schedule**: AC on, occupancy-aware, optional ML target suggestions
8. **Empty room past grace period**: AC off (5 minutes after schedule entry or last presence)

---

## Firebase Paths

| Path | Purpose |
|------|---------|
| `/rooms` | Room assignments and schedules |
| `/devices/{DEVICE_ID}/control` | Manual override, forced-off, AI toggle |
| `/devices/{DEVICE_ID}/acState` | Current AC power, temp, source |
| `/devices/{DEVICE_ID}/status` | Heartbeat (lastSeen, IP) |
| `/devices/{DEVICE_ID}/occupancy` | Presence detected |
| `/devices/{DEVICE_ID}/temperature` | DHT22 temperature |
| `/devices/{DEVICE_ID}/humidity` | DHT22 humidity |
| `/devices/{DEVICE_ID}/energyProfile` | Estimated watts |
| `/devices/{DEVICE_ID}/energyState` | Current runtime session |
| `/devices/{DEVICE_ID}/energyDaily/{date}` | Daily runtime and kWh |
| `/devices/{DEVICE_ID}/mlSuggestion` | ML temperature suggestion |
| `/decisionLogs` | Decision events, mode changes, AC state changes |

---

## Configuration (config/config.h)

**Hardware Pins**
- `DHTPIN`: GPIO13 (DHT22)
- `PIR_PIN`: GPIO14 (PIR motion)
- `IR_LED_PIN`: GPIO12 (IR transmitter)
- `WIFI_RESET_PIN`: GPIO27 (hold low 5s to clear WiFi credentials)

**AC Limits**
- `AC_TEMP_MIN`: 17°C
- `AC_TEMP_MAX`: 30°C
- `PRECOOL_MINUTES`: 10 minutes before schedule start
- `PRECOOL_TEMP`: 17°C
- `IR_SEND_REPEAT_COUNT`: 5 bursts
- `IR_SEND_REPEAT_DELAY_MS`: 200ms between bursts

**Sensor Thresholds**
- `MLX_HUMAN_OBJECT_MIN_C`: 30.0°C
- `MLX_HUMAN_OBJECT_MAX_C`: 40.0°C
- `MLX_HUMAN_DELTA_MIN_C`: 2.0°C (object - ambient)
- `MLX_CONFIRM_READS`: 2 consecutive positive reads
- `MLX_CLEAR_READS`: 2 consecutive negative reads
- `PIR_HOLD_MS`: 3 minutes
- `OCCUPANCY_EMPTY_OFF_MS`: 20 minutes (general occupancy grace)
- `SCHEDULE_NO_OCC_OFF_MS`: 5 minutes (schedule-specific grace)

**Timing**
- `DHT_INTERVAL_MS`: 7s
- `MLX_INTERVAL_MS`: 3s
- `ML_INTERVAL_MS`: 15 minutes (ML endpoint call)
- `WIFI_RECONNECT_MS`: 5s
- `WIFI_MAX_RECONNECT_FAILURES`: 36
- `HEARTBEAT_INTERVAL_MS`: 60s
- `ENERGY_FLUSH_INTERVAL_SEC`: 60s

**WiFiManager**
- `WIFI_PORTAL_SSID`: "OcuTemp-Setup"
- `WIFI_CONNECT_TIMEOUT_SEC`: 20s
- `WIFI_PORTAL_TIMEOUT_SEC`: 300s (5 minutes)

---


---

## Key Structs

**RoomConfig**
```cpp
struct RoomConfig {
  bool found;
  String uid;
  String roomName;
  String device;
  ScheduleSlot schedules[16];
  int scheduleCount;
};
```

**ScheduleSlot**
```cpp
struct ScheduleSlot {
  String day;          // e.g., "monday"
  int startMinute;     // 0-1439
  int endMinute;       // 0-1439
};
```

**ScheduleStatus**
```cpp
struct ScheduleStatus {
  bool hasScheduleToday;
  bool inPreCool;
  bool inSchedule;
  String windowKey;    // room|date|day|start|end
};
```

**StreamPendingAction**
```cpp
struct StreamPendingAction {
  bool hasPending;
  bool power;
  int temp;
  char source[32];
  bool writeForcedOffFalse;
  bool writeForcedOffPersisted;
  bool forcedOffPersistedVal;
  bool writeForcedOffWindowKey;
  char forcedOffWindowKey[96];
};
```

---

## Do

**Code Structure**
- Keep all functions in header files under `functions/`
- Use global state declared in `structures.h`, defined in `ESP32-Code.ino`
- Prefer static helper functions inside header files
- Use descriptive function names and comments for complex logic

**Non-Blocking Operations**
- Never use blocking WiFi reconnects
- Never use blocking Firebase calls in loop
- Use state machines for multi-step operations (e.g., `netAuthState`)

**State Synchronization**
- Always sync AC state to Firebase after changes
- Always log decision events for mode changes and AC state changes
- Always flush energy state periodically

**Sensor Polling**
- Only poll sensors during sensor windows (manual/pre-cool/schedule)
- Use consecutive read filtering for MLX90614 to avoid false positives
- Publish occupancy changes to Firebase

**Time Handling**
- Validate time with `timeIsValid()` before using `struct tm`
- Use NTP for time sync (60s re-sync interval)
- Use ISO 8601 strings for Firebase timestamps
- Use minute-based schedule matching (0-1439)

**Error Handling**
- Check Firebase errors and request reinit on auth/SSL errors
- Log failures with cooldown periods to avoid spam
- Handle missing Firebase fields gracefully with defaults

**Testing**
- Test WiFi provisioning flow (first boot, saved credentials, reset button)
- Test schedule evaluation across day boundaries
- Test occupancy detection with PIR and MLX90614
- Test manual override expiry
- Test forced-off window locking
- Test energy session tracking across power transitions
- Test Firebase control stream parsing

---

## Do NOT

- Hardcode WiFi SSID/password in firmware (use WiFiManager)
- Call `wm.resetSettings()` automatically on WiFi failure (only through reset button)
- Trust client-side AC state (always use `acIrStateTrusted` flag)
- Block the main loop with WiFi or Firebase calls
- Poll sensors continuously when outside sensor windows
- Ignore Firebase control stream errors
- Skip IR burst repeats (AC units need multiple IR signals)
- Allow manual overrides to persist indefinitely (always set expiry)
- Log decision events without cooldown periods
- Use floating-point temperatures in AC commands (always normalize to int)

---

## AI Agent Behavior

**Before Modifying Code**
1. Check existing global state in `structures.h`
2. Search for similar patterns in function modules
3. Verify Firebase paths in use
4. Check timing constants in `config.h`
5. Review control priority order

**When Adding Features**
- Add new functions to appropriate `functions/*.h` file
- Add new global state to `structures.h` and `ESP32-Code.ino`
- Add new Firebase paths to documentation
- Add new decision log events
- Add new configuration constants to `config.h`

**When Debugging**
- Check Serial output for decision event logs
- Verify Firebase control stream parsing
- Check WiFi and Firebase connection state
- Verify time sync (NTP)
- Check occupancy detection logic
- Review energy tracking session state

**Architectural Decisions**
Always explain reasoning when modifying:
- WiFi provisioning flow
- Firebase authentication and reconnect logic
- Schedule evaluation and control priority
- Occupancy detection thresholds
- Energy tracking session logic
- Control stream deferred action handling

---

## Common Patterns

**Non-Blocking Reconnect**
```cpp
void reconnectWiFiNonBlocking() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiLinkUp = true;
    return;
  }
  if (millis() - lastWiFiReconnectAttempt < WIFI_RECONNECT_MS) return;
  lastWiFiReconnectAttempt = millis();
  WiFi.reconnect();
  noteWiFiReconnectFailure();
}
```

**Firebase Error Handling**
```cpp
if (!Firebase.RTDB.setInt(&fbdo, path, value)) {
  String err = fbdo.errorReason();
  if (!isFirebaseTokenPendingError(err) && isFirebaseAuthOrSslError(err)) {
    requestFirebaseReinit(err);
  }
}
```

**Minute-Based Control Gate**
```cpp
int minuteStamp = t.tm_yday * 1440 + t.tm_hour * 60 + t.tm_min;
if (minuteStamp == lastCheckedMinuteStamp) return;
lastCheckedMinuteStamp = minuteStamp;
runMinuteControl(t);
```

**Deferred Stream Action**
```cpp
static void queueAcStreamAction(bool power, int temp, const char* source) {
  streamPendingAction.hasPending = true;
  streamPendingAction.power = power;
  streamPendingAction.temp = temp;
  copyStringToBuffer(String(source), streamPendingAction.source,
                     sizeof(streamPendingAction.source));
}
```

---

## Before Finishing

Always ensure:
- Code compiles without errors or warnings
- Serial output includes decision event logs for debugging
- Firebase paths are correct and documented
- Timing constants are in `config.h`, not hardcoded
- Global state is declared in `structures.h`, defined in `ESP32-Code.ino`
- Function modules remain header-only
- Non-blocking operations stay non-blocking
- AC state changes are logged and synced to Firebase
