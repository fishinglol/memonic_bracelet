#include <Arduino.h>
#include <driver/i2s.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "secrets.h"  // WIFI_SSID, WIFI_PASSWORD

// ==========================================
// CONFIG
// ==========================================
const char* SERVER_URL = "https://8001-01kkh2et3bdjymj2fjq6jabg8k.cloudspaces.litng.ai/api/esp32-audio";

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
// SEND AUDIO VIA HTTP POST (with retry)
// ==========================================
bool sendAudio(uint8_t* wavBuf, uint32_t wavSize) {
    for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
        Serial.printf("📤 Sending attempt %d/%d (%u bytes)...\n", attempt, MAX_RETRIES, wavSize);

        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("   WiFi lost — reconnecting...");
            connectWiFi();
        }

        WiFiClientSecure client;
        client.setInsecure();

        HTTPClient http;
        http.setTimeout(HTTP_TIMEOUT_MS);

        if (!http.begin(client, SERVER_URL)) {
            Serial.println("   ❌ http.begin() failed");
            delay(2000);
            continue;
        }

        http.addHeader("Content-Type", "application/octet-stream");

        int httpCode = http.POST(wavBuf, wavSize);

        if (httpCode > 0) {
            String response = http.getString();
            Serial.printf("   ✅ Server responded (%d):\n", httpCode);
            Serial.println(response);
            http.end();
            return true;
        } else {
            Serial.printf("   ❌ HTTP error: %s (code %d)\n",
                          http.errorToString(httpCode).c_str(), httpCode);
            http.end();

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
    sendAudio(wavBuf, wavSize);

    // Free buffer
    free(wavBuf);
    Serial.printf("Free heap after free: %u bytes\n", ESP.getFreeHeap());
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
    Serial.println("  (HTTP POST + Retry)");
    Serial.println("============================");
    Serial.printf("Total heap: %u bytes\n", ESP.getHeapSize());
    Serial.printf("Free heap:  %u bytes\n", ESP.getFreeHeap());

    connectWiFi();
    initI2S();

    Serial.printf("Free heap after init: %u bytes\n", ESP.getFreeHeap());
    Serial.println("Ready!\n");
}

// ==========================================
// LOOP
// ==========================================
void loop() {
    static unsigned long lastRecord = 0;

    if (millis() - lastRecord > RECORD_INTERVAL_MS) {
        recordAndSend();
        lastRecord = millis();
    }
}