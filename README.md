# JUPY Security Lock — Firmware

This repository contains the ESP32 firmware for a Facial Recognition door lock which integrates with a K230D vision module and the JUPY Locks React Native app (separate repo). The firmware manages local PIN entry, face unlock via the K230D, notifications via Firebase Cloud Messaging (FCM), and remote control via MQTT.

**Key Features**
- Face unlock integration: wakes and reads K230D UART JSON responses.
- Local passcode entry via a small TFT touchscreen keypad.
- Doorbell and remote unlock using MQTT and FCM notifications.
- On-device settings via a local REST API and secure pin-based auth.
- Battery monitoring and power-saving modes for long-life operation.

**Hardware & Pins**
- Hardware Schematic at [JUPY Lock Firmware.kicad_sch](./jupy_lock_schematic.kicad_sch)
- ESP32 (WROOM-series)
- K230D power: `K230D_PWR_PIN` 
- PIR motion sensor: `PIR_PIN`
- Lock (solenoid) control: `LOCK_PIN`
- Battery ADC: `BATTERY_PIN`
- TFT / Touch pins: configured for included `TFT_eSPI` usage; see `src/main.cpp` for mapping (`TFT_MISO`, `TFT_MOSI`, `TFT_SCLK`, `TFT_CS`, `TFT_DC`).

Pin constants are defined at the top of `src/main.cpp`.

Dependencies (PlatformIO)
- Arduino Framework for ESP32
- WiFi, WiFiClientSecure
- PubSubClient (MQTT)
- ArduinoJson
- Preferences (for persistent settings)
- TFT_eSPI (display + touch)

### Setup

**Build and Flash**
1. Install PlatformIO in VS Code.
2. Open this project.
3. Configure `platformio.ini` for your board and upload.

**Configure TFT_eSPI**

Navigate to the dependency folder .pio/[board]/User_Setup.h uncomment your board and edit the following; optionally in User_Setup_Select.h select your board
``` c++
// Example for ESP32 C5 Setup
#define TFT_MISO 19
#define TFT_MOSI 23
#define TFT_SCLK 18
#define TFT_CS   15  // Chip select control pin
#define TFT_DC    2  // Data Command control pin
// #define TFT_RST   4  // Reset pin (could connect to RST pin)

#define TFT_RST  -1  // Set TFT_RST to -1 if display RESET is connected to ESP32 board RST

#define TOUCH_CS 10 //Enable touch
```

### Operation

**Configuration & Provisioning**
- Settings and secrets are stored in the ESP32 `Preferences` namespace (`my-settings`).
- On first boot (no saved Wi‑Fi), a BLE / Matter-style provisioning payload is expected to supply `wifi-ssid`, `wifi-pwd`, `user-id`, `lock-name`, and `owner-name`. In the example code a placeholder payload is used — replace with your BLE/Matter provisioning flow.
- Set your FCM server key in the `fcm_key` constant to enable push notifications.

**Network & Cloud Interfaces**
- MQTT broker: default `broker.hivemq.com` (change in `src/main.cpp`).
- MQTT topics used:
	- Subscribe: `lock/commands/<USER_ID>` — receives JSON commands (e.g. `{ "cmd": "unlock" }`).
	- Publish (logs): `lock/logs/<USER_ID>` (when MQTT session active).
- FCM notifications: posts to `fcm.googleapis.com` using the configured server key. Payloads send notifications to topic `/topics/<USER_ID>/all`.

**Local REST API (HTTP on ESP32)**
- `POST /unlock` — Body: JSON { "pin": "1234", "name": "Caller" }. Verifies stored PIN and pulses the lock.
- `PATCH /update-settings` — Body: JSON with `name`, `pin`, and `settings` object. Requires auth with correct owner name + PIN. Settings include: `vid-quality`,  `call-timeout`, `snippet-time`, `share-analytics`.

**Behavior Notes**
- K230D wake: PIR or remote commands call `wakeK230D()` which toggles the K230D power pin and logs activity. K230D is auto-powered down after ~3s of no face detection (configurable in code).
- Initailization: BLE server for wifi commissioning and lock setup 
- Auth lockout: 3 failed auth attempts set an authorization timeout (`AUTH_DISABLE_TIME`) — after that period authFail resets.
- Intruder handling: repeated unknown-face detections increment an `intruder` counter and can cause a longer timeout.
- Battery monitoring: periodic ADC reads map to battery percentages and trigger FCM notifications for low battery states.

**Power Saving**
- Wi‑Fi modem sleep is enabled via `esp_wifi_set_ps(WIFI_PS_MIN_MODEM)` and station listen interval is adjusted to reduce power consumption.
- BLE is disabled after provisioning to save power.
- MQTT is disable during inactivity and re-enables after timeout 

**Customizing & Extending**
- Replace the placeholder provisioning with Matter or your BLE service to provision Wi‑Fi and owner details.
- Replace `fcm_key` and `mqtt_server` with your production credentials.
- Adjust pin constants and voltage divider math in `monitorBattery()` to match your hardware.

**Security and Privacy**
- Use external rest api to trigger fcm to avoid committing `fcm_key` to public repos.
- Ensure MQTT broker is configured with TLS/auth if used in production.
- Consider setting up HTTPS/TLS 3.1 for local rest api in production.
- Use cryptographic keys and methods for authorizing commands and changing settings in production.

### TODO
- Use a rest api for fcm to avoid to avoid storring `fcm key` on device.
- Awakes from Deep Sleep with radios turned off to Modem Sleep every 5 minutes (run commands and checks then goes back to deep sleep)
- Assign deep sleep wake up triggers
- Update schematics and pin declarations to use ESP32 S3 instead of the ESP32 C5 which I was going to use at first but will change due to board unavailability and unsuccessful compilation in Arduino IDE 2, ESP32C5 is currently very new.
- Configure and test FCM and remote MQTT commands
- Encypt ble server uuid and name for qr code scan
- Improve provision security with /api/auth check after wifi connected and setup code for secure pairing for secure pairing preventing passerby connection during advertising
