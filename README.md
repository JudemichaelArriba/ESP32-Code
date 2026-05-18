# OcuTemp ESP32 Firmware

Production-oriented ESP32 firmware for room air-conditioner automation. The device controls a Coolix-compatible AC unit using IR commands, Firebase Realtime Database state, room schedules, occupancy sensing, and optional ML-assisted temperature recommendations.

## Overview

OcuTemp is designed to run as a deployed room controller. It keeps WiFi provisioning separate from AC control so the device can be moved to a new network without reflashing firmware.

Core responsibilities:

- Provision WiFi through WiFiManager captive portal/AP mode.
- Connect to Firebase for room assignment, schedules, manual control, AC state, logs, heartbeat, and sensor data.
- Evaluate schedule windows and pre-cool periods.
- Detect occupancy with PIR and MLX90614 sensors.
- Control the AC through IR Coolix commands.
- Track estimated AC runtime and energy usage.
- Send optional ML requests to a Render endpoint for target temperature suggestions.

## Hardware

- ESP32 development board or compatible ESP32 module
- DHT22 temperature/humidity sensor
- PIR motion sensor
- Adafruit MLX90614 IR temperature sensor
- IR LED/transmitter for Coolix AC control
- Optional WiFi reset/provisioning button

Default pins are configured in `config/config.h`:

- `DHTPIN`: GPIO13
- `PIR_PIN`: GPIO14
- `IR_LED_PIN`: GPIO12
- `WIFI_RESET_PIN`: GPIO27, held low for 5 seconds to clear saved WiFi credentials

## Required Libraries

Install these through Arduino IDE Library Manager or your existing Arduino libraries folder:

- WiFiManager by tzapu, version `2.0.17`
- Firebase ESP Client
- ArduinoJson
- DHT sensor library
- Adafruit MLX90614
- Adafruit BusIO
- IRremoteESP8266

## WiFi Provisioning

The firmware no longer connects using hardcoded router credentials.

On boot:

1. The ESP32 tries saved WiFi credentials from flash/NVS using WiFiManager.
2. If no saved credentials exist, or the saved network cannot connect, it opens a setup hotspot.
3. The user connects to the hotspot and enters WiFi credentials in the WiFiManager captive portal.
4. WiFiManager saves the credentials to ESP32 flash/NVS.
5. The device connects to WiFi, syncs time, and continues the normal Firebase startup flow.

Current setup hotspot:

- SSID: `OcuTemp-Setup`

Runtime reconnect behavior:

- The device retries WiFi every `WIFI_RECONNECT_MS`.
- After `WIFI_MAX_RECONNECT_FAILURES`, it restarts into the provisioning flow.
- Saved WiFi credentials are not erased automatically.

Manual WiFi reset:

- Hold `WIFI_RESET_PIN` low for `WIFI_RESET_HOLD_MS`.
- The firmware calls `wm.resetSettings()`.
- The ESP32 restarts and opens the setup portal on the next boot.

## Configuration

`config/config.h` contains firmware settings that are safe to review during development:

- GPIO pins
- sensor thresholds
- schedule/pre-cool timing
- WiFiManager portal timing
- reconnect retry limits
- NTP settings
- AC temperature limits
- heartbeat and energy intervals

Runtime WiFi router credentials are managed by WiFiManager in ESP32 flash/NVS, not by hardcoded firmware constants.

## Project Structure

```text
ESP32-Code/
|- ESP32-Code.ino
|- README.md
|- config/
|  |- config.h
|  `- secrets.h
|- core/
|  `- structures.h
`- functions/
   |- ac_control.h
   |- energy_functions.h
   |- firebase_functions.h
   |- heartbeat_functions.h
   |- logger_functions.h
   |- schedule_functions.h
   |- sensor_functions.h
   |- utility_functions.h
   `- wifi_functions.h
```

## Main Modules

- `ESP32-Code.ino`: Main sketch, global state, hardware setup, boot flow, and main loop.
- `wifi_functions.h`: WiFiManager provisioning, captive portal setup, WiFi reset button, and reconnect failure escalation.
- `firebase_functions.h`: Firebase initialization, room fetch, AC/control state sync, and control stream handling.
- `schedule_functions.h`: Pre-cool and active schedule evaluation.
- `ac_control.h`: Coolix IR command generation and AC state application.
- `sensor_functions.h`: DHT22, PIR, MLX90614, occupancy publishing, and ML endpoint calls.
- `energy_functions.h`: Runtime session tracking and estimated energy usage.
- `heartbeat_functions.h`: Device status heartbeat updates.
- `logger_functions.h`: Decision, ML, and AC state logs.
- `utility_functions.h`: Time helpers, schedule parsing, PIR ISR, and AC temperature normalization.
- `structures.h`: Shared includes, structs, and global extern declarations.

## Runtime Flow

1. Initialize serial, sensors, PIR interrupt, and IR transmitter.
2. Configure device identity and backend service access.
3. Run WiFiManager provisioning through `setupWiFiProvisioning()`.
4. Sync NTP time.
5. In the main loop:
   - service the WiFi reset button
   - keep WiFi and Firebase connected
   - load startup room/control/energy state once Firebase is ready
   - attach and poll the Firebase control stream
   - run minute-based schedule control
   - expire manual overrides when needed
   - track energy runtime
   - send heartbeat
   - poll sensors only during manual, pre-cool, or schedule windows

## Control Behavior

Schedule logic follows this priority:

1. No assigned room: AC off
2. Forced off: AC off for the active schedule window
3. Manual override: follow requested power and target temperature
4. No schedule today: AC off
5. Outside schedule window: AC off
6. Pre-cool window: AC on at `PRECOOL_TEMP`
7. Active schedule: AC on, occupancy-aware, with optional ML target suggestions
8. Empty room past grace period: AC off

## Firebase Paths

The firmware reads or writes these main paths:

- `/rooms`
- `/devices/{DEVICE_ID}/control`
- `/devices/{DEVICE_ID}/acState`
- `/devices/{DEVICE_ID}/status`
- `/devices/{DEVICE_ID}/occupancy`
- `/devices/{DEVICE_ID}/temperature`
- `/devices/{DEVICE_ID}/humidity`
- `/devices/{DEVICE_ID}/energyProfile`
- `/devices/{DEVICE_ID}/energyState`
- `/devices/{DEVICE_ID}/energyDaily/{date}`
- `/devices/{DEVICE_ID}/mlSuggestion`
- `/decisionLogs`

## Production Notes

- The setup hotspot should only be used during provisioning or recovery.
- Do not call `wm.resetSettings()` automatically during normal WiFi failure; the firmware only clears credentials through the reset button path.
- Existing schedule, manual override, Firebase, sensor, and AC control flows are gated by WiFi/Firebase readiness checks.
- All function modules are header-only and are compiled through includes from `ESP32-Code.ino`.
