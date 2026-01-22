#include "ble_server.h"
#include <Preferences.h>

// ==================== BLECommissioningServer ====================
BLECommissioningServer::BLECommissioningServer() 
  : pServer(nullptr), pRxCharacteristic(nullptr), pTxCharacteristic(nullptr), 
    deviceConnected(false), payloadReceived(false) {}

BLECommissioningServer::~BLECommissioningServer() {
  BLEDevice::deinit();
}

void BLECommissioningServer::begin(const char* deviceName) {
  BLEDevice::init(deviceName);
  
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks(this));
  
  BLEService* commissionService = pServer->createService(SERVICE_UUID);
  
  // RX: Write-only (client sends commissioning payload)
  pRxCharacteristic = commissionService->createCharacteristic(
    RX_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE_NR
  );
  pRxCharacteristic->setCallbacks(new RxCharacteristicCallbacks(this));
  
  // TX: Read + Notify (server sends lock info after WiFi connects)
  pTxCharacteristic = commissionService->createCharacteristic(
    TX_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pTxCharacteristic->addDescriptor(new BLE2902());
  pTxCharacteristic->setValue("{}");  // Default empty response
  
  commissionService->start();
  
  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();
  
  Serial.println("[BLE] Server started - Device: " + String(deviceName));
}

void BLECommissioningServer::sendResponse(const String& response) {
  if (!pTxCharacteristic) return;

  pTxCharacteristic->setValue(response.c_str());
  pTxCharacteristic->notify();
  
  Serial.println("[BLE] Response sent: " + response);
}

bool BLECommissioningServer::isConnected() {
  return deviceConnected;
}

bool BLECommissioningServer::hasReceivedPayload() {
  return payloadReceived;
}

// ==================== ServerCallbacks ====================
void ServerCallbacks::onConnect(BLEServer* pServer) {
  bleServer->deviceConnected = true;
  Serial.println("[BLE] Client connected");
}

void ServerCallbacks::onDisconnect(BLEServer* pServer) {
  bleServer->deviceConnected = false;
  Serial.println("[BLE] Client disconnected");
  pServer->getAdvertising()->start();
}

// ==================== RxCharacteristicCallbacks ====================
void RxCharacteristicCallbacks::onWrite(BLECharacteristic* pCharacteristic) {
  std::string rxValue = pCharacteristic->getValue();
  
  if (rxValue.length() == 0) return;
  
  String payload = String((const char*)rxValue.c_str());
  Serial.println("[BLE] Received payload:");
  Serial.println(payload);
  
  // Parse and validate JSON
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  
  if (error) {
    Serial.println("[BLE] JSON parse error");
    return;
  }
  
  // Validate required fields
  if (!doc.containsKey("user-id") || !doc.containsKey("wifi-ssid") || 
      !doc.containsKey("wifi-pwd") || !doc.containsKey("lock-name") || 
      !doc.containsKey("owner-name") || !doc.containsKey("pin") ||
      !doc.containsKey("pairing-code")) {
    Serial.println("[BLE] Missing required fields");
    return;
  }
  
  Preferences prefs;
  prefs.begin("my-storage", false);
  if( !doc["pairing-code"].as<String>().equals(prefs.getString("pairing-code"))) {
    Serial.println("[BLE] Invalid pairing code");
    return;
  }
  
  // Store in NVS
  prefs.putString("user-id", doc["user-id"].as<String>());
  prefs.putString("wifi-ssid", doc["wifi-ssid"].as<String>());
  prefs.putString("wifi-pwd", doc["wifi-pwd"].as<String>());
  prefs.putString("lock-name", doc["lock-name"].as<String>());
  prefs.putString("owner-name", doc["owner-name"].as<String>());
  prefs.putString("pin", doc["pin"].as<String>());
  prefs.end();
  
  bleServer->payloadReceived = true;
  Serial.println("[BLE] Credentials stored successfully");
  
  // Send acknowledgment via TX characteristic
  String ack = "{\"status\":\"received\"}";
  bleServer->sendResponse(ack);
}

