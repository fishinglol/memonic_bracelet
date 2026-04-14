#include <Arduino.h>
#include <driver/i2s.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "secrets.h"  // WIFI_SSID, WIFI_PASSWORD

// ==========================================
// CONFIG
// ==========================================
const char* SERVER_HOST = "8001-01kkh2et3bdjymj2fjq6jabg8k.cloudspaces.litng.ai";
const int   SERVER_PORT = 443;
const char* AUDIO_PATH  = "/api/esp32-audio";
const char* ENROLL_PATH = "/api/esp32-enroll/";

// Enrollment config
#define ENROLL_SECONDS  7
#define ENROLL_SAMPLES  (SAMPLE_RATE * ENROLL_SECONDS)

// Audio config
#define I2S_SCK   5
#define I2S_WS    4
#define I2S_SD    6
#define I2S_PORT  I2S_NUM_0
#define SAMPLE_RATE    16000
#define RECORD_SECONDS 5
#define SAMPLES_TOTAL  (SAMPLE_RATE * RECORD_SECONDS)

// Timing & retry
#define RECORD_INTERVAL_MS  15000
#define MAX_RETRIES         3
#define HTTP_TIMEOUT_MS     60000  // 60s — AI processing takes time

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
// WAV HEADER — 44 bytes for 16-bit mono PCM
// ==========================================
void writeWavHeader(uint8_t* buf, uint32_t dataSize, uint32_t sampleRate) {
    uint32_t fileSize = 36 + dataSize;
    uint16_t channels = 1;
    uint16_t bitsPerSample = 16;
    uint32_t byteRate = sampleRate * channels * bitsPerSample / 8;
    uint16_t blockAlign = channels * bitsPerSample / 8;

    memcpy(buf + 0,  "RIFF", 4);
    memcpy(buf + 4,  &fileSize, 4);
    memcpy(buf + 8,  "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    uint32_t subchunk1Size = 16;
    memcpy(buf + 16, &subchunk1Size, 4);
    uint16_t audioFormat = 1;
    memcpy(buf + 20, &audioFormat, 2);
    memcpy(buf + 22, &channels, 2);
    memcpy(buf + 24, &sampleRate, 4);
    memcpy(buf + 28, &byteRate, 4);
    memcpy(buf + 32, &blockAlign, 2);
    memcpy(buf + 34, &bitsPerSample, 2);
    memcpy(buf + 36, "data", 4);
    memcpy(buf + 40, &dataSize, 4);
}

// ==========================================
// RECORD AUDIO INTO BUFFER
// ==========================================
int recordAudio(int16_t* audioData, int maxSamples) {
    int32_t i2sBuf[256];
    int totalSamples = 0;

    // Print first few raw I2S values for mic debugging
    bool printedDebug = false;

    while (totalSamples < maxSamples) {
        size_t bytesRead = 0;
        i2s_read(I2S_PORT, i2sBuf, sizeof(i2sBuf), &bytesRead, portMAX_DELAY);
        int count = bytesRead / sizeof(int32_t);

        int remaining = maxSamples - totalSamples;
        if (count > remaining) count = remaining;

        for (int i = 0; i < count; i++) {
            audioData[totalSamples + i] = (int16_t)(i2sBuf[i] >> 16);
        }

        // Debug: print first 10 raw I2S values to check mic output
        if (!printedDebug && totalSamples == 0) {
            Serial.print("   Raw I2S[0..9]: ");
            for (int i = 0; i < 10 && i < count; i++) {
                Serial.printf("%d ", i2sBuf[i]);
            }
            Serial.println();
            Serial.print("   PCM[0..9]:     ");
            for (int i = 0; i < 10 && i < count; i++) {
                Serial.printf("%d ", audioData[i]);
            }
            Serial.println();
            printedDebug = true;
        }

        totalSamples += count;
    }

    // Audio stats
    int32_t minVal = 32767, maxVal = -32768;
    int64_t sumSq = 0;
    for (int i = 0; i < totalSamples; i++) {
        int16_t s = audioData[i];
        if (s < minVal) minVal = s;
        if (s > maxVal) maxVal = s;
        sumSq += (int64_t)s * s;
    }
    float rms = sqrt((float)sumSq / totalSamples);
    Serial.printf("   Audio stats — Min: %d, Max: %d, RMS: %.1f\n", minVal, maxVal, rms);

    if (rms < 10) {
        Serial.println("   ⚠️  Audio is nearly SILENT! Check mic wiring.");
    } else if (rms < 100) {
        Serial.println("   ⚠️  Audio is very quiet. Mic gain may be too low.");
    } else {
        Serial.println("   ✅ Audio level looks good!");
    }

    return totalSamples;
}

// ==========================================
// SEND AUDIO VIA RAW HTTPS POST (Bypasses HTTPClient bugs)
// ==========================================
bool sendAudioRaw(const char* path, uint8_t* wavBuf, uint32_t wavSize) {
    for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
        Serial.printf("📤 Sending attempt %d/%d (%u bytes) to %s \n", attempt, MAX_RETRIES, wavSize, path);

        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("   WiFi lost — reconnecting...");
            connectWiFi();
        }

        WiFiClientSecure client;
        client.setInsecure();
        client.setTimeout(60); // 60s timeout for AI processing

        if (!client.connect(SERVER_HOST, SERVER_PORT)) {
            Serial.println("   ❌ HTTPS connection FAILED!");
            delay(2000);
            continue;
        }

        // Send HTTP headers manually
        client.printf("POST %s HTTP/1.1\r\n", path);
        client.printf("Host: %s\r\n", SERVER_HOST);
        client.printf("Content-Type: application/octet-stream\r\n");
        client.printf("Content-Length: %u\r\n", wavSize);
        client.printf("Connection: close\r\n");
        client.printf("\r\n");

        // Send body in chunks so we don't blow up the WiFiClient buffer
        uint32_t offset = 0;
        while (offset < wavSize) {
            uint32_t chunkSize = wavSize - offset;
            if (chunkSize > 4096) chunkSize = 4096;
            client.write(wavBuf + offset, chunkSize);
            offset += chunkSize;
        }

        Serial.println("   ✅ Uploaded. Waiting for server response...");

        // Read response
        String responseLine = client.readStringUntil('\n');
        responseLine.trim();
        
        // Skip remaining headers
        while (client.connected()) {
            String line = client.readStringUntil('\n');
            if (line == "\r" || line == "") break; 
        }

        // Read body
        String responseBody = "";
        while (client.available()) {
            responseBody += (char)client.read();
        }
        client.stop();

        if (responseLine.indexOf("200 OK") != -1) {
            Serial.println("   ✅ Server responded (200):");
            Serial.println(responseBody);
            return true;
        } else {
            Serial.printf("   ❌ Server error: %s\n", responseLine.c_str());
            Serial.println(responseBody);

            if (attempt < MAX_RETRIES) {
                int waitMs = attempt * 3000;
                Serial.printf("   Retrying in %d seconds...\n", waitMs / 1000);
                delay(waitMs);
            }
        }
    }

    Serial.println("   ❌ All retries failed!");
    return false;
}

