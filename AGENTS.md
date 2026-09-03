# AGENTS.md — OcuTemp ESP32 Firmware

## Project Identity

OcuTemp is production-grade ESP32 firmware for room air-conditioner automation. It controls Coolix-compatible AC units through IR commands, integrates with Firebase Realtime Database for remote control and monitoring, evaluates room schedules with occupancy-aware cooling, and optionally requests ML-predicted temperature recommendations.

The device operates as a deployed room controller with WiFi provisioning separate from AC control logic, enabling network changes without reflashing firmware.

Current production baseline: `2026.09.03-recovery3-occupancy3-accounting1`. Build for an ESP32 Dev Module with the `Huge APP (3 MB No OTA/1 MB SPIFFS)` partition scheme. The current image does not fit the default application partition, and the deployed layout does not support OTA.

---

## AI Agent Persona

**Act as**: IoT Senior Engineer

When working with this codebase, assume the role of a senior IoT engineer with expertise in:
- Embedded systems development (ESP32, Arduino framework)
- Production firmware architecture and best practices
- Network resilience and recovery patterns
- State management and persistence in resource-constrained environments
- Real-time control systems with safety-critical considerations
- Flash wear management and long-running device stability

Apply this expertise when reviewing code, making recommendations, debugging issues, or implementing features. Prioritize reliability, safety, maintainability, and production readiness in all decisions.

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
    ├── persistence_functions.h # NVS override/checkpoint state, diagnostic ring
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
- `manualOverrideActive`, `manualOverridePower`, `manualOverrideTargetTemp`, `manualOverrideUntil`: Manual control state
- `forcedOffActive`, `forcedOffWindowKey`: Schedule window lock for forced-off
- `presenceDetected`, `pirMotionDetected`, `mlxPresenceDetected`: Occupancy state
- `lastPresenceReported`, `occupancyPublishPending`, `lastOccupancyPublishAttemptMillis`: Confirmed Firebase occupancy synchronization state
- `firebaseRecoveryCount`, `firebaseSessionRecoveryStreak`, heartbeat/NTP counters: Network recovery diagnostics
- `runtimeBreadcrumb`, `previousResetOperation`, persistent diagnostic history: Reset diagnostics
- `streamPendingAction`: Deferred Firebase control stream actions

### Modules

**wifi_functions.h**
- WiFiManager captive portal provisioning
- Saved credentials in ESP32 flash/NVS
- Reset button held 5s clears credentials and restarts
- Runtime reconnect every 5s; 36 failed attempts trigger a restart into the saved-credential/provisioning boot flow
- Automatic recovery never erases saved credentials

**firebase_functions.h**
- Firebase authentication state machine with WiFi stability wait, bounded timeouts, retry backoff, and device-specific jitter
- Room assignment fetch
- AC state sync
- Control stream (polling Firebase Realtime Database for remote commands)
- Manual override, forced-off, AI toggle handling
- Supervises `Firebase.ready()` loss, rebuilds the complete Firebase session, escalates repeated failures to a WiFi reconnect, and safely restarts after a continuous outage
- NTP validity monitoring and recovery

**schedule_functions.h**
- `evaluateScheduleStatus()`: checks daily schedule, pre-cool window, active window
- `shouldPollSensors()`: returns true during manual/pre-cool/schedule windows

**ac_control.h**
- `applyAcState()`: sends Coolix IR commands, updates global AC state, syncs Firebase, logs
- IR burst repeat (5 times with 200ms delay)
- Temperature normalization (17-30°C)

**sensor_functions.h**
- DHT22 polling (7s interval)
- Unified 5-minute occupancy hold refreshed by direct PIR or confirmed MLX detection
- MLX90614 presence detection with two-positive/two-negative filtering
- Retry-safe occupancy publishing; a value is marked reported only after Firebase confirms the write
- ML endpoint call for temperature suggestions

