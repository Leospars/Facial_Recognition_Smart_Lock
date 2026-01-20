#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
// #include <Matter.h>
// #include <MatterEndPoint.h>
#include <Preferences.h>
#include <TFT_eSPI.h>

#include "esp_bt.h"
// #include "esp_bt_main.h"
#include <esp_wifi.h>

// --- Pins (As specified) ---
#define PIR_PIN 26
#define LOCK_PIN 24
#define K230D_PWR_PIN 23
#define BATTERY_PIN 0
#define TOUCH_CS 10

// Display PINS
#define TFT_MISO 2
#define TFT_MOSI 7
#define TFT_SCLK 6
#define TFT_CS 1    // Chip select control pin
#define TFT_DC 5    // Data Command control pin
#define TFT_RST -1  // Set TFT_RST to -1 if display RESET is connected to ESP32 board RST

// --- Configuration & Credentials ---
const char *fcm_server = "fcm.googleapis.com";
const char *fcm_key = "YOUR_FCM_SERVER_KEY";  // Legacy Key or OAuth2 Relay
const char *mqtt_server = "broker.hivemq.com";

#define AUTH_DISABLE_TIME 30 * 60000UL  // 30 minutes

// --- Instances ---
TFT_eSPI tft = TFT_eSPI();
// XPT2046_Touchscreen ts(TOUCH_CS);
WiFiClient espClient;
PubSubClient mqttClient(espClient);
WebServer localServer(80);
// MatterDoorLock doorLock;
Preferences prefs;

// --- Stored Variables ---
String LOCK_NAME = "";
String OWNER_NAME = "";

// --- State Management ---
bool mqttActive = false;
unsigned long authTimeout = 0;
unsigned long bootTime = 0;
unsigned long lastActivity = 0;
unsigned long k230StartTime = 0;
bool k230IsRunning = false;
bool share_analytics = false;
bool notify_motion = false;

uint8_t authFail = 0;
String passcodeBuffer = "";

// Function Prototypes
void handlePIR();
void handleUART();
void handleTouch();
void monitorBattery();
void initialCommisioning();
void wakeK230D(String command = "{\"cmd\":\"On\"}");

bool checkPin(const char *);
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

void setup() {
  Serial.begin(115200);

  pinMode(PIR_PIN, INPUT);
  pinMode(LOCK_PIN, OUTPUT);
  pinMode(K230D_PWR_PIN, OUTPUT);
  pinMode(BATTERY_PIN, INPUT);

  digitalWrite(LOCK_PIN, HIGH);      // Fail-secure: HIGH usually keeps locked
  digitalWrite(K230D_PWR_PIN, LOW);  // K230D off by default

  tft.init();
  tft.setRotation(1);
  drawKeypad();

  // 0. Initialize Storage
  prefs.begin("my-settings", false);

  // 1. Matter/BLE Provisioning & Transition
  initialCommisioning();

  // 2. Local REST API
  setupREST();

  mqttClient.setServer(mqtt_server, 1883);
  mqttClient.setCallback(mqttCallback);
}

void loop() {
  handlePIR();
  handleUART();
  handleTouch();
  monitorBattery();
  localServer.handleClient();

  if (mqttActive) {
    if (!mqttClient.connected()) reconnectMQTT();
    mqttClient.loop();

    // Auto-disable MQTT after 2 minutes of no remote commands to save battery
    if (millis() - lastActivity > 120000) {
      endMQTTSession();
    }
  }

  if (authTimeout) {
    if (millis() >= authTimeout + AUTH_DISABLE_TIME) {
      authTimeout = 0;
      authFail = 0;
    }
  }

  // K230D Power Management (5s timeout logic)
  if (k230IsRunning && (millis() - k230StartTime > 5000)) {
    Serial.println("K230D Timeout: No face detected. Powering down.");
    digitalWrite(K230D_PWR_PIN, LOW);
    k230IsRunning = false;
  }
}

// --- CORE LOGIC FUNCTIONS ---

