# ESP32-Code

ESP32 firmware for room AC automation using:
- DHT22 (temperature/humidity)
- PIR + MLX90614 (occupancy detection)
- Firebase Realtime Database (state, schedules, remote control)
- IR Coolix transmitter (AC commands)
- Optional Render ML endpoint (target temperature recommendation)

## Project Purpose
This project controls an air conditioner based on:
1. Room schedule from Firebase
2. Presence detection (PIR + IR temperature sensing)
3. Manual override from Firebase
4. ML-assisted temperature recommendation during active schedule windows

## Folder Structure

```text
ESP32-Code/
|- ESP32-Code.ino
|- README.md
|- .gitignore
|- .gitattributes
|- config/
|  |- config.h
|  `- secrets.h
|- core/
|  `- structures.h
`- functions/
   |- utility_functions.h
   |- firebase_functions.h
   |- ac_control.h
   |- schedule_functions.h
   `- sensor_functions.h
```

## What Each File Does

### Root
- `ESP32-Code.ino`: Main sketch. Initializes hardware/services and runs the control loop.
- `README.md`: Project documentation.
- `.gitignore`: Ignores `config/secrets.h` so credentials are not committed.
- `.gitattributes`: Git text normalization (`* text=auto`).

### `config/`
- `config.h`: Hardware pins, timing intervals, AC limits, pre-cool timing, occupancy thresholds, NTP settings.
- `secrets.h`: Sensitive values (Wi-Fi, Firebase API/auth, Render endpoint, `DEVICE_ID`).

### `core/`
- `structures.h`: Shared includes, structs, and global extern declarations.
  - Defines:
    - `ScheduleSlot`: day + start/end minutes
    - `RoomConfig`: assigned room + schedule list
    - `ScheduleStatus`: schedule flags (`hasScheduleToday`, `inPreCool`, `inSchedule`)

### `functions/`
- `utility_functions.h`:
  - PIR ISR (`onPirMotion`)
  - AC temp normalization
  - Time/date helpers
  - Schedule parsing and day/range matching

- `firebase_functions.h`:
  - Wi-Fi reconnect + Firebase init
  - Fetch assigned room from `/rooms`
  - Load/sync AC state to `/devices/{DEVICE_ID}/acState`
  - Load and stream manual control from `/devices/{DEVICE_ID}/control`

- `ac_control.h`:
  - Sends IR commands via Coolix protocol
  - Applies power/temp state with source tracking (`schedule`, `manual`, `ml`, etc.)
  - Parses control JSON and enforces manual override behavior

- `schedule_functions.h`:
  - Evaluates if current time is pre-cool or active schedule
  - Decides whether sensors should be polled

- `sensor_functions.h`:
  - Reads DHT22 and MLX90614 on intervals
  - Combines PIR + MLX signals into presence state
  - Pushes occupancy/temp/humidity to Firebase
  - Calls Render ML endpoint and extracts `recommended_ac`

## Runtime Control Flow
1. Boot: initialize serial, sensors, IR, Wi-Fi, NTP.
2. Loop:
   - Keep Wi-Fi/Firebase connected and stream attached.
   - Load startup state once (room assignment, AC/control state).
   - Poll sensors only during manual/pre-cool/schedule windows.
   - On each minute tick, run schedule control logic:
     - No room or no schedule today => AC off
     - Outside window => AC off
     - Manual override => follow manual power/temp
     - Pre-cool => AC on at fixed pre-cool temp
     - In schedule with occupancy => AC on and periodically adjust using ML
     - Empty too long => AC off

## Firebase Paths Used
- `/rooms`
- `/devices/{DEVICE_ID}/control`
- `/devices/{DEVICE_ID}/acState`
- `/devices/{DEVICE_ID}/occupancy`
- `/devices/{DEVICE_ID}/temperature`
- `/devices/{DEVICE_ID}/humidity`

## Notes
- All function modules are header-only; logic is compiled through includes from the main `.ino`.