**energy_functions.h**
- Runtime session tracking (start/stop based on AC power transitions)
- Daily energy cache (runtimeSeconds, estimatedKwh, sessionCount)
- Periodic flush to Firebase (60s)
- Writes an NVS checkpoint of the daily totals whenever a flush write is not confirmed, so a reboot during an outage does not discard accumulated runtime. The checkpoint is retired only after both the `/energyDaily` totals write and the `/energyState` marker write are confirmed
- Merges that checkpoint back once, at startup with verified time, by taking the larger absolute total; it also adopts the checkpoint's `lastFlushAt` so the already-accounted span is not replayed

**heartbeat_functions.h**
- Periodic status heartbeat (60s interval)
- Writes local `lastSeen`, Firebase server timestamp `lastSeenServer`, IP, firmware/reset/network health, and occupancy diagnostics

**persistence_functions.h**
- Persists manual override state in NVS
- Persists the energy checkpoint (absolute daily totals, so a merge is idempotent)
- Persists the occupancy/schedule checkpoint (wall-clock window start and last direct sensor evidence)
- Maintains a six-record diagnostic ring across resets
- Stores an RTC operation breadcrumb around marked blocking network calls
- Uploads saved diagnostics in the next successful heartbeat, then clears the local ring

**logger_functions.h**
- Decision event logs (`/decisionLogs`)
- ML suggestion logs
- AC state change logs
- Bounded RAM ring buffer holding events that could not reach Firebase; entries keep their original event-time `updatedAt` and are replayed after recovery with `bufferedDuringOutage: true`
- Replay is drained a few entries per loop pass, and an overflow beyond the ring capacity is reported once as a `decision_log_overflow` event
- A failed replay write starts a `DECISION_LOG_DRAIN_RETRY_MS` cooldown, so a broken session is never reattempted with a blocking write on the very next loop pass; a successful write clears the cooldown immediately

**utility_functions.h**
- `onPirMotion()`: PIR interrupt handler
- `normalizeACTemp()`: clamp AC temp to 17-30°C
- `nowIsoString()`, `dateKeyFromTm()`: time formatting
- `timeIsValid()`: NTP time validation
- `sameDay()`, `isWithinMinuteRange()`: schedule matching

---

## Boot Flow

1. Initialize Serial and capture the previous RTC reset breadcrumb
2. Restore persisted manual override state
3. Initialize DHT22, PIR interrupt, MLX90614, and Coolix IR state
4. Load device identity and Firebase credentials from `secrets.h`
5. Connect with saved WiFi credentials; open the protected captive portal when required
6. Request NTP synchronization
7. Apply a still-valid persisted manual override before Firebase startup
8. Initialize the 120-second loop-task watchdog (schedule/occupancy warm recovery happens later, on the first minute-control pass, because it needs the room assignment)
9. Enter the main loop; Firebase authentication starts only after WiFi and time are usable

---

## Main Loop

1. Service WiFi reset button
2. Reconnect WiFi if disconnected (non-blocking)
3. Validate/recover NTP and service continuous-outage recovery
4. Advance the Firebase authentication/recovery state machine
5. Load startup state once Firebase ready:
   - Fetch assigned room
   - Load AC state, control state, energy profile, energy state from Firebase
   - Sync AC state to Firebase
   - Initialize energy tracking
6. Attach and poll control stream (Firebase Realtime Database polling)
7. Process deferred stream actions (IR sends stay outside stream parsing)
8. Run minute-based schedule control
9. Check manual override expiry
10. Tick energy tracking (periodic flush)
11. Tick heartbeat
11a. Drain a bounded number of buffered decision logs when Firebase is writable
12. Poll sensors only in a manual/pre-cool/schedule window; otherwise reset and publish idle occupancy false once
13. Reset the loop-task watchdog

---

## Control Priority

Schedule logic follows this priority:

1. **No assigned room**: AC off
2. **Forced off**: AC off for the active schedule window (window-locked)
3. **Manual override**: follow `manualOverridePower` and `manualOverrideTargetTemp`
4. **No schedule today**: AC off
5. **Outside schedule window**: AC off
6. **Pre-cool window**: AC on at `PRECOOL_TEMP` (17°C)
7. **Active schedule**: AC on, occupancy-aware, optional ML target suggestions
8. **Empty room past grace period**: AC off when the unified 5-minute occupancy hold/grace expires; the minute gate may add less than 1 minute