// ==========================================
// MAIN: RECORD + SEND
// ==========================================
void recordAndSend() {
    Serial.printf("\n════════════════════════════════\n");
    Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());

    uint32_t dataSize = SAMPLES_TOTAL * sizeof(int16_t);  // 160000 bytes
    uint32_t wavSize  = 44 + dataSize;

    // Allocate WAV buffer
    uint8_t* wavBuf = (uint8_t*)malloc(wavSize);
    if (!wavBuf) {
        Serial.printf("❌ Cannot allocate %u bytes! (free: %u)\n", wavSize, ESP.getFreeHeap());
        return;
    }
    Serial.printf("Allocated %u bytes for WAV (free: %u)\n", wavSize, ESP.getFreeHeap());

    // Write WAV header
    writeWavHeader(wavBuf, dataSize, SAMPLE_RATE);

    // Record audio
    Serial.printf("🎤 Recording %d seconds...\n", RECORD_SECONDS);
    int16_t* audioData = (int16_t*)(wavBuf + 44);
    int samples = recordAudio(audioData, SAMPLES_TOTAL);
    Serial.printf("   Recorded %d samples\n", samples);

    // Send with retry
    sendAudioRaw(AUDIO_PATH, wavBuf, wavSize);

    // Free buffer
    free(wavBuf);
    Serial.printf("Free heap after free: %u bytes\n", ESP.getFreeHeap());
    Serial.printf("════════════════════════════════\n\n");
}

