#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
// #include <Matter.h>
// #include <MatterEndPoint.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <esp_wifi.h>

#include "ble_server.h"
#include "esp_bt.h"

// --- Pins (As specified) ---
#define PIR_PIN 26
#define LOCK_PIN 24
#define K230D_PWR_PIN 23
#define BATTERY_PIN 0
#define BUTTON_PIN 25
#define T_IRQ 3  // Touch Interupt

// Display PINS initialized in User_Setup.h of TFT_eSPI Library

// --- Configuration & Credentials ---
const char *fcm_server = "fcm.googleapis.com";
const char *fcm_key = "YOUR_FCM_SERVER_KEY";  // Legacy Key or OAuth2 Relay
const char *mqtt_server = "broker.hivemq.com";
const char *laptop_ip = "192.168.50.163";
const char *cloud_endpoint = ("http://" + String(laptop_ip) + ":3000/api/lock").c_str();

#define LOCK_ID "c0ffee00-1234-4abc-9def-9876543210aa"  // Unique Lock Identifier UUIDv7
#define LOCK_MODEL "JUPY Block Pro"                     // Lock Model
#define FIRMWARE_VERSION "v1.0"                         // Firmware version
#define PAIRING_CODE "A1B2C3"                           // Lock Pairing Code
#define AUTH_DISABLE_TIME 30 * 60000UL                  // 30 minutes
#define COMMISSION_TIME 10 * 60000UL                    // 10 minutes
#define MQTT_ACTIVE_TIMEOUT 2 * 60000UL                 // 2 minutes
#define K230D_MAX_UPTIME 3000UL                         // 3 seconds

// --- Instances ---
TFT_eSPI tft = TFT_eSPI();
// XPT2046_Touchscreen ts(TOUCH_CS);
WiFiClient espClient;
PubSubClient mqttClient(espClient);
WebServer localServer(80);
// MatterDoorLock doorLock;
Preferences prefs;
BLECommissioningServer bleServer;

// --- Stored Variables ---
String LOCK_NAME = "";
String OWNER_NAME = "";
String USER_ID = "";

// --- State Management ---
bool mqttActive = false;
unsigned long authTimeout = 0;
unsigned long bootTime = 0;
unsigned long commissionTimeout = 0;
unsigned long faceUnlockTimeout = 0;
unsigned long lastActivity = 0;
unsigned long k230StartTime = 0;
unsigned long k230UpTime = 0;
unsigned long lastBatCheck = 0;

bool k230IsRunning = false;
bool pinManuallyEntered = false;
bool share_analytics = false;
bool notify_motion = false;

uint8_t intruder = 0;
uint8_t authFail = 0;
String passcodeBuffer = "";

// Function Prototypes
void handlePIR();
void handleUART();
void handleTouch();
void handleTimeouts();
void monitorBattery();
void initialCommisioning();
void wakeK230D(String command = "{\"cmd\":\"on\"}");

bool checkPin(const char *);
void unlockDoor(String);
void drawKeypad();
void FCM_Notification(String, String);
void setupREST();
void serverLog(String);
void mqttCallback(char *, byte *, unsigned int);
void reconnectMQTT();
void endMQTTSession();

struct HTTPResponse {
  int code;
  const char *contentType;
  String body;
};

void wakeUpReason() {
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1) {
    Serial.println("Woke up from PIR or Button!");
  } else if (wakeup_reason == ESP_SLEEP_WAKEUP_TOUCHPAD) {
    Serial.println("Woke up from Touch!");
  } else if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
    Serial.println("Woke up from Timer!");
  } else {
    Serial.println("Natural first wakeup. Not waking from deep sleep");
  }
}

