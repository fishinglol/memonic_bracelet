#include <Arduino.h>
#include <driver/i2s.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

// ==========================================
// CONFIG — Update these for your setup
// ==========================================
#define WIFI_SSID      "YOUR_WIFI"
#define WIFI_PASSWORD  "YOUR_PASSWORD"

// Lightning AI server — the path after the host
const char* SERVER_HOST = "8001-01kkh2et3bdjymj2fjq6jabg8k.cloudspaces.litng.ai";
const char* SERVER_PATH = "/api/esp32-audio";
const int   SERVER_PORT = 443;  // HTTPS

// Audio config
#define I2S_SCK   5
#define I2S_WS    4  
#define I2S_SD    6
#define I2S_PORT  I2S_NUM_0
#define SAMPLE_RATE    16000
#define RECORD_SECONDS 5
#define SAMPLES_TOTAL  (SAMPLE_RATE * RECORD_SECONDS)

// Timing
#define LOOP_DELAY_MS  10000  // Wait 10s between recordings

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
// WIFI CONNECT
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
// STREAM RECORD + SEND (low memory — ~1KB RAM only)
//
// Instead of buffering 160KB in RAM, we:
//   1. Open HTTPS connection
//   2. Send HTTP headers + WAV header (44 bytes)
//   3. Stream I2S samples directly to the socket as we record
//   4. Read the server response
//
// Total RAM used: ~1KB (I2S read buffer) + 44 bytes (WAV header)
// ==========================================
void recordAndSend() {
    Serial.printf("Free heap before record: %u bytes\n", ESP.getFreeHeap());

    uint32_t dataSize = SAMPLES_TOTAL * sizeof(int16_t);  // 160000 bytes
    uint32_t wavSize  = 44 + dataSize;                     // 160044 bytes

    // --- 1. Connect to server ---
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi disconnected — reconnecting...");
        connectWiFi();
    }

    WiFiClientSecure client;
    client.setInsecure();  // Skip SSL cert validation (OK for dev)
    client.setTimeout(30);  // 30 second timeout

    Serial.println("Connecting to server...");
    if (!client.connect(SERVER_HOST, SERVER_PORT)) {
        Serial.println("HTTPS connection FAILED!");
        return;
    }
    Serial.println("Connected!");

    // --- 2. Send HTTP headers ---
    client.printf("POST %s HTTP/1.1\r\n", SERVER_PATH);
    client.printf("Host: %s\r\n", SERVER_HOST);
    client.printf("Content-Type: application/octet-stream\r\n");
    client.printf("Content-Length: %u\r\n", wavSize);
    client.printf("Connection: close\r\n");
    client.printf("\r\n");

    // --- 3. Send WAV header (44 bytes) ---
    uint8_t wavHeader[44];
    buildWavHeader(wavHeader, dataSize, SAMPLE_RATE);
    client.write(wavHeader, 44);

    // --- 4. Stream I2S audio directly to server ---
    Serial.printf("Recording & streaming %d seconds...\n", RECORD_SECONDS);
    int32_t i2sBuf[128];       // ~512 bytes — small I2S read buffer
    int16_t sampleBuf[128];    // ~256 bytes — converted 16-bit samples
    int totalSamples = 0;

    while (totalSamples < SAMPLES_TOTAL) {
        size_t bytesRead = 0;
        i2s_read(I2S_PORT, i2sBuf, sizeof(i2sBuf), &bytesRead, portMAX_DELAY);
        int count = bytesRead / sizeof(int32_t);

        // Limit to remaining samples needed
        int remaining = SAMPLES_TOTAL - totalSamples;
        if (count > remaining) count = remaining;

        // Convert 32-bit I2S → 16-bit PCM
        for (int i = 0; i < count; i++) {
            sampleBuf[i] = (int16_t)(i2sBuf[i] >> 16);
        }

        // Stream directly to server — no big buffer needed!
        client.write((uint8_t*)sampleBuf, count * sizeof(int16_t));
        totalSamples += count;
    }

    Serial.printf("Streamed %d samples. Waiting for server response...\n", totalSamples);

    // --- 5. Read server response ---
    // Skip HTTP headers
    while (client.connected()) {
        String line = client.readStringUntil('\n');
        if (line == "\r") break;  // End of headers
    }

    // Read response body (JSON)
    String response = "";
    while (client.available()) {
        response += (char)client.read();
    }
    client.stop();

    Serial.println("Server response:");
    Serial.println(response);
    Serial.printf("Free heap after: %u bytes\n", ESP.getFreeHeap());
}

// ==========================================
// SETUP + LOOP
// ==========================================
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== Memonic Bracelet ===");
    Serial.printf("Total heap: %u bytes\n", ESP.getHeapSize());
    Serial.printf("Free heap:  %u bytes\n", ESP.getFreeHeap());

    connectWiFi();
    initI2S();

    Serial.printf("Free heap after init: %u bytes\n", ESP.getFreeHeap());
    Serial.println("Ready! Starting recording loop...\n");
}

void loop() {
    recordAndSend();
    Serial.printf("Waiting %d seconds before next recording...\n\n", LOOP_DELAY_MS / 1000);
    delay(LOOP_DELAY_MS);
}