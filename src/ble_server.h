#ifndef BLE_SERVER_H
#define BLE_SERVER_H

#include <ArduinoJson.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

// UUIDs for BLE Service and Characteristics
#define SERVICE_UUID "12345678-1234-5678-1234-56789abcdef0"
#define RX_CHAR_UUID "87654321-4321-8765-4321-0fedcba98765"  // Receive commissioning payload
#define TX_CHAR_UUID "abcdef12-5678-90ab-cdef-1234567890ab"  // Send lock info response

class BLECommissioningServer {
public:
  BLECommissioningServer();
  ~BLECommissioningServer();

  void begin(const char *deviceName);
  void sendResponse(const String &response);
  bool isConnected();
  bool hasReceivedPayload();
  bool hasReceivedIPAck();
  void end();

private:
  BLEServer *pServer;
  BLECharacteristic *pRxCharacteristic;
  BLECharacteristic *pTxCharacteristic;
  bool deviceConnected;
  bool payloadReceived;
  bool ipReceivedAck;

  friend class ServerCallbacks;
  friend class RxCharacteristicCallbacks;
};

// Callback class for BLE Server events
class ServerCallbacks : public BLEServerCallbacks {
public:
  ServerCallbacks(BLECommissioningServer *server) : bleServer(server) {}

  void onConnect(BLEServer *pServer);
  void onDisconnect(BLEServer *pServer);

private:
  BLECommissioningServer *bleServer;
};

// Callback class for RX Characteristic (only for parsing/validation)
class RxCharacteristicCallbacks : public BLECharacteristicCallbacks {
public:
  RxCharacteristicCallbacks(BLECommissioningServer *server) : bleServer(server) {}

  void onWrite(BLECharacteristic *pCharacteristic);

private:
  BLECommissioningServer *bleServer;
};

#endif  // BLE_SERVER_H