void setup() {
  Serial.begin(115200);

  wakeUpReason();
  pinMode(PIR_PIN, INPUT);
  pinMode(LOCK_PIN, OUTPUT);
  pinMode(K230D_PWR_PIN, OUTPUT);
  pinMode(BATTERY_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT);

  digitalWrite(LOCK_PIN, HIGH);      // Fail-secure: HIGH usually keeps locked
  digitalWrite(K230D_PWR_PIN, LOW);  // K230D off by default

  tft.init();
  tft.setRotation(1);
  drawKeypad();

  // 0. Initialize Storage
  prefs.begin("my_storage", false);

  // 1. Start BLE Server for commissioning
  prefs.putString("pairing_code", PAIRING_CODE);
  bleServer.begin("JUPY Lock Pro");

  // 2. Matter/BLE Provisioning & Transition
  Serial.println("Check for commsioning");
  initialCommisioning();

  // 3. Local REST API
  Serial.println("Setup Rest Server");
  setupREST();

  mqttClient.setServer(mqtt_server, 1883);
  mqttClient.setCallback(mqttCallback);
}

void loop() {
  if (digitalRead(BUTTON_PIN)) {
    unlockDoor("Manual");
  }

  handlePIR();
  handleUART();
  handleTouch();
  monitorBattery();
  localServer.handleClient();

  if (mqttActive) {
    if (!mqttClient.connected()) reconnectMQTT();
    mqttClient.loop();
  }

  handleTimeouts();
}

// --- CORE LOGIC FUNCTIONS ---

void handlePIR() {
  if (digitalRead(PIR_PIN) == HIGH && !k230IsRunning) {
    delay(50);  // Debounce
    if (notify_motion) FCM_Notification("Motion Detected", "Waking up Vision System...");
    wakeK230D();
  }
}

void wakeK230D(String command) {
  digitalWrite(K230D_PWR_PIN, HIGH);
  if (faceUnlockTimeout) {
    command.replace("}", ", \"face_timeout\": true }");
    // Disable camera on start up and skip face recog code,
    // but if doorbell request then enable camera on K230D side
  }
  Serial.println(command);
  k230StartTime = millis();
  k230IsRunning = true;
}

void K230DPowerOff() {
  digitalWrite(K230D_PWR_PIN, LOW);
  k230IsRunning = false;
  serverLog("{\"event\": \"power_off\", \"uptime\": \"" + String(k230UpTime / 1000) + "\"}");
  k230UpTime = 0;
  Serial.println("K230D Powered Off.");
}

void startDeepSleep(unsigned long milli_sec = 0) {
  // Shutdown WiFi
  esp_wifi_stop();
  esp_wifi_deinit();

  // Shutdown Bluetooth
  btStop();
  esp_bt_controller_disable();
  esp_bt_controller_deinit();

  Serial.println("Radios gracefully shut down.");

  // 1. Enable EXT1 Wakeup (PIR on Pin 7, Button on Pin 0)
  // Note: All pins in this call must share the same level (e.g., HIGH)
  // If Button is pull-up (LOW when pressed), it needs an inverter to work with PIR (HIGH)
  uint64_t pin_mask = (1ULL << PIR_PIN) | (1ULL << BUTTON_PIN);
  esp_sleep_enable_ext1_wakeup(pin_mask, ESP_EXT1_WAKEUP_ANY_HIGH);

  // 2. Enable Touch Wakeup using touch interrupt pin
  esp_sleep_enable_ext0_wakeup((gpio_num_t)T_IRQ, 0);  // 0 = Wake on LOW level

  // 3. Timer wakeup if time given else pin interrupt wakeup
  if (milli_sec > 0) esp_sleep_enable_timer_wakeup(milli_sec * 1000ULL);

  Serial.println("Entering Deep Sleep now...");
  esp_deep_sleep_start();
}

void handleTimeouts() {
  if (authTimeout) {
    if (millis() >= authTimeout + AUTH_DISABLE_TIME) {
      authTimeout = 0;
      authFail = 0;
    }
  }

  if (mqttActive) {
    // Auto-disable MQTT after 2 minutes of no remote commands to save battery
    if (millis() - lastActivity > MQTT_ACTIVE_TIMEOUT) {
      endMQTTSession();
    }
  }

  // K230D Power Management (3s x 3 = 9s timeout logic)
  if (k230IsRunning && (millis() - k230StartTime > K230D_MAX_UPTIME)) {
    Serial.println("K230D Timeout: No face detected. Powering down.");
    K230DPowerOff();
  }

  // Timeout after failure extension until pin is manually entered
  if (faceUnlockTimeout && pinManuallyEntered) {
    faceUnlockTimeout = 0;
    intruder = 0;
    pinManuallyEntered = false;
  }
}

