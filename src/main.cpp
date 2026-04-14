#include <Arduino.h>
#include <driver/i2s.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "secrets.h"  // WIFI_SSID, WIFI_PASSWORD

// ==========================================
// CONFIG
// ==========================================
const char* SERVER_HOST = "8001-01kkh2et3bdjymj2fjq6jabg8k.cloudspaces.litng.ai";
const int   SERVER_PORT = 443;
const char* AUDIO_PATH  = "/api/esp32-audio";
const char* ENROLL_PATH = "/api/esp32-enroll/";

// Audio
#define I2S_SCK   5
#define I2S_WS    4
#define I2S_SD    6
#define I2S_PORT  I2S_NUM_0
#define SAMPLE_RATE    16000
#define RECORD_SECONDS 5
#define ENROLL_SECONDS 7

// Timing
#define RECORD_INTERVAL_MS  15000

// ==========================================
// I2S INIT
// ==========================================
void initI2S() {
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate          = SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 8,
        .dma_buf_len          = 256,
        .use_apll             = false,
    };
    i2s_pin_config_t pins = {
        .bck_io_num   = I2S_SCK,
        .ws_io_num    = I2S_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = I2S_SD,
    };
    i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
    i2s_set_pin(I2S_PORT, &pins);
    i2s_start(I2S_PORT);
}

// ==========================================
// WIFI
// ==========================================
void connectWiFi() {
    Serial.printf("Connecting to WiFi: %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\nWiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("\nWiFi FAILED — restarting...");
        ESP.restart();
    }
}

// ==========================================
// WAV HEADER
// ==========================================
void writeWavHeader(uint8_t* buf, uint32_t dataSize) {
    uint32_t byteRate = SAMPLE_RATE * 1 * 16 / 8;
    uint32_t fileSize = 36 + dataSize;
    
    memcpy(buf + 0, "RIFF", 4);
    memcpy(buf + 4, &fileSize, 4);
    memcpy(buf + 8, "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    uint32_t subchunk1Size = 16;
    memcpy(buf + 16, &subchunk1Size, 4);
    uint16_t audioFormat = 1;
    memcpy(buf + 20, &audioFormat, 2);
    uint16_t channels = 1;
    memcpy(buf + 22, &channels, 2);
    uint32_t sr = SAMPLE_RATE;
    memcpy(buf + 24, &sr, 4);
    memcpy(buf + 28, &byteRate, 4);
    uint16_t blockAlign = 2;
    memcpy(buf + 32, &blockAlign, 2);
    uint16_t bps = 16;
    memcpy(buf + 34, &bps, 2);
    memcpy(buf + 36, "data", 4);
    memcpy(buf + 40, &dataSize, 4);
}

// ==========================================
// STREAM AUDIO ON THE FLY (NO MALLOC!)
// Records directly into HTTPS socket chunk by chunk
// ==========================================
void recordAndStreamAudio(const char* urlPath, int durationSecs) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi lost, reconnecting...");
        connectWiFi();
    }

    uint32_t totalSamples = SAMPLE_RATE * durationSecs;
    uint32_t dataSize = totalSamples * sizeof(int16_t);
    uint32_t contentLen = 44 + dataSize;

    Serial.printf("\n════════════════════════════════\n");
    Serial.printf("🎤 Streaming %d seconds to %s...\n", durationSecs, urlPath);
    Serial.printf("   Free heap: %u bytes\n", ESP.getFreeHeap());

    WiFiClientSecure client;
    client.setInsecure(); // Skip SSL cert check
    client.setTimeout(60);

    Serial.print("   Connecting to server... ");
    if (!client.connect(SERVER_HOST, SERVER_PORT)) {
        Serial.println("❌ FAILED!");
        return;
    }
    Serial.println("OK!");

    // Send HTTP POST headers
    client.printf("POST %s HTTP/1.1\r\n", urlPath);
    client.printf("Host: %s\r\n", SERVER_HOST);
    client.printf("Content-Type: application/octet-stream\r\n");
    client.printf("Content-Length: %u\r\n", contentLen);
    client.printf("Connection: close\r\n\r\n");

    // Send WAV Header (44 bytes)
    uint8_t header[44];
    writeWavHeader(header, dataSize);
    client.write(header, 44);

    // Stream audio on-the-fly
    int32_t i2sBuf[256];
    int16_t sampleBuf[256];
    uint32_t samplesSent = 0;

    while (samplesSent < totalSamples) {
        size_t bytesRead = 0;
        i2s_read(I2S_PORT, i2sBuf, sizeof(i2sBuf), &bytesRead, portMAX_DELAY);
        int count = bytesRead / sizeof(int32_t);

        int remaining = totalSamples - samplesSent;
        if (count > remaining) count = remaining;

        // Convert 32-bit I2S to 16-bit PCM
        for (int i = 0; i < count; i++) {
            sampleBuf[i] = (int16_t)(i2sBuf[i] >> 16);
        }

        client.write((const uint8_t*)sampleBuf, count * sizeof(int16_t));
        samplesSent += count;
    }

    Serial.println("   ✅ Audio stream finished. Waiting for result...");

    // Read response line (e.g., HTTP/1.1 200 OK)
    String responseLine = client.readStringUntil('\n');
    responseLine.trim();
    
    // Skip remaining headers
    while (client.connected()) {
        String line = client.readStringUntil('\n');
        if (line == "\r" || line == "") break; 
    }

    // Read full body JSON
    String responseBody = "";
    while (client.connected() || client.available()) {
        if (client.available()) {
            responseBody += (char)client.read();
        }
    }
    client.stop();

    if (responseLine.indexOf("200 OK") != -1) {
        Serial.println("   🎉 SUCCESS!");
        Serial.println(responseBody);
    } else {
        Serial.printf("   ❌ ERROR: %s\n", responseLine.c_str());
        Serial.println(responseBody);
    }
    Serial.printf("════════════════════════════════\n\n");
}

// ==========================================
// SETUP
// ==========================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n============================");
    Serial.println("  Memonic Bracelet (Streaming)");
    Serial.println("============================");
    Serial.println("Commands (type in Serial Monitor):");
    Serial.println("  ENROLL <name>  — Record a voice profile");
    Serial.println("============================");

    connectWiFi();
    initI2S();

    Serial.println("\n✅ Boot Complete! Ready to stream.\n");
}

// ==========================================
// LOOP
// ==========================================
void loop() {
    static unsigned long lastRecord = 0;

    // Check for Serial commands for Enrollment mode
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();

        if (cmd.startsWith("ENROLL ") || cmd.startsWith("enroll ")) {
            String userId = cmd.substring(7);
            userId.trim();

            Serial.printf("\n🎙️ ENROLLMENT MODE: '%s'\n", userId.c_str());
            Serial.println("Starting in 3..."); delay(1000);
            Serial.println("2..."); delay(1000);
            Serial.println("1..."); delay(1000);
            
            String enrollPathStr = String(ENROLL_PATH) + userId;
            recordAndStreamAudio(enrollPathStr.c_str(), ENROLL_SECONDS);
            
            lastRecord = millis(); // restart auto-record timer
        }
    }

    // Auto-record every 15 seconds
    if (millis() - lastRecord > RECORD_INTERVAL_MS) {
        recordAndStreamAudio(AUDIO_PATH, RECORD_SECONDS);
        lastRecord = millis();
    }
}