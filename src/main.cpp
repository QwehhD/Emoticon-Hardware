#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <MFRC522.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include "secrets.h"

// RFID SPI Configuration for ESP32-S3
#define SS_PIN 5
#define RST_PIN 4
#define SCK_PIN 18
#define MISO_PIN 19
#define MOSI_PIN 13

// Nextion Serial Configuration
#define RXD2 16
#define TXD2 17

MFRC522 rfid(SS_PIN, RST_PIN);
WiFiClientSecure espClient;
PubSubClient client(espClient);

// NTP Client for accurate timestamps
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 25200, 60000); // UTC+7 (Jakarta), update every 60s

#define NEXTION_SERIAL Serial2
#define NEXTION_BAUD 9600

enum State
{
    STATE_IDLE,
    STATE_WAITING_EMOTION,
    STATE_WAITING_UID_VALIDATION
};

State currentState = STATE_IDLE;
String currentCardUID = "";

// MQTT Validation timeout variables
unsigned long rfidValidationStartTime = 0;
const unsigned long VALIDATION_TIMEOUT = 10000; // 10 seconds (increased from 3)
bool validationInProgress = false;

// RFID debouncing variables
unsigned long lastRFIDReadTime = 0;
const unsigned long RFID_DEBOUNCE_TIME = 2000; // 2 seconds between reads

const char *emotionMap[] = {
    nullptr,
    "senang",
    "sedih",
    "marah"};

void setup_wifi();
void reconnect();
void onMqttMessage(char *topic, byte *payload, unsigned int length);
void sendToNextion(uint8_t page);
void handleNextionInput();
void sendEmotionData(String uid, String emotion);
void handleRFID();
void handleRFIDValidationTimeout();
void sendUIDValidationRequest(String uid);
void printHex(byte *buffer, byte bufferSize);

void sendToNextion(uint8_t page)
{
    String command = "page " + String(page);
    NEXTION_SERIAL.print(command);
    NEXTION_SERIAL.write(0xFF);
    NEXTION_SERIAL.write(0xFF);
    NEXTION_SERIAL.write(0xFF);
    Serial.print("[Nextion] Sent command: ");
    Serial.println(command);
}

void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n\n=== Smart Emotion Tracker Initializing ===\n");

    NEXTION_SERIAL.begin(NEXTION_BAUD, SERIAL_8N1, RXD2, TXD2);
    Serial.println("[Nextion] Serial2 initialized at 9600 baud (RX=16, TX=17)");

    // Initialize SPI with custom pins
    SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
    Serial.println("[RFID] SPI initialized (SCK=18, MISO=19, MOSI=13, SS=5)");

    // Initialize RFID
    rfid.PCD_Init();

    // Check chip version for hardware confirmation
    byte chipVersion = rfid.PCD_ReadRegister(rfid.VersionReg);
    Serial.print("[RFID] Chip Version: 0x");
    Serial.println(chipVersion, HEX);

    if (chipVersion == 0x91 || chipVersion == 0x92)
    {
        Serial.println("[RFID] RC522 successfully connected (SPI)!");
    }
    else
    {
        Serial.println("[RFID] WARNING: Check SPI cables or pin connections!");
    }

    setup_wifi();

    // Initialize NTP client if WiFi is connected
    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.println("[NTP] Initializing time client...");
        timeClient.begin();
        timeClient.update();
        Serial.print("[NTP] Current time: ");
        Serial.println(timeClient.getFormattedTime());

        espClient.setInsecure();
        client.setServer(SECRET_MQTT_SERVER, SECRET_MQTT_PORT);
        client.setCallback(onMqttMessage);
        Serial.println("[MQTT] Configured HiveMQ Cloud connection");
    }
    else
    {
        Serial.println("[MQTT] Skipping MQTT setup - WiFi not connected");
    }

    sendToNextion(0);

    Serial.println("\n=== Initialization Complete ===\n");
    Serial.println("Waiting for RFID card scan...");
}

void loop()
{
    // Check WiFi connection first
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[WiFi] Connection lost! Reconnecting...");
        setup_wifi();

        // Reconfigure MQTT after WiFi reconnection
        if (WiFi.status() == WL_CONNECTED)
        {
            espClient.setInsecure();
            client.setServer(SECRET_MQTT_SERVER, SECRET_MQTT_PORT);
            client.setCallback(onMqttMessage);
        }
    }

    if (!client.connected())
    {
        reconnect();
    }
    client.loop();

    // Update NTP time periodically
    timeClient.update();

    handleRFID();

    handleRFIDValidationTimeout();

    handleNextionInput();

    delay(10);
}

void setup_wifi()
{
    Serial.print("\n[WiFi] Connecting to: ");
    Serial.println(SECRET_SSID);

    WiFi.mode(WIFI_STA);
    WiFi.begin(SECRET_SSID, SECRET_PASS);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20)
    {
        delay(500);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.println("\n[WiFi] Connected!");
        Serial.print("[WiFi] IP Address: ");
        Serial.println(WiFi.localIP());
    }
    else
    {
        Serial.println("\n[WiFi] Connection failed!");
    }
}