void unlockDoor(String source) {
  FCM_Notification("Lock Status", "Unlocked by " + source);
  // Fail-secure lock logic, Adjust logic for your lock type
  digitalWrite(LOCK_PIN, HIGH);  // Activate Solenoid (Open Lock)
  delay(3000);                   // Pulse duration
  digitalWrite(LOCK_PIN, LOW);   // Deactivate
}

bool checkPin(const char *passCode) {
  // Compare pass code from eeprom non volatile memory
  String pin = prefs.getString("pin");
  if (pin.equals("")) {
    Serial.println("No pin code is set");
    return true;
  }
  return pin.equals(passCode);
}

void handleUART() {
  if (Serial.available()) {
    String response = Serial.readStringUntil('\n');
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, response);

    if (!error) {
      lastActivity = millis();
      String status = doc["status"];

      if (status == "match") {
        unlockDoor(doc["name"]);
        serverLog("{\"event\": \"unlock\", \"method\": \"face\", \"success\": \"true\", \"name\": \"" +
                  doc["name"].as<String>() + "\"}");
        K230DPowerOff();
      } else if (status == "intruder") {
        FCM_Notification("Intruder Alert!", "Unknown face detected at door.");
        intruder += 1;
        if (intruder <= 3) {
          // Stay on for another 3s (reset timer) to capture more frames/upload
          k230UpTime += millis() - k230StartTime;
          k230StartTime = millis();
        } else {
          faceUnlockTimeout = millis();
          K230DPowerOff();
        }
        serverLog("{\"event\": \"unlock\", \"method\": \"face\", \"success\": \"false\"}");
      } else if (status == "awake") {
        bootTime = (millis() - k230StartTime);
        serverLog("{\"event\": \"boot\", \"bootTime\": \"" + String(float(bootTime) / 1000.0, 4) + "\"}");
      }
    }
  }
}

uint8_t getBatteryLevel() {
  int raw = analogRead(BATTERY_PIN);
  float sumVolt = 0;
  for (uint8_t i = 0; i < 10; i++) {
    sumVolt += (raw / 4095.0) * 3.3 * (12.0 / 3.3);  // Adjust for your voltage divider
  }
  float voltage = sumVolt / 10;
  int scaled = (int)(voltage * 100);
  switch (scaled) {
    case 1260 ... 1300:  // 12.6+ volts
      return 100;
    case 1250: return 90;
    case 1242: return 80;
    case 1232: return 70;
    case 1220: return 60;
    case 1206: return 50;
    case 1190: return 40;
    case 1175: return 30;
    case 1158: return 20;
    case 1131: return 10;
    case 1050: return 0;
    default: return 0;
  }
}

void monitorBattery() {
  if (millis() - lastBatCheck > 15 * 60000ULL) {  // Check every 15 minutes
    uint8_t batLevel = getBatteryLevel();
    switch (batLevel) {
      case 20:
        FCM_Notification("Low Battery", "{\"battery\": 20%}");
        FCM_Notification("Low Battery", "{\"warning\": \"Battery Low. Charge battery soon.\"}");
        break;
      case 10:
        FCM_Notification("Low Battery", "{\"battery\": 10%}");
        FCM_Notification("Low Battery", "{\"warning\": \"Battery Low. Charge battery.\"}");
        break;
      case 0: FCM_Notification("Low Battery", "{\"warning\": \"Battery depleted. Recharge Now!\"}"); break;
      default: FCM_Notification("Lock Battery", "{\"battery\": " + String(batLevel) + "%}");
    }
    lastBatCheck = millis();
  }
}

// --- NOTIFICATIONS & CONNECTIVITY ---

