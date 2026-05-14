# Smart Emotion Tracker - ESP32 IoT Application

Complete Arduino-based embedded system for reading RFID cards, capturing emotion responses via Nextion display, and publishing to HiveMQ Cloud MQTT.

## Hardware Setup

### ESP32-S3-DevKit-C-1

- **Processor**: Dual-core Xtensa 32-bit CPU, 240 MHz
- **Memory**: 8MB PSRAM, 16MB Flash
- **Connectivity**: WiFi 802.11 b/g/n (2.4 GHz)

### RFID Reader (MFRC522)

- **Interface**: SPI
- **Pins**:
  - CS/SS → GPIO 5
  - RST → GPIO 22
  - MOSI → GPIO 23
  - MISO → GPIO 19
  - SCK → GPIO 18

### Nextion Display (HMI)

- **Interface**: UART (Serial2)
- **Pins**:
  - RX → GPIO 16
  - TX → GPIO 17
- **Baud Rate**: 9600 bps
- **Protocol**: Binary touch events + text commands

### WiFi & MQTT

- **SSID**: Configured in `secrets.h`
- **MQTT Broker**: HiveMQ Cloud (SSL/TLS on port 8883)
- **Authentication**: Username + Password from `secrets.h`

## Project Structure

```
.
├── src/
│   └── main.cpp              # Main application logic
├── include/
│   └── secrets.h             # WiFi & MQTT credentials
├── lib/                       # External libraries
├── platformio.ini            # PlatformIO configuration
└── README.md                 # This file
```

## Application Flow

### 1. **RFID Card Detection**

- Continuously monitors RFID reader for new cards
- Reads card UID and converts to hexadecimal string
- Stores UID in `currentCardUID`
- Transitions to `STATE_WAITING_EMOTION`

### 2. **Nextion Page Switch**

- Sends command `page 1` to display emotion selection interface
- Command format: `page X` + 0xFF 0xFF 0xFF (terminator)

### 3. **Emotion Selection**

- Display shows 3 emotion buttons:
  - Button 1 (ComponentID 0x01): **senang** (happy)
  - Button 2 (ComponentID 0x02): **sedih** (sad)
  - Button 3 (ComponentID 0x03): **marah** (angry)
- User taps button on Nextion display

### 4. **Nextion Touch Event Reception**

- Monitors Serial2 for 7-byte touch event:
  ```
  [0x65] [0x01] [ComponentID] [0x01] [0xFF] [0xFF] [0xFF]
  ```
- Validates frame and extracts ComponentID
- Maps to emotion string

### 5. **MQTT Publishing**

- Creates JSON payload:
  ```json
  {
    "card_uid": "AB12CD34",
    "emotion": "senang",
    "timestamp": 12345678
  }
  ```
- Publishes to topic: `v1/emotion/logs`
- Returns Nextion to page 0 after successful publish

## Configuration

### secrets.h

Create `include/secrets.h` with your credentials:

```cpp
#ifndef SECRETS_H
#define SECRETS_H

// WiFi Configuration
#define SECRET_SSID "Your WiFi Network"
#define SECRET_PASS "Your WiFi Password"

// HiveMQ Cloud Configuration
#define SECRET_MQTT_SERVER "xxxxx.s1.eu.hivemq.cloud"
#define SECRET_MQTT_USER "username"
#define SECRET_MQTT_PASS "password"
#define SECRET_MQTT_PORT 8883

#endif
```

### platformio.ini

Includes required libraries:

- **MFRC522** (v1.4.10) - RFID reader driver
- **PubSubClient** (v2.8) - MQTT client
- **ArduinoJson** (v7.0.4) - JSON serialization

## State Management

```
    ┌─────────────────────────────────────┐
    │       STATE_IDLE                     │
    │  (Waiting for RFID card)            │
    └──────────────┬──────────────────────┘
                   │ RFID card detected
                   ↓
    ┌─────────────────────────────────────┐
    │   STATE_WAITING_EMOTION             │
    │  (Nextion page 1 displayed)         │
    │  (Listening for touch event)        │
    └──────────────┬──────────────────────┘
                   │ Touch event received
                   ↓
            [MQTT publish]
                   │
                   ↓
    ┌─────────────────────────────────────┐
    │       STATE_IDLE (reset)            │
    │   (Return to page 0)                │
    └─────────────────────────────────────┘
```

## Serial Communication

### RFID (SPI)

- Handled by MFRC522 library
- UID read as byte array, converted to hex string
- Example: `AA BB CC DD` → `"AABBCCDD"`