// ==========================================
// ENROLL VOICE via Serial command
//
// Type "ENROLL fish" in Serial Monitor
// Records 7 seconds, sends to /api/esp32-enroll/fish
// ==========================================
void enrollVoice(String userId) {
    userId.trim();
    if (userId.length() == 0) {
        Serial.println("❌ Usage: ENROLL <username>");
        Serial.println("   Example: ENROLL fish");
        return;
    }

    Serial.printf("\n════════════════════════════════\n");
    Serial.printf("🎙️  ENROLLMENT MODE: '%s'\n", userId.c_str());
    Serial.printf("════════════════════════════════\n");
    Serial.println("Speak clearly for 7 seconds...");
    Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());

    uint32_t dataSize = ENROLL_SAMPLES * sizeof(int16_t);
    uint32_t wavSize  = 44 + dataSize;

    uint8_t* wavBuf = (uint8_t*)malloc(wavSize);
    if (!wavBuf) {
        Serial.printf("❌ Cannot allocate %u bytes!\n", wavSize);
        return;
    }

    writeWavHeader(wavBuf, dataSize, SAMPLE_RATE);

    // Countdown
    Serial.println("Starting in...");
    for (int i = 3; i > 0; i--) {
        Serial.printf("  %d...\n", i);
        delay(1000);
    }
    Serial.println("🎤 RECORDING NOW — speak clearly!");

    int16_t* audioData = (int16_t*)(wavBuf + 44);
    int samples = recordAudio(audioData, ENROLL_SAMPLES);
    Serial.printf("   Recorded %d samples (%.1fs)\n", samples, (float)samples / SAMPLE_RATE);

    // Build enrollment Path
    String enrollPath = String(ENROLL_PATH) + userId;
    
    // Send 
    sendAudioRaw(enrollPath.c_str(), wavBuf, wavSize);

    free(wavBuf);
    Serial.printf("Free heap after: %u bytes\n", ESP.getFreeHeap());
    Serial.printf("════════════════════════════════\n\n");
}

// ==========================================
// SETUP
// ==========================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n============================");
    Serial.println("  Memonic Bracelet v2");
    Serial.println("============================");
    Serial.println("Commands (type in Serial Monitor):");
    Serial.println("  ENROLL <name>  — Record 7s to enroll voice");
    Serial.println("  (auto-records every 15s otherwise)");
    Serial.println("============================");
    Serial.printf("Total heap: %u bytes\n", ESP.getHeapSize());
    Serial.printf("Free heap:  %u bytes\n", ESP.getFreeHeap());

    connectWiFi();
    initI2S();

    Serial.printf("Free heap after init: %u bytes\n", ESP.getFreeHeap());
    Serial.println("Ready!\n");
}

// ==========================================
// LOOP — checks Serial for commands, auto-records otherwise
// ==========================================
void loop() {
    static unsigned long lastRecord = 0;

    // Check for Serial commands
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();

        if (cmd.startsWith("ENROLL ") || cmd.startsWith("enroll ")) {
            String userId = cmd.substring(7);
            enrollVoice(userId);
            lastRecord = millis();  // Reset timer after enrollment
        } else if (cmd.length() > 0) {
            Serial.printf("Unknown command: '%s'\n", cmd.c_str());
            Serial.println("Available: ENROLL <name>");
        }
    }

    // Auto-record at interval
    if (millis() - lastRecord > RECORD_INTERVAL_MS) {
        recordAndSend();
        lastRecord = millis();
    }
}