void FCM_Notification(String title, String body) {
  WiFiClientSecure client;
  client.setInsecure();
  if (client.connect(fcm_server, 443)) {
    String payload = "{\"to\":\"/topics/" + USER_ID + "/all\", \"priority\":\"high\", \"notification\":{\"title\":\"" +
                     title + "\", \"body\":\"" + body + "\"}}";
    client.println("POST /fcm/send HTTP/1.1");
    client.println("Authorization: key=" + String(fcm_key));
    client.println("Content-Type: application/json");
    client.print("Content-Length: ");
    client.println(payload.length());
    client.println();
    client.print(payload);
  }
  client.stop();
}

bool registerLock(String token) {
  HTTPClient http;
  String url = String(cloud_endpoint) + "/register";

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + token);
  String body = "{\"userId\":\"" + USER_ID + "\", \"lockId\":\"" + LOCK_ID + "\",\"lockName\":\"" + LOCK_NAME +
                "\",\"owner\":\"" + OWNER_NAME + "\",\"model\":\"" + String(LOCK_MODEL) + "\",\"firmwareVersion\":\"" +
                String(FIRMWARE_VERSION) + "\"}";
  Serial.printf("Post Data: %s", body.c_str());
  int httpResponseCode = http.POST(body);

  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.printf("Response code: %d\nResponse: %s\n", httpResponseCode, response.c_str());
    if (httpResponseCode == HTTP_CODE_CREATED) {
      return true;
    } else {
      return false;
    }
  } else {
    Serial.printf("[HTTP] Register lock failed, error: %s\n", http.errorToString(httpResponseCode).c_str());
    return false;
  }
  prefs.remove("token");
  http.end();
}

void initialCommisioning() {
  String WIFI_SSID = prefs.getString("wifi_ssid");
  String WIFI_PWD = prefs.getString("wifi_pwd");

  // If credentials not in NVS, wait for BLE commissioning
  if (WIFI_SSID.isEmpty()) {
    Serial.println("Waiting for BLE commissioning payload to complete...");

    unsigned long commissionStart = millis();
    while (millis() - commissionStart < COMMISSION_TIME) {
      delay(100);  // Avoid busy loop
      if (bleServer.hasReceivedPayload()) {
        WIFI_SSID = prefs.getString("wifi_ssid");
        WIFI_PWD = prefs.getString("wifi_pwd");
        USER_ID = prefs.getString("user_id");
        LOCK_NAME = prefs.getString("lock_name");
        OWNER_NAME = prefs.getString("owner");
        break;
      }
    }

    if (WIFI_SSID.isEmpty()) {
      Serial.println("Commission timeout. Require Restart...");
      startDeepSleep();
      return;
    }
  }

  // Connect to WiFi
  Serial.println("Connecting to WiFi: " + WIFI_SSID);
  WiFi.begin(WIFI_SSID.c_str(), WIFI_PWD.c_str());

  unsigned long wifiStartTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStartTime < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection failed. Restarting...");
    prefs.clear();  // Discard BLE message
    bleServer.sendResponse("{\"status\":\"wifi_fail\"}");
    startDeepSleep(200);
    return;
  }

  Serial.println("WiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  if (!registerLock(prefs.getString("token"))) {
    prefs.clear();
    bleServer.sendResponse("{\"error\":\"Failed to register Lock\"}");
    Serial.println("Registering lock failed.");
    startDeepSleep();
  }

  String simpleId = String(LOCK_ID).substring(0, 4);
  simpleId.toUpperCase();
  WiFi.setHostname(("JUPY_" + LOCK_NAME + simpleId).c_str());  // Set local hostname
  Serial.println("Hostname set to: JUPY_" + LOCK_NAME);

  // Send lock info via BLE TX characteristic (after WiFi is connected)
  bleServer.sendResponse("{\"lock_id\":\"" + String(LOCK_ID) + "\",\"lock_ip\":\"" + WiFi.localIP().toString() +
                         "\",hostname\":\"" + String(WiFi.getHostname()) + "\"}");

  // --- CRITICAL POWER SAVING MODES ---
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);  // 1. Enable Wi-Fi Modem Sleep

  // 2. Set DTIM interval
  wifi_config_t conf;
  esp_wifi_get_config(WIFI_IF_STA, &conf);
  conf.sta.listen_interval = 10;  // Listen every 10 beacons (~1 second)
  esp_wifi_set_config(WIFI_IF_STA, &conf);

  Serial.println("Wi-Fi Power Save Enabled");

  // Disable BLE after commissioning
  btStop();
  esp_bt_controller_disable();
  esp_bt_controller_deinit();
  Serial.println("BLE Disabled. Wi-Fi Active.");
}

