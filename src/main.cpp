#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>

// Definisi Pin SPI untuk ESP32-S3
#define SS_PIN    5   
#define RST_PIN   4   
#define SCK_PIN   18
#define MISO_PIN  19
#define MOSI_PIN  13

MFRC522 rfid(SS_PIN, RST_PIN);

void setup() {
    Serial.begin(115200);
    delay(2000);
    
    // Memulai SPI dengan pin kustom
    SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
    
    // Inisialisasi RFID
    rfid.PCD_Init();
    
    // Cek versi chip untuk memastikan koneksi hardware
    byte v = rfid.PCD_ReadRegister(rfid.VersionReg);
    Serial.print("RFID Chip Version: 0x");
    Serial.println(v, HEX);

    if (v == 0x91 || v == 0x92) {
        Serial.println("RC522 Berhasil Terhubung (SPI)!");
    } else {
        Serial.println("Gagal! Cek kabel SPI atau pastikan pin sudah pas.");
    }
}

void loop() {
    if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
        return;
    }

    Serial.print("UID: ");
    for (byte i = 0; i < rfid.uid.size; i++) {
        Serial.print(rfid.uid.uidByte[i] < 0x10 ? " 0" : " ");
        Serial.print(rfid.uid.uidByte[i], HEX);
    }
    Serial.println();

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
}