---

## Firebase Paths

| Path | Purpose |
|------|---------|
| `/rooms` | Room assignments and schedules |
| `/devices/{DEVICE_ID}/control` | Manual override, forced-off, AI toggle |
| `/devices/{DEVICE_ID}/acState` | Current AC power, temp, source |
| `/devices/{DEVICE_ID}/status` | Heartbeat, server timestamp, recovery/reset health, and occupancy diagnostics |
| `/devices/{DEVICE_ID}/occupancy` | Presence detected |
| `/devices/{DEVICE_ID}/temperature` | DHT22 temperature |
| `/devices/{DEVICE_ID}/humidity` | DHT22 humidity |
| `/devices/{DEVICE_ID}/energyProfile` | Estimated watts |
| `/devices/{DEVICE_ID}/energyState` | Current runtime session |
| `/devices/{DEVICE_ID}/energyDaily/{date}` | Daily runtime and kWh |
| `/devices/{DEVICE_ID}/mlSuggestion` | ML temperature suggestion |
| `/decisionLogs` | Decision events, mode changes, AC state changes. An entry recorded while Firebase was unreachable carries `bufferedDuringOutage: true` and an `updatedAt` from when the event happened, not when it was uploaded |

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
- `OCCUPANCY_HOLD_MS`: 5 minutes from the latest direct PIR/MLX detection
- `PIR_HOLD_MS`: alias of the unified 5-minute hold for PIR-recent diagnostics
- `SCHEDULE_NO_OCC_OFF_MS`: alias of the unified 5-minute hold; do not stack another grace period
- `OCCUPANCY_EMPTY_OFF_MS`: legacy 20-minute constant; currently not used by active schedule control

**Timing**
- `DHT_INTERVAL_MS`: 7s
- `MLX_INTERVAL_MS`: 3s
- `ML_INTERVAL_MS`: 15 minutes (ML endpoint call)
- `WIFI_RECONNECT_MS`: 5s
- `WIFI_MAX_RECONNECT_FAILURES`: 36
- `HEARTBEAT_INTERVAL_MS`: 60s
- `HEARTBEAT_FAILURE_RETRY_MS`: 10s
- `OCCUPANCY_PUBLISH_RETRY_MS`: 5s while a change is pending
- `OCCUPANCY_SERIAL_DIAGNOSTIC_INTERVAL_MS`: 60s local Serial diagnostic only; it creates no Firebase write or sensor read
- `OCCUPANCY_PERSIST_MIN_INTERVAL_MS`: 60s minimum between NVS writes of direct occupancy evidence (flash wear bound)
- `OCCUPANCY_PERSIST_MAX_AGE_MS`: 12 hours; a checkpoint older than this is never trusted for warm recovery
- `DECISION_LOG_BUFFER_CAPACITY`: 24 buffered decision log entries (RAM only)
- `DECISION_LOG_DRAIN_PER_CYCLE`: 2 replayed entries per loop pass
- `DECISION_LOG_DRAIN_RETRY_MS`: 10s cooldown after a failed replay write; only failures start it, so a healthy backlog still drains at the full per-cycle rate
- `ENERGY_FLUSH_INTERVAL_SEC`: 60s
- `FIREBASE_READY_LOSS_TIMEOUT_MS`: 75s
- `NETWORK_OUTAGE_RESTART_MS`: 10 minutes
- `LOOP_WATCHDOG_TIMEOUT_MS`: 120s
- `NTP_VALID_CHECK_MS`: 30s
- `NTP_RECONFIG_INTERVAL_MS`: 6 hours

**WiFiManager**
- `WIFI_PORTAL_SSID`: "OcuTemp-Setup"
- `WIFI_CONNECT_TIMEOUT_SEC`: 20s
- `WIFI_PORTAL_TIMEOUT_SEC`: 300s (5 minutes)

---

## Occupancy State Model