void endMQTTSession() {
  mqttClient.disconnect();
  mqttActive = false;
  Serial.println("MQTT Session Terminated to save battery.");
}

void reconnectMQTT() {
  if (mqttClient.connect("JUPY_SmartLock")) {
    mqttClient.subscribe(("lock/commands/" + USER_ID).c_str(), 0);
  }
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  lastActivity = millis();
  JsonDocument doc;
  deserializeJson(doc, payload);

  if (doc["cmd"] == "unlock") unlockDoor("Remote App");
  else if (doc["cmd"] == "start_call")
    wakeK230D("{\"cmd\":\"start_call\",\"room_id\":\"" + doc["room_id"].as<String>() + "\"}");
  else if (doc["cmd"] == "end_call") endMQTTSession();
}

void serverLog(String log) {
  // TODO: User database logging instead via post request instead of MQTT
  if (mqttActive) {
    mqttClient.publish(("lock/logs/" + USER_ID).c_str(), log.c_str());
  }
}

void handleRequest(String route, HTTPMethod method, std::function<HTTPResponse(const String &)> callback) {
  localServer.on(route, method, [route, callback]() {
    String body = "";
    if (localServer.hasArg("plain")) {
      body = localServer.arg("plain");  // get POST body
      Serial.println("Received body: " + body);
    } else {
      Serial.print("At route " + route);
      Serial.println(" No body received");
    }
    HTTPResponse resp = callback(body);
    if (resp.code == 0 && resp.contentType == "") resp = HTTPResponse{200, "text/plain", String("")};
    localServer.send(resp.code, resp.contentType, resp.body.c_str());
  });
}

bool validateSettings(const char *setting) {
  String settings[] = {"motion_sensitivity", "vid_quality", "call_timeout", "snippet_time", "share_analytics"};
  for (String option : settings) {
    if (option.equals(setting)) return true;
  }
  return false;
}

HTTPResponse updateSettings(String body) {
  if (authFail == 3) {
    return HTTPResponse{401, "application/json",
                        ("{\"status\":\"fail\", \"error\":\"Authorization Timeout\", \"timeRemaining\": " +
                         String((AUTH_DISABLE_TIME - (millis() - authTimeout)) / 60000UL) + "}")};
  }
  JsonDocument data;
  DeserializationError error = deserializeJson(data, body);
  if (error) {
    return HTTPResponse{400, "application/json", "{\"status\":\"fail\", \"error\":\"Parsing failed. Try Again.\"}"};
  }

  String name = data["name"];
  long time = data["time"];  // 1351824120

  JsonObject settings = data["settings"];
  int motion_sensitivity = settings["motion_sensitivity"];  // 80
  int vid_quality = settings["vid_quality"];                // 1024
  int call_timeout = settings["call_timeout"];              // 40
  int snippet_time = settings["snippet_time"];              // 15
  notify_motion = settings["notify_motion"];                // true
  share_analytics = settings["share_analytics"];            // true

  if (!checkPin(data["pin"].as<const char *>()) || !name.equals(OWNER_NAME)) {
    authFail += 1;
    if (authFail == 3) {
      authTimeout = millis();
    }
    return HTTPResponse{401, "application/json", "{\"status\":\"fail\", \"error\":\"Unauthorized Access\"}"};
  }
  if (name && settings) {
    for (JsonPair kvp : settings) {
      String option = String(kvp.key().c_str());
      if (validateSettings(option.c_str())) continue;
      else {
        return HTTPResponse{400, "application/json",
                            "{\"status\":\"fail\", \"error\":\"Unknown settings. May need firmware update\"}"};
      }

      uint value = (uint)kvp.value() | prefs.getUInt(option.c_str());
      prefs.putUInt(option.c_str(), value);  // Write settings to non-volatile storage
      if (option.equals("lock_name")) {
        FCM_Notification("Change Lock Name",
                         name + " changed " + OWNER_NAME + "'s " + LOCK_NAME + " to " + value + ".");
      } else if (option.equals("call_timeout")) {
        wakeK230D("{\"cmd\":\"set_call_timeout\",\"call_timeout\": " + String(value) + "}");
      } else if (option.equals("snippet_time")) {
        wakeK230D("{\"cmd\":\"set_snippet_time\",\"snippet_time\": " + String(value) + "}");
      } else if (option.equals("vid_quality")) {
        wakeK230D("{\"cmd\":\"set_vid_quality\",\"vid_quality\": " + String(value) + "}");
      } else if (option.equals("pin")) {
        FCM_Notification("Pin changed", name + " changed " + OWNER_NAME + "'s " + LOCK_NAME + "pin.");
      }

      if (share_analytics) {
        String json;
        serializeJson(settings, json);
        json = "{type: \"settings\"," + json + "}";
        serverLog(json.c_str());
      }
    }
    return HTTPResponse{200, "application/json", "{\"status\":\"success\"}"};
  } else {
    return HTTPResponse{400, "application/json", "{\"status\":\"fail\", \"error\":\"Bad request.\"}"};
  }
}

