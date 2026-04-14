#include <Arduino.h>
#include <driver/i2s.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include "secrets.h"  // WIFI_SSID, WIFI_PASSWORD

// ==========================================
// CONFIG
// ==========================================
const char* WS_HOST = "8001-01kkh2et3bdjymj2fjq6jabg8k.cloudspaces.litng.ai";
const int   WS_PORT = 443;
const char* WS_PATH = "/ws/audio";

// Audio config (unchanged from original)
#define I2S_SCK   5
#define I2S_WS    4
#define I2S_SD    6
#define I2S_PORT  I2S_NUM_0
#define SAMPLE_RATE    16000
#define RECORD_SECONDS 5
#define SAMPLES_TOTAL  (SAMPLE_RATE * RECORD_SECONDS)

// Timing
#define RECORD_INTERVAL_MS  15000  // Record every 15 seconds

// ==========================================
// STATE
// ==========================================
WebSocketsClient webSocket;
bool wsConnected = false;
bool waitingForResponse = false;
unsigned long lastRecordTime = 0;

// ==========================================
// I2S INIT (same pin config as before)
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
// WEBSOCKET EVENT HANDLER
// ==========================================
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            wsConnected = false;
            Serial.println("🔌 WebSocket disconnected — will auto-reconnect");
            break;

        case WStype_CONNECTED:
            wsConnected = true;
            waitingForResponse = false;
            Serial.printf("🔌 WebSocket connected to: %s%s\n", WS_HOST, WS_PATH);
            Serial.printf("   Free heap: %u bytes\n", ESP.getFreeHeap());
            break;

        case WStype_TEXT:
            // Server sends JSON result after processing
            Serial.println("\n📨 Server response:");
            Serial.println((char*)payload);
            Serial.println();
            waitingForResponse = false;
            break;

        case WStype_ERROR:
            Serial.printf("❌ WebSocket error (length: %u)\n", length);
            break;

        default:
            break;
    }
}

// ==========================================
// RECORD & STREAM via WebSocket
//
// Protocol:
//   1. Send TEXT "START"
//   2. Send BINARY chunks (raw 16-bit PCM, 16kHz, mono)
//   3. Send TEXT "STOP"
//   4. Wait for JSON response
//
// RAM usage: ~1KB (256-sample I2S buf + 256-sample conversion buf)
// ==========================================
void recordAndStream() {
    if (!wsConnected) {
        Serial.println("⚠️  Not connected, skipping recording");
        return;
    }

    Serial.printf("\nFree heap before record: %u bytes\n", ESP.getFreeHeap());

    // Signal server: start recording
    webSocket.sendTXT("START");
    delay(50);  // Let server process the command

    Serial.printf("🎤 Recording %d seconds...\n", RECORD_SECONDS);

    int32_t i2sBuf[256];       // ~1KB — I2S read buffer (32-bit samples)
    int16_t sampleBuf[256];    // ~512 bytes — converted 16-bit samples
    int totalSamples = 0;
    int chunksSent = 0;

    while (totalSamples < SAMPLES_TOTAL) {
        size_t bytesRead = 0;
        i2s_read(I2S_PORT, i2sBuf, sizeof(i2sBuf), &bytesRead, portMAX_DELAY);
        int count = bytesRead / sizeof(int32_t);

        // Limit to remaining samples
        int remaining = SAMPLES_TOTAL - totalSamples;
        if (count > remaining) count = remaining;

        // Convert 32-bit I2S → 16-bit PCM
        for (int i = 0; i < count; i++) {
            sampleBuf[i] = (int16_t)(i2sBuf[i] >> 16);
        }

        // Send raw PCM chunk over WebSocket (binary)
        webSocket.sendBIN((uint8_t*)sampleBuf, count * sizeof(int16_t));
        totalSamples += count;
        chunksSent++;

        // Keep WebSocket alive during recording
        webSocket.loop();
    }

    Serial.printf("✅ Sent %d samples in %d chunks\n", totalSamples, chunksSent);

    // Signal server: stop and process
    webSocket.sendTXT("STOP");
    waitingForResponse = true;

    Serial.println("⏳ Waiting for server response...");
    Serial.printf("Free heap after record: %u bytes\n", ESP.getFreeHeap());
}

// ==========================================
// SETUP
// ==========================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n============================");
    Serial.println("  Memonic Bracelet (WebSocket)");
    Serial.println("============================");
    Serial.printf("Total heap: %u bytes\n", ESP.getHeapSize());
    Serial.printf("Free heap:  %u bytes\n", ESP.getFreeHeap());

    connectWiFi();
    initI2S();

    // Connect WebSocket (WSS — secure)
    webSocket.beginSSL(WS_HOST, WS_PORT, WS_PATH);
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);  // Auto-reconnect every 5s if dropped

    Serial.println("WebSocket connecting...");
    Serial.printf("Free heap after init: %u bytes\n", ESP.getFreeHeap());
}

// ==========================================
// LOOP — non-blocking, WebSocket stays alive
// ==========================================
void loop() {
    // Must call frequently to keep WebSocket alive & handle events
    webSocket.loop();

    // Record at fixed intervals (only when connected and not waiting for a response)
    if (wsConnected && !waitingForResponse && (millis() - lastRecordTime > RECORD_INTERVAL_MS)) {
        recordAndStream();
        lastRecordTime = millis();
    }
}