1. A PIR interrupt/current HIGH or filtered MLX-positive state is direct occupancy evidence.
2. Direct evidence updates `lastPresenceDetectedMillis`.
3. `presenceDetected` stays true for one unified 5-minute hold from the latest direct evidence.
4. New evidence restarts that same timer; no second hold is applied.
5. Active-schedule empty shutdown uses the same 5-minute reference. Once occupancy becomes false, the grace has already elapsed; only the once-per-minute control gate can add up to about 1 minute.
6. Outside all sensor windows, sensor/occupancy state is reset and Firebase idle occupancy false is published once.

`lastPresenceReported` means the last value Firebase actually confirmed, not the last attempted value. If Firebase is unavailable when local occupancy changes, `occupancyPublishPending` remains true and the latest local value is retried no faster than every 5 seconds after Firebase becomes usable. Never set `lastPresenceReported` before a successful RTDB write.

MLX classification requires object temperature from 30-40°C, object-minus-ambient delta of at least 2°C, and two consecutive positive reads. Two consecutive negative/invalid classifications clear `mlxPresenceDetected`; the unified hold prevents UI flicker after it clears.

## Reboot Warm Recovery

The goal is reboot transparency: a device that restarts mid-window should reach the same control decision as one that never restarted. Occupancy and the empty-room grace baseline are therefore anchored to wall clock in NVS, not to `millis()`.

- On a genuine schedule-window entry the device persists the window key and the wall-clock moment it entered.
- Direct PIR/MLX evidence updates a persisted `lastPresenceEpoch`, at most once per `OCCUPANCY_PERSIST_MIN_INTERVAL_MS`.
- On the first minute-control pass after boot, if the persisted window key equals the currently active one, the device reconstructs `scheduleWindowEnteredMillis` from the stored epoch instead of restarting the grace from boot time. If the stored evidence is still inside the 5-minute hold it also restores `presenceDetected` and `lastPresenceDetectedMillis`.
- Restoration is refused when the clock is not verified, when the stored epoch is in the future, when the window key differs, or when the checkpoint is older than `OCCUPANCY_PERSIST_MAX_AGE_MS`. A refusal falls back to the ordinary fresh-entry behaviour.
- A warm recovery can only bring the empty-room shutdown forward or extend an existing hold by at most the remaining 5 minutes. It never powers the AC on by itself, and a reboot no longer grants the room an extra grace period.
- Because a warm recovery can find the grace already spent, the boot-time IR refresh is suppressed when the empty shutdown is due, so the AC is not switched on immediately before being switched off. If a person is then sensed, the next minute pass powers the AC back on normally.

## Heartbeat and Occupancy Diagnostics

The heartbeat writes `/devices/{DEVICE_ID}/status` every 60 seconds using one `setJSON` request. The existing `/devices/{DEVICE_ID}/occupancy` boolean remains the web-app compatibility path. Adding diagnostic fields does not create extra heartbeat requests.

| Field | Meaning |
|------|---------|
| `lastSeen` | ESP local ISO-8601 time |
| `lastSeenServer` | Firebase server-resolved timestamp; use this to distinguish clock problems from failed writes |
| `resetReason` | Current ESP reset category; `1` is power-on, `3` software reset, `4` panic, `6` task watchdog, `9` brownout |
| `firebaseRecoveryCount` | Firebase session rebuilds during the current boot; not a reboot counter |
| `firebaseRecoveryStreak` | Consecutive recovery escalation count; a successful heartbeat clears it |
| `tokenStatus` / `tokenErrorCode` | Current Firebase token lifecycle state and an optional error code |
| `occupancyDiagnostics/presenceDetected` | Final held occupancy value used by the firmware |
| `occupancyDiagnostics/occupancyHoldActive` | Occupancy is true without a currently active PIR pin or MLX classification and is being retained by the 5-minute hold |
| `occupancyDiagnostics/pirMotionDetected` | Current PIR input level |
| `occupancyDiagnostics/pirRecentMotion` | PIR edge occurred within the 5-minute hold |
| `occupancyDiagnostics/mlxPresenceDetected` | Filtered MLX human-like heat classification |
| `occupancyDiagnostics/mlxReadingValid` | Object, ambient, and delta readings are all finite |
| `occupancyDiagnostics/mlxObjectTemp` | Latest MLX object temperature in °C; null when invalid/reset |
| `occupancyDiagnostics/mlxAmbientTemp` | Latest MLX ambient temperature in °C; null when invalid/reset |
| `occupancyDiagnostics/mlxDeltaTemp` | Object minus ambient in °C; null when invalid/reset |
| `occupancyDiagnostics/lastPresenceAgeMs` | Milliseconds since the latest direct PIR/MLX evidence; null when none exists in the current sensor window |
| `occupancyDiagnostics/occupancyPublishPending` | Local `/occupancy` differs from the last confirmed Firebase value or a write still needs retrying |