void setupREST() {
  handleRequest("/unlock", HTTP_POST, [](String body) {
    JsonDocument data;
    DeserializationError error = deserializeJson(data, body);
    if (error) {
      return HTTPResponse{400, "application/json", "{\"status\":\"fail\", \"error\":\"Parsing failed. Try again\" }"};
    }
    if (checkPin(data["pin"].as<const char *>())) {
      unlockDoor(data["name"]);
      return HTTPResponse{200, "application/json", "{\"status\":\"success\"}"};
    } else {
      return HTTPResponse{401, "application/json",
                          "{\"status\":\"fail\", \"error\":\"Wrong pin stored, pin may have been updated\" }"};
    }
  });
  handleRequest("/update-settings", HTTP_PATCH, &updateSettings);
  handleRequest("/status", HTTP_GET, [](String body) {
    String status = "{";
    status += "\"lock_name\":\"" + LOCK_NAME + "\",";
    status += "\"owner\":\"" + OWNER_NAME + "\",";
    status += "\"wifi_ssid\":\"" + prefs.getString("wifi_ssid") + "\",";
    status += "\"battery\":\"" + String(getBatteryLevel()) + "\",";
    status += "}";
    return HTTPResponse{200, "application/json", status};
  });
  localServer.begin();
}

// --- DISPLAY & TOUCH ---
void drawKeypad() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  String keys[4][3] = {
      {"1", "2", "3"},
      {"4", "5", "6"},
      {"7", "8", "9"},
      {"x", "0", "🔔"}  // X: Clear, B: Bell/Enter
  };

  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 3; c++) {
      tft.drawRect(c * 80, r * 60 + 10, 80, 50, TFT_WHITE);
      tft.drawString(keys[r][c], c * 80 + 35, r * 60 + 60);
    }
  }
}

void handleTouch() {
  uint16_t x, y;

  if (tft.getTouch(&x, &y)) {
    int col = x / 80;
    int row = (y - 40) / 60;

    if (row == 3 && col == 0) {  // X - Clear
      passcodeBuffer = "";
    } else if (row == 3 && col == 2) {  // B - Bell
      if (passcodeBuffer.length() == 0) {
        FCM_Notification("Doorbell", "Someone is at " + OWNER_NAME + "'s " + LOCK_NAME + "!");
        mqttActive = true;  // Enable MQTT to listen for the call initiation
      } else {
        if (checkPin(passcodeBuffer.c_str())) {
          unlockDoor("Passcode");
          if (faceUnlockTimeout) pinManuallyEntered = true;
        }
        passcodeBuffer = "";
      }
    }
    delay(200);  // Debounce
  }
}