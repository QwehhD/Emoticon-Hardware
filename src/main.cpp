#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <MFRC522.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include "secrets.h"

#define SS_PIN 5
#define RST_PIN 22
#define RXD2 16
#define TXD2 17

MFRC522 rfid(SS_PIN, RST_PIN);
WiFiClientSecure espClient;
PubSubClient client(espClient);

#define NEXTION_SERIAL Serial2
#define NEXTION_BAUD 9600

enum State {
  STATE_IDLE,
  STATE_WAITING_EMOTION
};

State currentState = STATE_IDLE;
String currentCardUID = "";

const char* emotionMap[] = {
  nullptr,
  "senang",
  "sedih",
  "marah"
};

void setup_wifi();
void reconnect();
void sendToNextion(uint8_t page);
void handleNextionInput();
void sendEmotionData(String uid, String emotion);
void handleRFID();
void printHex(byte* buffer, byte bufferSize);

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n\n=== Smart Emotion Tracker Initializing ===\n");
    
    NEXTION_SERIAL.begin(NEXTION_BAUD, SERIAL_8N1, RXD2, TXD2);
    Serial.println("[Nextion] Serial2 initialized at 9600 baud (RX=16, TX=17)");
    
    SPI.begin();
    
    rfid.PCD_Init();
    Serial.println("[RFID] MFRC522 initialized (SS=5, RST=22)");
    Serial.print("[RFID] Firmware version: ");
    Serial.println(rfid.PCD_GetAntennaGain(), HEX);
    
    setup_wifi();
    
    espClient.setInsecure();
    client.setServer(SECRET_MQTT_SERVER, SECRET_MQTT_PORT);
    Serial.println("[MQTT] Configured HiveMQ Cloud connection");

    sendToNextion(0);
    
    Serial.println("\n=== Initialization Complete ===\n");
    Serial.println("Waiting for RFID card scan...");
}

void loop() {
    if (!client.connected()) {
        reconnect();
    }
    client.loop();

    handleRFID();

    handleNextionInput();
    
    delay(10);
}

void setup_wifi() {
    Serial.print("\n[WiFi] Connecting to: ");
    Serial.println(SECRET_SSID);
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(SECRET_SSID, SECRET_PASS);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n[WiFi] Connected!");
        Serial.print("[WiFi] IP Address: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("\n[WiFi] Connection failed!");
    }
}

void reconnect() {
    if (client.connected()) {
        return;
    }
    
    static unsigned long lastAttempt = 0;
    unsigned long now = millis();
    
    if (now - lastAttempt < 5000) {
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
    
    if (client.connect(clientID.c_str(), SECRET_MQTT_USER, SECRET_MQTT_PASS)) {
        Serial.println("connected!");
        Serial.print("[MQTT] Client ID: ");
        Serial.println(clientID);
    } else {
        Serial.print("failed, rc=");
        Serial.println(client.state());
    }
}

void handleRFID() {
    if (!rfid.PICC_IsNewCardPresent()) {
        return;
    }
    
    if (!rfid.PICC_ReadCardSerial()) {
        return;
    }
    
    String cardUID = "";
    for (byte i = 0; i < rfid.uid.size; i++) {
        if (rfid.uid.uidByte[i] < 0x10) {
            cardUID += "0";
        }
        cardUID += String(rfid.uid.uidByte[i], HEX);
    }
    cardUID.toUpperCase();
    
    Serial.print("[RFID] Card detected! UID: ");
    Serial.println(cardUID);
    
    currentCardUID = cardUID;
    currentState = STATE_WAITING_EMOTION;
    
    sendToNextion(1);
    
    rfid.PICC_HaltA();
    
    rfid.PCD_StopCrypto1();
}

void sendToNextion(uint8_t page) {
    String command = "page ";
    command += String(page);
    
    NEXTION_SERIAL.write(command.c_str());
    NEXTION_SERIAL.write(0xFF);
    NEXTION_SERIAL.write(0xFF);
    NEXTION_SERIAL.write(0xFF);
    
    Serial.print("[Nextion] Sent command 'page ");
    Serial.print(page);
    Serial.println("'");
}

void handleNextionInput() {
    if (NEXTION_SERIAL.available() < 7) {
        return;
    }
    
    uint8_t header = NEXTION_SERIAL.read();
    
    if (header != 0x65) {
        while (NEXTION_SERIAL.available()) {
            NEXTION_SERIAL.read();
        }
        return;
    }
    
    uint8_t data[6];
    for (int i = 0; i < 6; i++) {
        if (NEXTION_SERIAL.available()) {
            data[i] = NEXTION_SERIAL.read();
        } else {
            return;
        }
    }
    
    if (data[0] != 0x01 || data[2] != 0x01 || 
        data[3] != 0xFF || data[4] != 0xFF || data[5] != 0xFF) {
        return;
    }
    
    uint8_t componentID = data[1];
    
    Serial.print("[Nextion] Touch event, ComponentID: 0x");
    Serial.println(componentID, HEX);
    
    if (currentState == STATE_WAITING_EMOTION && componentID >= 1 && componentID <= 3) {
        String emotion = String(emotionMap[componentID]);
        
        Serial.print("[Emotion] Selected: ");
        Serial.println(emotion);
        
        sendEmotionData(currentCardUID, emotion);
        
        sendToNextion(0);
        
        currentState = STATE_IDLE;
        currentCardUID = "";
    }
}

void sendEmotionData(String uid, String emotion) {
    if (!client.connected()) {
        Serial.println("[MQTT] Not connected, cannot send emotion data");
        return;
    }
    
    DynamicJsonDocument doc(256);
    doc["card_uid"] = uid;
    doc["emotion"] = emotion;
    doc["timestamp"] = millis();
    
    String payload;
    serializeJson(doc, payload);
    
    const char* topic = "v1/emotion/logs";
    
    Serial.print("[MQTT] Publishing to ");
    Serial.print(topic);
    Serial.print(": ");
    Serial.println(payload);
    
    if (client.publish(topic, payload.c_str())) {
        Serial.println("[MQTT] Message published successfully");
    } else {
        Serial.println("[MQTT] Failed to publish message");
    }
}

void printHex(byte* buffer, byte bufferSize) {
    for (byte i = 0; i < bufferSize; i++) {
        Serial.print(buffer[i] < 0x10 ? " 0" : " ");
        Serial.print(buffer[i], HEX);
    }
}