Interpretation rules:

- `presenceDetected=true` with `occupancyHoldActive=true` is expected during the five-minute hold.
- Persistently `pirMotionDetected=true` in an empty room suggests a stuck/noisy PIR input.
- Repeatedly small `lastPresenceAgeMs` in an empty room suggests repeated PIR noise or an MLX false positive.
- `mlxPresenceDetected=true` in an empty room means a warm object satisfies the configured range/delta; inspect placement and temperatures before changing thresholds.
- Local `presenceDetected=false`, root `/occupancy=true`, and `occupancyPublishPending=true` means Firebase is temporarily stale and awaiting retry.

## Network and Reset Recovery

- WiFi reconnect attempts occur every 5 seconds. After 36 failures, the ESP restarts into the normal saved-credential/WiFiManager flow; credentials are not erased.
- Firebase waits 5 seconds after WiFi stability, initializes with bounded socket/TLS/response timeouts, and allows a 20-second ready settle period.
- Three Firebase data/stream failures request a full Firebase session rebuild. Backoff grows from 5 to 60 seconds and includes device-specific jitter.
- If `Firebase.ready()` remains false for 75 seconds after being ready, the session is rebuilt.
- Three consecutive Firebase session recoveries force a WiFi disconnect/reconnect without erasing credentials; this escalation has a 2-minute cooldown.
- A successful heartbeat clears the continuous-outage timer and recovery streak. A continuous 10-minute outage persists override/diagnostic state and performs a controlled restart.
- NTP validity is checked every 30 seconds. Invalid time is retried every 30 seconds; five failures force WiFi recovery. Normal `configTime()` reconfiguration is limited to every 6 hours or WiFi restoration.
- The loop task is watched with a 120-second watchdog. Marked synchronous network operations leave an RTC breadcrumb that can identify the operation after a watchdog/software reset.

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
- Do not add unbounded network calls. The legacy Firebase client is synchronous, so keep its socket/TLS/response timeouts, RTC breadcrumbs, retry limits, and watchdog coverage intact
- Use state machines for multi-step operations (e.g., `netAuthState`)

**State Synchronization**
- Always sync AC state to Firebase after changes
- Always log decision events for mode changes and AC state changes
- Always flush energy state periodically
- Treat `lastPresenceReported` as confirmed remote state; preserve `occupancyPublishPending` across failed/unavailable writes

**Sensor Polling**
- Only poll sensors during sensor windows (manual/pre-cool/schedule)
- Use consecutive read filtering for MLX90614 to avoid false positives
- Publish occupancy changes to Firebase
- Keep `OCCUPANCY_HOLD_MS`, `PIR_HOLD_MS`, and `SCHEDULE_NO_OCC_OFF_MS` aligned unless a deliberate product decision requires different UI and AC timings
- Update `lastPresenceDetectedMillis` only from direct PIR/MLX evidence, never from an already-held occupancy value; otherwise the hold doubles itself
- Persist occupancy evidence only from that same direct evidence, and keep the NVS write throttled
- Compare occupancy and grace references as elapsed ages, never as raw `millis()` magnitudes; a restored reference is deliberately offset and magnitude comparisons break across it

