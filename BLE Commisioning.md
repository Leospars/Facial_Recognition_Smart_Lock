# Smart Lock BLE Commissioning Flow Documentation

## Overview

This document describes the commissioning process for the smart lock device using BLE and Wi-Fi connectivity, followed by cloud registration.

The system uses:

* Pairing Code: **123456**
* BLE Service UUID: `12345678-1234-5678-1234-56789abcdef0`
* RX Characteristic UUID: `87654321-4321-8765-4321-0fedcba98765`
* TX Characteristic UUID: `abcdef12-5678-90ab-cdef-1234567890ab`

The commissioning process allows a mobile application to securely provision Wi-Fi credentials and user metadata into the lock.

---

## System Components

### 1. Mobile Application

Responsible for:

* Connecting to the lock over BLE
* Sending commissioning payload
* Receiving lock status and IP information

### 2. BLE Commissioning Server (ESP32)

Handles:

* BLE advertising and connections
* Payload validation
* Secure credential storage
* Status notifications

### 3. Preferences (NVS Storage)

Non-volatile storage used for:

* Wi-Fi credentials
* User information
* Token
* Pairing code validation

### 4. Cloud Backend

Provides:

* Lock registration endpoint `/register`
* Authentication using Bearer token
* Device provisioning confirmation

---

## Commissioning Payload Format

```json
{
  "user_id": "user123",
  "wifi_ssid": "HomeWiFi",
  "wifi_pwd": "password",
  "lock_name": "FrontDoor",
  "owner": "John",
  "pin": "1234",
  "pairing_code": "123456",
  "token": "jwt_token_here"
}
```

---

## Commissioning Flow

### Step 1 — BLE Connection

1. Lock advertises BLE service.
2. Mobile app connects.
3. Mobile app writes payload to RX characteristic.

---

### Step 2 — Payload Validation

Device validates:

* JSON format
* Required fields
* Pairing code match with stored value

If validation fails:

```
{"error":"Invalid pairing code"}
```

If successful:

```
{"status":"received"}
```

Credentials are stored in NVS.

---

### Step 3 — Wi-Fi Connection

Device attempts connection using stored credentials.

#### Failure Case

* Preferences cleared
* BLE response:

```
{"status":"wifi_fail"}
```

* Device enters deep sleep

---

### Step 4 — Cloud Registration

Device sends HTTP POST:

```
POST /register
Authorization: Bearer <token>
Content-Type: application/json
```

Body contains:

* userId
* lockId
* lockName
* owner
* model
* firmwareVersion

#### Success

HTTP 201 → Registration complete

#### Failure

BLE response:

```
{"error":"Failed to register Lock"}
```

---

### Step 5 — Lock Information Response

After successful registration:

```
{
  "lock_id": "LOCK123",
  "lock_ip": "192.168.1.10",
  "hostname": "JUPY_FrontDoorABCD"
}
```

and BLE response:

```
{"status":"ip_received"}
```
---

## Power Optimization Phase

After commissioning:

1. Wi-Fi modem sleep enabled
2. DTIM listen interval configured
3. BLE connection disabled
4. Device transitions to normal operation mode

---

## Timeout Behavior

If no BLE payload is received within commissioning window:

* Device enters deep sleep
* Requires restart to retry commissioning

---

## Security Considerations

* Pairing code verification prevents unauthorized provisioning
* Token-based cloud authentication
* Credentials stored in NVS (non-volatile secure storage)
* BLE disabled after commissioning to reduce attack surface

---

## Future Improvements

* Encrypted BLE payload
* Mutual TLS for cloud registration
* Secure element for credential storage
* Commissioning retry backoff strategy

---

## Conclusion

The commissioning flow ensures a secure and reliable onboarding process by combining BLE provisioning, Wi-Fi connectivity, and authenticated cloud registration, followed by aggressive power optimization.