void reconnect()
{
    if (client.connected())
    {
        return;
    }

    // Check WiFi connection first
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[MQTT] Cannot connect - WiFi not connected");
        return;
    }

    static unsigned long lastAttempt = 0;
    unsigned long now = millis();

    if (now - lastAttempt < 5000)
    {
        return;
    }
    lastAttempt = now;

    Serial.print("[MQTT] Attempting connection to ");
    Serial.print(SECRET_MQTT_SERVER);
    Serial.print(":");
    Serial.print(SECRET_MQTT_PORT);
    Serial.print(" ... ");

    String clientID = "ESP32-Emotion-";
    clientID += String(random(0xffff), HEX);

    if (client.connect(clientID.c_str(), SECRET_MQTT_USER, SECRET_MQTT_PASS))
    {
        Serial.println("✓ Connected!");
        Serial.print("[MQTT] Client ID: ");
        Serial.println(clientID);

        // CRITICAL: Subscribe to auth_status BEFORE sending any check_uid requests
        if (client.subscribe("v1/emotion/auth_status", 1))
        {
            Serial.println("[MQTT] ✓ Subscribed to v1/emotion/auth_status (QoS 1)");
        }
        else
        {
            Serial.println("[MQTT] ✗ Failed to subscribe to v1/emotion/auth_status");
        }
    }
    else
    {
        Serial.print("failed, rc=");
        Serial.println(client.state());
    }
}

void handleRFID()
{
    // Only process RFID when in IDLE state
    if (currentState != STATE_IDLE)
    {
        return;
    }

    if (!rfid.PICC_IsNewCardPresent())
    {
        return;
    }

    if (!rfid.PICC_ReadCardSerial())
    {
        return;
    }

    // Debouncing: prevent reading the same card too quickly
    unsigned long currentTime = millis();
    if (currentTime - lastRFIDReadTime < RFID_DEBOUNCE_TIME)
    {
        Serial.println("[RFID] Debouncing - ignoring rapid re-scan");
        rfid.PICC_HaltA();
        rfid.PCD_StopCrypto1();
        return;
    }
    lastRFIDReadTime = currentTime;

    String cardUID = "";
    for (byte i = 0; i < rfid.uid.size; i++)
    {
        if (rfid.uid.uidByte[i] < 0x10)
        {
            cardUID += "0";
        }
        cardUID += String(rfid.uid.uidByte[i], HEX);
    }
    cardUID.toUpperCase();

    Serial.print("[RFID] Card detected! UID: ");
    Serial.println(cardUID);

    // Send UID to MQTT for validation
    currentCardUID = cardUID;
    sendUIDValidationRequest(cardUID);

    currentState = STATE_WAITING_UID_VALIDATION;
    rfidValidationStartTime = millis();
    validationInProgress = true;

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
}

void sendEmotionData(String uid, String emotion)
{
    if (!client.connected())
    {
        Serial.println("[MQTT] Not connected, cannot send emotion data");
        return;
    }

    DynamicJsonDocument doc(256);
    doc["card_uid"] = uid;
    doc["emotion"] = emotion;
    
    // CRITICAL: Use Unix timestamp in SECONDS (not milliseconds)
    unsigned long timestamp = timeClient.getEpochTime();
    doc["timestamp"] = timestamp;

    String payload;
    serializeJson(doc, payload);

    const char *topic = "v1/emotion/logs";

    Serial.print("[MQTT] Publishing to ");
    Serial.print(topic);
    Serial.print(": ");
    Serial.println(payload);
    Serial.print("[MQTT] Timestamp (seconds): ");
    Serial.println(timestamp);

    // Publish with QoS 1 for reliability
    if (client.publish(topic, payload.c_str(), false))
    {
        Serial.println("[MQTT] ✓ Emotion log sent successfully");
    }
    else
    {
        Serial.println("[MQTT] ✗ Failed to publish emotion log");
    }
}

void printHex(byte *buffer, byte bufferSize)
{
    for (byte i = 0; i < bufferSize; i++)
    {
        Serial.print(buffer[i] < 0x10 ? " 0" : " ");
        Serial.print(buffer[i], HEX);
    }
}