**Time Handling**
- Validate time with `timeIsValid()` before using `struct tm`
- Check NTP validity every 30s and limit ordinary `configTime()` reconfiguration to every 6 hours or WiFi restoration
- Use ISO 8601 strings for Firebase timestamps
- Use Firebase server timestamps for remote freshness/ordering when local clock validity is uncertain
- Use minute-based schedule matching (0-1439)

**Error Handling**
- Check Firebase errors and request reinit on auth/SSL errors
- Log failures with cooldown periods to avoid spam
- Handle missing Firebase fields gracefully with defaults

**Testing**
- Test WiFi provisioning flow (first boot, saved credentials, reset button)
- Test schedule evaluation across day boundaries
- Test occupancy with PIR-only, MLX-only, combined detection, sensor clearing, and the exact 5-minute hold boundary
- Test that a new direct detection restarts the hold but an already-held value does not
- Test an occupancy change while Firebase is unavailable, then verify the latest value publishes after recovery and `occupancyPublishPending` clears
- Verify `/devices/{DEVICE_ID}/occupancy` remains compatible and `status/occupancyDiagnostics` matches the local source flags
- Test manual override expiry
- Test forced-off window locking
- Test energy session tracking across power transitions
- Test Firebase control stream parsing
- Test token refresh/network loss for longer than 75s and verify session recovery without erasing WiFi credentials

---

## Do NOT

- Hardcode WiFi SSID/password in firmware (use WiFiManager)
- Print, paste, commit, or document values from `config/secrets.h`
- Read `config/secrets.h` at all - not to print it, not to parse it, not to load it into memory for any purpose. Treat the file as off limits unless the user explicitly asks for something that requires it
- Authenticate to Firebase, the ML endpoint, or any other remote service using project credentials unless the user explicitly asks for that in the request at hand. Never sign in to inspect, verify, or debug on your own initiative
- Query the deployed database to verify behavior. Verification happens on the device, only when the user says to run it, using Serial output and observed device behavior as the evidence - never by reading or writing the backend with credentials taken from the project
- Call `wm.resetSettings()` automatically on WiFi failure (only through reset button)
- Trust client-side AC state (always use `acIrStateTrusted` flag)
- Block the main loop with WiFi or Firebase calls
- Poll sensors continuously when outside sensor windows
- Ignore Firebase control stream errors
- Mark occupancy as reported before Firebase confirms the write
- Restore occupancy, the grace baseline, or energy totals while `timeIsValid()` is false; an unverified clock must never resurrect state
- Replay a whole buffered decision log backlog in one loop pass; the Firebase client is synchronous and the watchdog is 120s
- Retry any blocking Firebase write on every loop pass without an interval guard; `Firebase.ready()` reports token state, not the health of the socket a write just died on
- Decide durability from a reachability probe. Clearing a checkpoint, retiring local state, or marking something synced may only follow a **confirmed write result**; `Firebase.ready()` and `WiFi.status()` both read true while a stale token fails every write
- Store energy checkpoints as increments; totals are absolute so a repeated merge stays idempotent
- Refresh `lastPresenceDetectedMillis` from `presenceDetected` or another held/derived value
- Add a second empty-room grace after the unified occupancy hold without explicitly changing the product behavior
- Skip IR burst repeats (AC units need multiple IR signals)
- Allow manual overrides to persist indefinitely (always set expiry)
- Log decision events without cooldown periods
- Use floating-point temperatures in AC commands (always normalize to int)

---

## Known Constraints and Diagnostic Caveats