### Nextion (Serial2 @ 9600 baud)

**Command Format** (MCU → Display):

```
Command String + 0xFF 0xFF 0xFF
Example: "page 1" + FF FF FF
```

**Touch Event Format** (Display → MCU):

```
Byte 0:     0x65              (header)
Byte 1:     0x01              (event type: touch)
Byte 2:     ComponentID       (1, 2, or 3)
Byte 3:     0x01              (press/release)
Bytes 4-6:  0xFF 0xFF 0xFF    (terminator)
```

### WiFi & MQTT (SSL/TLS)

- Connects to HiveMQ Cloud on port 8883
- Uses `WiFiClientSecure` with `setInsecure()` for self-signed certificates
- Publishes JSON to `v1/emotion/logs` topic

## Debugging

### Serial Monitor Output

```
=== Smart Emotion Tracker Initializing ===

[Nextion] Serial2 initialized at 9600 baud (RX=16, TX=17)
[RFID] MFRC522 initialized (SS=5, RST=22)
[RFID] Firmware version: 88
[WiFi] Connecting to: KOST BUNDA 1 LANTAI 2
.....
[WiFi] Connected!
[WiFi] IP Address: 192.168.1.100
[MQTT] Configured HiveMQ Cloud connection

=== Initialization Complete ===

Waiting for RFID card scan...
[RFID] Card detected! UID: AB12CD34
[Nextion] Sent command 'page 1'
[Nextion] Touch event, ComponentID: 0x01
[Emotion] Selected: senang
[MQTT] Publishing to v1/emotion/logs: {"card_uid":"AB12CD34","emotion":"senang","timestamp":5432}
[MQTT] Message published successfully
[Nextion] Sent command 'page 0'
```

### Troubleshooting

| Issue                     | Cause                       | Solution                               |
| ------------------------- | --------------------------- | -------------------------------------- |
| No RFID detection         | Wrong pins or antenna issue | Check SS (5) and RST (22) connections  |
| Nextion no response       | Wrong Serial2 pins          | Verify RX=16, TX=17, baud=9600         |
| WiFi not connecting       | Wrong SSID/password         | Update `secrets.h`                     |
| MQTT connection fails     | Bad credentials or network  | Check username/password in `secrets.h` |
| Touch events not received | Protocol mismatch           | Verify Nextion sends 7-byte format     |

## Build & Upload

### Prerequisites

- PlatformIO IDE or CLI
- Python 3.6+

### Build

```bash
pio run -e esp32-s3-devkitc-1
```

### Upload

```bash
pio run -e esp32-s3-devkitc-1 -t upload
```

### Monitor Serial Output

```bash
pio device monitor -b 115200
```

## Libraries Used

| Library                     | Version  | Purpose            |
| --------------------------- | -------- | ------------------ |
| MFRC522                     | 1.4.10   | RFID card reading  |
| PubSubClient                | 2.8      | MQTT communication |
| ArduinoJson                 | 7.0.4    | JSON serialization |
| WiFi (ESP32 core)           | Built-in | WiFi connectivity  |
| SPI (ESP32 core)            | Built-in | SPI communication  |
| HardwareSerial (ESP32 core) | Built-in | UART communication |

## Implementation Notes

### Thread Safety

- No RTOS tasks used; single-threaded event loop
- `loop()` checks MQTT, RFID, and Nextion sequentially
- Debouncing handled by 10ms loop delay

### Memory Management

- Dynamic JSON buffer (256 bytes) for emotion payload
- String objects used for UID storage
- Fixed emotion map array (4 entries)

### Power Considerations

- No sleep modes implemented; continuous operation
- WiFi remains connected during idle state
- RFID antenna draws ~50mA when active

### Error Handling

- MQTT reconnection attempts every 5 seconds
- Invalid Nextion frames ignored (protocol validation)
- WiFi auto-reconnect via Arduino core
- No recovery for critical failures (watchdog reset recommended)

## Future Enhancements

1. **Offline Mode**: Queue emotion data if MQTT disconnected
2. **Feedback**: LED/buzzer confirmation on successful emotion capture
3. **Multiple Profiles**: Support different user ID prefixes on Nextion
4. **Low Power**: Implement sleep/wake via RFID interrupt
5. **Data Logging**: Store emotion history in flash memory
6. **Cloud Sync**: Periodic sync of queued data to cloud

## License

This project is part of the Emoticon Hardware ecosystem.