void sendUIDValidationRequest(String uid)
{
    if (!client.connected())
    {
        Serial.println("[MQTT] ✗ Not connected, cannot send validation request");
        return;
    }

    StaticJsonDocument<128> doc;
    doc["uid"] = uid;

    String payload;
    serializeJson(doc, payload);

    const char *topic = "v1/emotion/check_uid";

    Serial.print("[MQTT] Publishing UID validation to ");
    Serial.print(topic);
    Serial.print(": ");
    Serial.println(payload);

    // CRITICAL: Publish with QoS 1 (retained=false) for guaranteed delivery
    if (client.publish(topic, payload.c_str(), false))
    {
        Serial.println("[MQTT] ✓ UID validation request sent");
        Serial.println("[MQTT] Waiting for response on v1/emotion/auth_status...");
    }
    else
    {
        Serial.println("[MQTT] ✗ Failed to send validation request");
        Serial.print("[MQTT] Client state: ");
        Serial.println(client.state());
    }
}

void onMqttMessage(char *topic, byte *payload, unsigned int length)
{
    Serial.print("[MQTT] ✓ Message received on topic: ");
    Serial.println(topic);
    
    // Convert payload to string
    String message = "";
    for (unsigned int i = 0; i < length; i++)
    {
        message += (char)payload[i];
    }
    Serial.print("[MQTT] Payload: ");
    Serial.println(message);

    // Check if this is the auth_status topic
    if (strcmp(topic, "v1/emotion/auth_status") != 0)
    {
        Serial.println("[MQTT] Ignoring message from non-auth_status topic");
        return;
    }

    // Only process if we're waiting for validation
    if (currentState != STATE_WAITING_UID_VALIDATION || !validationInProgress)
    {
        Serial.println("[MQTT] Received auth_status but not waiting for validation");
        return;
    }

    // Parse JSON payload
    StaticJsonDocument<128> doc;
    DeserializationError error = deserializeJson(doc, message);

    if (error)
    {
        Serial.print("[MQTT] ✗ JSON parse error: ");
        Serial.println(error.c_str());
        return;
    }

    // Check if 'valid' field exists
    if (!doc.containsKey("valid"))
    {
        Serial.println("[MQTT] ✗ Response missing 'valid' field");
        return;
    }

    bool isValid = doc["valid"].as<bool>();

    validationInProgress = false;

    if (isValid)
    {
        Serial.println("[MQTT] ✓ UID is VALID - Access granted");
        Serial.println("[UI] Showing emotion selection screen");
        currentState = STATE_WAITING_EMOTION;
        sendToNextion(1);
    }
    else
    {
        Serial.println("[MQTT] ✗ UID is INVALID - Access denied");
        Serial.println("[UI] Card not registered");
        currentState = STATE_IDLE;
        currentCardUID = "";
        sendToNextion(0);
    }
}

void handleRFIDValidationTimeout()
{
    // Only check timeout if validation is in progress
    if (!validationInProgress || currentState != STATE_WAITING_UID_VALIDATION)
    {
        return;
    }

    unsigned long currentTime = millis();
    unsigned long elapsedTime = currentTime - rfidValidationStartTime;

    if (elapsedTime >= VALIDATION_TIMEOUT)
    {
        Serial.print("[MQTT] ✗ ERROR: No response from server within ");
        Serial.print(VALIDATION_TIMEOUT);
        Serial.println(" ms - Timeout!");
        Serial.println("[MQTT] Possible causes:");
        Serial.println("  1. Backend not listening to v1/emotion/check_uid");
        Serial.println("  2. Backend not publishing to v1/emotion/auth_status");
        Serial.println("  3. MQTT connection issue");
        Serial.print("[MQTT] Connection status: ");
        Serial.println(client.connected() ? "Connected" : "Disconnected");

        validationInProgress = false;
        currentState = STATE_IDLE;
        currentCardUID = "";

        // Reset RFID debounce timer to allow immediate retry
        lastRFIDReadTime = 0;

        // Show error message and return to page 0
        sendToNextion(0);
    }
}

void handleNextionInput()
{
    if (!NEXTION_SERIAL.available())
    {
        return;
    }

    uint8_t header = NEXTION_SERIAL.read();
    if (header != 0x65)
    {
        // Flush remaining bytes if header is invalid
        while (NEXTION_SERIAL.available())
        {
            NEXTION_SERIAL.read();
        }
        return;
    }

    uint8_t data[6];
    for (int i = 0; i < 6; i++)
    {
        if (NEXTION_SERIAL.available())
        {
            data[i] = NEXTION_SERIAL.read();
        }
        else
        {
            return;
        }
    }

    if (data[0] != 0x01 || data[2] != 0x01 ||
        data[3] != 0xFF || data[4] != 0xFF || data[5] != 0xFF)
    {
        return;
    }

    uint8_t componentID = data[1];

    Serial.print("[Nextion] Touch event, ComponentID: 0x");
    Serial.println(componentID, HEX);

    if (currentState == STATE_WAITING_EMOTION && componentID >= 1 && componentID <= 3)
    {
        String emotion = String(emotionMap[componentID]);

        Serial.print("[Emotion] Selected: ");
        Serial.println(emotion);

        sendEmotionData(currentCardUID, emotion);

        sendToNextion(0);

        currentState = STATE_IDLE;
        currentCardUID = "";
    }
}