- `Firebase-ESP-Client` is deprecated but remains the production dependency. Do not migrate libraries as an incidental fix: the maintained `FirebaseClient` API is asynchronous, requires frequent `FirebaseApp::loop()`, and needs a planned stream/auth/callback rewrite while preserving deferred IR actions and control priority.
- Firebase RTDB calls in the current library are synchronous. Existing timeouts, the 120-second watchdog, and `markRuntimeOperation()` breadcrumbs reduce risk but do not make the calls asynchronous.
- Initial setup NTP synchronization still uses repeated default-timeout `getLocalTime()` calls and can delay boot by roughly 115 seconds in the worst case. The watchdog is initialized afterward. Runtime NTP recovery is bounded/non-blocking.
- WiFiManager `autoConnect()` and the captive portal are intentionally blocking during boot/provisioning. The portal times out after 5 minutes and the ESP restarts; saved credentials are not erased.
- Status is written with `setJSON`, so optional reset/token/diagnostic fields may disappear on a later heartbeat. Persistent diagnostic-ring entries and previous-reset breadcrumbs are uploaded once and then cleared locally after a successful heartbeat.
- `bootCount` is RTC-retained across soft/watchdog resets but is lost on complete power loss. `resetReason=1` means a power-on reset and cannot identify the interrupted operation because RTC breadcrumbs may also be lost.
- Heartbeat freshness proves Firebase communication, not that every local control path was recently executed. Conversely, web “offline” means heartbeat staleness and does not by itself prove that AC schedule control stopped.
- The current partition is Huge APP with no OTA. Changing the partition scheme can erase or relocate flash data and must be tested separately; do not enable OTA casually on deployed devices.
- The buffered decision log ring is RAM only. It survives an outage but not a power cycle inside that outage; the reboot-durable evidence of an outage is the `network_outage_start` / `network_outage_recovered` pair in the persistent diagnostic ring.
- When the ring overflows, the oldest entries are dropped and the total is reported once as a `decision_log_overflow` event after the backlog drains.
- An energy checkpoint whose `dateKey` is not today is discarded rather than merged, because writing it into the wrong daily bucket would be worse than the small under-count. A reboot that crosses midnight during an outage can therefore lose the previous day's unsynced tail.
- Warm recovery depends on the schedule window key, which embeds the calendar date, so it cannot resurrect state across days even before the 12-hour age cap applies.

---

## AI Agent Behavior

**Before Modifying Code**
1. Inspect `git status` and preserve all pre-existing user changes
2. Check existing global state in `structures.h` and definitions in `ESP32-Code.ino`
3. Search for similar patterns and every caller/consumer before changing shared state
4. Verify Firebase paths and database write semantics in use
5. Check timing constants in `config.h`, including aliases that intentionally share one duration
6. Review control priority and deferred stream-action behavior
7. Treat source code as authoritative if this document and implementation ever disagree; update this document in the same change

**When Adding Features**
- Add new functions to appropriate `functions/*.h` file
- Add new global state to `structures.h` and `ESP32-Code.ino`
- Add new Firebase paths to documentation
- Add new decision log events
- Add new configuration constants to `config.h`
- Preserve existing Firebase paths used by the web app; add diagnostics under `/status` unless a product/API change is explicitly requested

**When Debugging**
- Check Serial output for decision event logs
- Verify Firebase control stream parsing
- Distinguish power loss, watchdog/panic, software restart, WiFi loss, Firebase readiness loss, permission failure, and stale heartbeat before assigning a cause
- Check WiFi, Firebase token/readiness, heartbeat age, reset reason, recovery counters, and persistent breadcrumbs together
- Verify local and server timestamps before diagnosing time sync
- For occupancy, compare root `/occupancy`, all `occupancyDiagnostics` source flags, `lastPresenceAgeMs`, and `occupancyPublishPending`
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
- Firmware changes compile without errors or source warnings using `esp32:esp32:esp32:PartitionScheme=huge_app`; the default partition is too small
- Record the final flash/RAM utilization. The last verified `occupancy2` build used 1,610,651 bytes flash (51%) and 57,712 bytes global RAM (17%)
- Serial output includes decision event logs for debugging
- Firebase paths are correct and documented
- Timing constants are in `config.h`, not hardcoded
- Global state is declared in `structures.h`, defined in `ESP32-Code.ino`
- Function modules remain header-only
- Non-blocking operations stay non-blocking
- AC state changes are logged and synced to Firebase
- Existing web-app Firebase paths and control priority remain compatible
- No firmware upload, credential erase, partition change, or OTA action occurs unless the user explicitly requests it