void handlePIR() {
  if (digitalRead(PIR_PIN) == HIGH && !k230IsRunning) {
    delay(50);  // Debounce
    if (notify_motion)
      FCM_Notification("Motion Detected", "Waking up Vision System...");
    wakeK230D();
  }
}

void wakeK230D(String command) {
  digitalWrite(K230D_PWR_PIN, HIGH);
  Serial.println(command);
  k230StartTime = millis();
  k230IsRunning = true;
}

void K230DPowerOff() {
  digitalWrite(K230D_PWR_PIN, LOW);
  k230IsRunning = false;
  serverLog("{\"event\": \"power_off\", \"uptime\": \"" + String((millis() - k230StartTime) / 1000) + "\"}");
  Serial.println("K230D Powered Off.");
}

void unlockDoor(String source) {
  FCM_Notification("Lock Status", "Unlocked by " + source);
  // Fail-secure lock logic, Adjust logic for your lock type
  digitalWrite(LOCK_PIN, HIGH);   // Activate Solenoid
  delay(3000);                   // Pulse duration
  digitalWrite(LOCK_PIN, LOW);  // Deactivate
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
        serverLog("{\"event\": \"unlock\", \"method\": \"face\", \"success\": \"true\", \"name\": \"" + doc["name"].as<String>() + "\"}");
        K230DPowerOff();
      } else if (status == "intruder") {
        FCM_Notification("Intruder Alert!", "Unknown face detected at door.");
        // Stay on for another interval (reset timer) to capture more frames/upload
        k230StartTime = millis();
      } else if (status == "awake") {
        bootTime = (millis() - k230StartTime);
        serverLog("{\"event\": \"boot\", \"bootTime\": \"" + String(float(bootTime) / 1000.0, 4) + "\"}");
      }
    }
  }
}

void monitorBattery() {
  static unsigned long lastBatCheck = 0;
  if (millis() - lastBatCheck > 180000) {  // Check every 3 minutes
    int raw = analogRead(BATTERY_PIN);
    float sumVolt = 0;
    for (uint8_t i = 0; i < 10; i++) {
      sumVolt += (raw / 4095.0) * 3.3 * (12.0 / 3.3);  // Adjust for your voltage divider
    }
    float voltage = sumVolt / 10;
    int scaled = (int)(voltage * 100);
    switch (scaled) {
      case 1260 ... 1300:  // 12.6+ volts
        FCM_Notification("Lock Battery", "{\"battery\": 100%}");
        break;
      case 1250:
        FCM_Notification("Lock Battery", "{\"battery\": 90%}");
        break;
      case 1242:
        FCM_Notification("Lock Battery", "{\"battery\": 80%}");
        break;
      case 1232:
        FCM_Notification("Lock Battery", "{\"battery\": 70%}");
        break;
      case 1220:
        FCM_Notification("Lock Battery", "{\"battery\": 60%}");
        break;
      case 1206:
        FCM_Notification("Lock Battery", "{\"battery\": 50%}");
        break;
      case 1190:
        FCM_Notification("Lock Battery", "{\"battery\": 40%}");
        break;
      case 1175:
        FCM_Notification("Lock Battery", "{\"battery\": 30%}");
        break;
      case 1158:
        FCM_Notification("Low Battery", "{\"battery\": 20%}");
        FCM_Notification("Low Battery", "{\"warning\": \"Battery Low. Charge battery soon.\"}");
        break;
      case 1131:
        FCM_Notification("Low Battery", "{\"battery\": 10%}");
        FCM_Notification("Low Battery", "{\"warning\": \"Battery Low. Charge battery.\"}");
        break;
      case 1050:
        FCM_Notification("Low Battery", "{\"warning\": \"Battery depleted. Recharge Now!\"}");
        break;
      default:
        FCM_Notification("Low Battery", "{\"error\": \"Voltage out of range\"}");
    }
    lastBatCheck = millis();
  }
}

// --- NOTIFICATIONS & CONNECTIVITY ---

