#include <Arduino.h>
#include <driver/i2s.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

// CONFIG — Update credentials in src/secrets.h
// ==========================================
#include "secrets.h"

// Lightning AI server
const char* SERVER_HOST = "8001-01kkh2et3bdjymj2fjq6jabg8k.cloudspaces.litng.ai";
const char* AUDIO_PATH  = "/api/esp32-audio";
const char* HEARTBEAT_PATH = "/update";
const int   SERVER_PORT = 443;  // HTTPS

// Audio config
#define I2S_SCK   5
#define I2S_WS    4
#define I2S_SD    6
#define I2S_PORT  I2S_NUM_0
#define SAMPLE_RATE    16000
#define RECORD_SECONDS 5
#define SAMPLES_TOTAL  (SAMPLE_RATE * RECORD_SECONDS)

// ==========================================
// TIMING — millis()-based, non-blocking
//
// Heartbeat keeps the UI showing "Connected"
// even between audio recordings.
// ==========================================
#define HEARTBEAT_INTERVAL_MS 10000   // ping server every 10s
#define RECORD_INTERVAL_MS    20000   // record every 20s

unsigned long lastHeartbeat = 0;
unsigned long lastRecording = 0;

// ==========================================
// WAV HEADER — 44 bytes for 16-bit mono PCM
// ==========================================
void buildWavHeader(uint8_t* buf, uint32_t dataSize, uint32_t sampleRate) {
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
    uint16_t audioFormat = 1;  // PCM
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
// WIFI — auto-reconnect, never crash
// ==========================================
void ensureWiFi() {
    if (WiFi.status() == WL_CONNECTED) return;

    Serial.printf("Connecting to WiFi: %s", WIFI_SSID);
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\nWiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("\nWiFi not ready — will retry next loop");
    }
}

// ==========================================
// HEARTBEAT — tiny POST to /update
//
// Keeps the UI showing "Connected" between
// audio recordings.  Uses minimal RAM.
// ==========================================
void sendHeartbeat() {
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10);

    if (!client.connect(SERVER_HOST, SERVER_PORT)) {
        Serial.println("  ♥ Heartbeat: server unreachable");
        return;
    }

    const char* body = "{\"bracelet\":\"Connected\"}";
    int bodyLen = strlen(body);

    client.printf("POST %s HTTP/1.1\r\n", HEARTBEAT_PATH);
    client.printf("Host: %s\r\n", SERVER_HOST);
    client.print("Content-Type: application/json\r\n");
    client.printf("Content-Length: %d\r\n", bodyLen);
    client.print("Connection: close\r\n\r\n");
    client.print(body);

    // Drain response quickly (we don't care about the body)
    unsigned long deadline = millis() + 5000;
    while (client.connected() && millis() < deadline) {
        if (client.available()) { client.read(); }
        else { delay(10); }
    }
    client.stop();
    Serial.println("  ♥ Heartbeat OK");
}

// ==========================================
// RECORD & SEND — streaming, crash-safe
//
// ~1 KB RAM only (no big audio buffer).
// All errors are caught — never crashes.
// ==========================================
void recordAndSend() {
    Serial.printf("\n🎙  Free heap: %u bytes\n", ESP.getFreeHeap());

    uint32_t dataSize = SAMPLES_TOTAL * sizeof(int16_t);  // 160 000 bytes
    uint32_t wavSize  = 44 + dataSize;

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(30);

    Serial.println("   Connecting to server...");
    if (!client.connect(SERVER_HOST, SERVER_PORT)) {
        Serial.println("   ✗ HTTPS connection FAILED — skipping this cycle");
        return;
    }
    Serial.println("   ✓ Connected!");

    // HTTP headers
    client.printf("POST %s HTTP/1.1\r\n", AUDIO_PATH);
    client.printf("Host: %s\r\n", SERVER_HOST);
    client.print("Content-Type: application/octet-stream\r\n");
    client.printf("Content-Length: %u\r\n", wavSize);
    client.print("Connection: close\r\n\r\n");

    // WAV header (44 bytes)
    uint8_t wavHeader[44];
    buildWavHeader(wavHeader, dataSize, SAMPLE_RATE);
    client.write(wavHeader, 44);

    // Stream I2S → server (low memory)
    Serial.printf("   Recording & streaming %d seconds...\n", RECORD_SECONDS);
    int32_t i2sBuf[128];
    int16_t sampleBuf[128];
    int totalSamples = 0;

    while (totalSamples < SAMPLES_TOTAL) {
        size_t bytesRead = 0;
        i2s_read(I2S_PORT, i2sBuf, sizeof(i2sBuf), &bytesRead, portMAX_DELAY);
        int count = bytesRead / sizeof(int32_t);

        int remaining = SAMPLES_TOTAL - totalSamples;
        if (count > remaining) count = remaining;

        for (int i = 0; i < count; i++) {
            sampleBuf[i] = (int16_t)(i2sBuf[i] >> 16);
        }

        client.write((uint8_t*)sampleBuf, count * sizeof(int16_t));
        totalSamples += count;
    }

    Serial.printf("   Sent %d samples. Reading response...\n", totalSamples);

    // Read response with timeout (won't hang forever)
    unsigned long deadline = millis() + 30000;
    while (client.connected() && millis() < deadline) {
        String line = client.readStringUntil('\n');
        if (line == "\r") break;
    }

    String response = "";
    while (client.available() && millis() < deadline) {
        response += (char)client.read();
    }
    client.stop();

    if (response.length() > 0) {
        Serial.println("   Response: " + response);
    } else {
        Serial.println("   (no response body)");
    }
    Serial.printf("   Free heap after: %u bytes\n", ESP.getFreeHeap());
}

// ==========================================
// SETUP
// ==========================================
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== Memonic Bracelet ===");
    Serial.printf("Total heap: %u bytes\n", ESP.getHeapSize());
    Serial.printf("Free heap:  %u bytes\n", ESP.getFreeHeap());

    // WiFi stays connected permanently
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);
    ensureWiFi();

    initI2S();

    Serial.printf("Free heap after init: %u bytes\n", ESP.getFreeHeap());
    Serial.println("Ready!\n");

    // First heartbeat immediately
    if (WiFi.status() == WL_CONNECTED) {
        sendHeartbeat();
    }
    lastHeartbeat = millis();
    lastRecording = millis();
}

// ==========================================
// LOOP — non-blocking, always-alive
//
// Two independent timers:
//   1. Heartbeat every 10s  → keeps UI "Connected"
//   2. Recording every 20s  → sends audio to pipeline
//
// If anything fails, we log and move on.
// The ESP32 never crashes or restarts.
// ==========================================
void loop() {
    // Reconnect WiFi if dropped
    ensureWiFi();

    if (WiFi.status() != WL_CONNECTED) {
        delay(1000);  // wait before retry
        return;
    }

    unsigned long now = millis();

    // ── Heartbeat (lightweight, every 10s) ──
    if (now - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
        sendHeartbeat();
        lastHeartbeat = now;
    }

    // ── Record & send audio (every 20s) ──
    if (now - lastRecording >= RECORD_INTERVAL_MS) {
        recordAndSend();
        lastRecording = now;
        lastHeartbeat = now;  // audio upload also counts as heartbeat
    }

    delay(100);  // prevent watchdog reset
}