void FCM_Notification(String title, String body) {
  WiFiClientSecure client;
  client.setInsecure();
  if (client.connect(fcm_server, 443)) {
    String payload = "{\"to\":\"/topics/all\", \"priority\":\"high\", \"notification\":{\"title\":\"" + title + "\", \"body\":\"" + body + "\"}}";
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

void initialCommisioning() {
  String WIFI_SSID = prefs.getString("wifi-ssid");
  String WIFI_PWD = prefs.getString("wifi-pwd");

  if (WIFI_SSID == "") {
    // Start Matter or BLE provisioning to get Wi-Fi, lock and user credentials

    LOCK_NAME = "lock";
    OWNER_NAME = "owner";
    String PIN = "1234";
    
    prefs.putString("wifi-ssid", "Your_SSID");
    prefs.putString("wifi-pwd", "Your_PASSWORD");
    prefs.putString("lock-name", LOCK_NAME);
    prefs.putString("pin", PIN);
  }
  
  // Matter.begin(); // Provisioning starts here
  // After Wi-Fi credentials are received via Matter BLE:
  WiFi.begin(WIFI_SSID, WIFI_PWD);
  while (WiFi.status() != WL_CONNECTED)
    delay(500);

  // --- CRITICAL POWER SAVING MODES ---
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);  // 1. Enable Wi-Fi Modem Sleep

  // 2. Set DTIM interval (Router dependent, but we tell the ESP32 to be aggressive)
  wifi_config_t conf;
  esp_wifi_get_config(WIFI_IF_STA, &conf);
  conf.sta.listen_interval = 10;  // Listen every 10 beacons (~1 second)
  esp_wifi_set_config(WIFI_IF_STA, &conf);

  Serial.println("Wi-Fi Power Save Enabled");

  // DISABLE BLUETOOTH
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
  if (mqttClient.connect("SmartLock_ESP32")) {
    mqttClient.subscribe("lock/commands");
  }
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  lastActivity = millis();
  JsonDocument doc;
  deserializeJson(doc, payload);

  if (doc["cmd"] == "unlock") unlockDoor("Remote App");
  else if (doc["cmd"] == "start_call") wakeK230D("{\"cmd\":\"start_call\", \"room-id\":\"" + doc["room-id"].as<String>() + "\"}");
  else if (doc["cmd"] == "end_call") endMQTTSession();
}

void serverLog(String log) {
  //TODO: User database logging instead via post request instead of MQTT
  if (mqttActive) {
    mqttClient.publish("lock/logs/", log.c_str());
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
    if (resp.code == 0 && resp.contentType == "") resp = HTTPResponse{ 200, "text/plain", String("") };
    localServer.send(resp.code, resp.contentType, resp.body.c_str());
  });
}

// name, id, value, pin settings: "lock-name", "pin"
bool validateSettings(const char *setting) {
  String settings[] = { "motion-sensitivity", "vid-quality", "call-timeout", "snippet-time", "share-analytics" };
  for (String option : settings) {
    if (option.equals(setting))
      return true;
  }
  return false;
}

HTTPResponse updateSettings(String body) {
  if (authFail == 3) {
    return HTTPResponse{ 401, "application/json", ("{\"status\":\"fail\", \"error\":\"Authorization Timeout\", \"timeRemaining\": " + String((AUTH_DISABLE_TIME - (millis() - authTimeout)) / 60000UL) + "}") };
  }
  JsonDocument data;
  DeserializationError error = deserializeJson(data, body);
  if (error) {
    return HTTPResponse{ 400, "application/json", "{\"status\":\"fail\", \"error\":\"Parsing failed. Try Again.\"}" };
  }

  String name = data["name"];
  long time = data["time"];  // 1351824120

  JsonObject settings = data["settings"];
  int motion_sensitivity = settings["motion-sensitivity"];   // 80
  int vid_quality = settings["vid-quality"];                 // 1024
  int call_timeout = settings["call-timeout"];               // 40
  int snippet_time = settings["snippet-time"];               // 15
  notify_motion = settings["notify-motion"];                 // true
  share_analytics = settings["share-analytics"];             // true

  if (!checkPin(data["pin"].as<const char *>()) || !name.equals(OWNER_NAME)) {
    authFail += 1;
    if (authFail == 3) {
      authTimeout = millis();
    }
    return HTTPResponse{ 401, "application/json", "{\"status\":\"fail\", \"error\":\"Unauthorized Access\"}" };
  }
  if (name && settings) {
    for (JsonPair kvp : settings) {
      String option = String(kvp.key().c_str());
      if (validateSettings(option.c_str()))
        continue;
      else {
        return HTTPResponse{ 400, "application/json", "{\"status\":\"fail\", \"error\":\"Unknown settings. May need firmware update\"}" };
      }

      uint value = (uint) kvp.value() | prefs.getUInt(option.c_str());
      prefs.putUInt(option.c_str(), value); // Write settings to non-volatile storage
      if (option.equals("lock-name")) {
        FCM_Notification("Change Lock Name", name + " changed " + OWNER_NAME + "'s " + LOCK_NAME + " to " + value + ".");
      } else if(option.equals("call-timeout")) {
        wakeK230D("{\"cmd\":\"set_call_timeout\", \"call-timeout\": " + String(value) + "}");
      } else if(option.equals("snippet-time")) {
        wakeK230D("{\"cmd\":\"set_snippet_time\", \"snippet-time\": " + String(value) + "}");
      } else if (option.equals("vid-quality")) {
        wakeK230D("{\"cmd\":\"set_vid_quality\", \"vid-quality\": " + String(value) + "}");
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
    return HTTPResponse{ 200, "application/json", "{\"status\":\"success\"}" };
  } else {
    return HTTPResponse{ 400, "application/json", "{\"status\":\"fail\", \"error\":\"Bad request.\"}" };
  }
}

void setupREST() {
  handleRequest("/unlock", HTTP_POST, [](String body) {
    JsonDocument data;
    DeserializationError error = deserializeJson(data, body);
    if (error) {
      return HTTPResponse{ 400, "application/json", "{\"status\":\"fail\", \"error\":\"Parsing failed. Try again\" }" };
    }
    if (checkPin(data["pin"].as<const char *>())) {
      unlockDoor(data["name"]);
      return HTTPResponse{ 200, "application/json", "{\"status\":\"success\"}" };
    } else {
      return HTTPResponse{ 401, "application/json", "{\"status\":\"fail\", \"error\":\"Wrong pin stored, pin may have been updated\" }" };
    }
  });

  handleRequest("/update-settings", HTTP_PATCH, &updateSettings);
  handleRequest("/status", HTTP_GET, [](String body) {
    String status = "{";
    status += "\"lock-name\":\"" + LOCK_NAME + "\",";
    status += "\"owner-name\":\"" + OWNER_NAME + "\",";
    status += "\"wifi-ssid\":\"" + prefs.getString("wifi-ssid") + "\",";
    status += "\"battery-voltage\":\"" + String((analogRead(BATTERY_PIN) / 4095.0) * 3.3 * (12.0 / 3.3), 2) + "\",";
    status += "}";
    return HTTPResponse{ 200, "application/json", status };
  });
  localServer.begin();
}

// --- DISPLAY & TOUCH ---
void drawKeypad() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  String keys[4][3] = {
    { "1", "2", "3" },
    { "4", "5", "6" },
    { "7", "8", "9" },
    { "x", "0", "🔔" }  // X: Clear, B: Bell/Enter
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
        FCM_Notification("Doorbell", "Someone is at " + LOCK_NAME + "!");
        mqttActive = true;  // Enable MQTT to listen for the call initiation
      } else {
        if (checkPin(passcodeBuffer.c_str()))
          unlockDoor("Passcode");
        passcodeBuffer = "";
      }
    }
    delay(200);  // Debounce
  }
}