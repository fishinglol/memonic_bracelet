#include <Arduino.h>
#include <driver/i2s.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include "secrets.h"   // WIFI_SSID, WIFI_PASSWORD — not committed to git

// ============================================================
//  Memonic — ESP32-S3 Super Mini
//
//  Architecture: UDP relay (no TLS on ESP32)
//  ─────────────────────────────────────────
//   ESP32 → UDP (LAN, no TLS) → Mac relay.py → WSS/TLS → Lightning AI
//
//   No TLS on ESP32 = no ring overflow, no latency spike
//   Mac handles the slow cloud WSS connection
// ============================================================

// --- WiFi Config ---
const char* ssid      = WIFI_SSID;      // defined in src/secrets.h (git-ignored)
const char* password  = WIFI_PASSWORD;
const char* relayHost = "255.255.255.255"; // broadcast — auto-finds phone, no IP config needed
const int   relayPort = 5005;            // UDP: ESP32 → Mac (audio)
const int   cmdPort   = 5006;            // UDP: Mac → ESP32 (commands)

// --- INMP441 Pins ---
#define I2S_SCK  5
#define I2S_WS   4
#define I2S_SD   6
#define I2S_PORT I2S_NUM_0

// --- Audio Config ---
#define SAMPLE_RATE           16000
#define AUTO_RECORD_SECONDS   2
#define ENROLL_RECORD_SECONDS 3

// DMA: 8 × 256 × 4B = 8KB
#define DMA_BUF_COUNT 8
#define DMA_BUF_LEN   256

// Ring buffer: 64KB = 2s
#define RING_BUFFER_SIZE 65536

// Send batch: 4KB per UDP packet
#define SEND_BATCH_BYTES 4096

// --- Globals ---
WiFiUDP          udp;
RingbufHandle_t  audioRingBuf  = nullptr;
TaskHandle_t     i2sTaskHandle = nullptr;

volatile bool          isRecording        = false;
volatile bool          isStreaming        = false;
volatile unsigned long recordingStartMs   = 0;
volatile unsigned long recordingDurationMs = 0;

// --- Forward Declarations ---
void startRecording(int seconds, const String& mode, const String& user);
void i2sTask(void* param);

// ============================================================
//  UDP helpers
// ============================================================
void sendText(const String& txt) {
    udp.beginPacket(relayHost, relayPort);
    udp.write((const uint8_t*)txt.c_str(), txt.length());
    udp.endPacket();
}

void sendBin(const uint8_t* data, size_t len) {
    udp.beginPacket(relayHost, relayPort);
    udp.write(data, len);
    udp.endPacket();
}

// ============================================================
//  Command parser — called when relay sends a command
// ============================================================
void handleCommand(const String& cmd) {
    Serial.printf("⬅️  Relay: %s\n", cmd.c_str());

    if (isRecording) {
        if (cmd.startsWith("STOP_STREAM")) {
            isStreaming = false;
            isRecording = false;
            Serial.printf("🛑 Stream stopped | heap: %d\n", ESP.getFreeHeap());
        } else {
            Serial.printf("✓ Result: %s\n", cmd.c_str());
        }
        return;
    }

    if (cmd.startsWith("ENROLL ")) {
        String name = cmd.substring(7); name.trim();
        startRecording(ENROLL_RECORD_SECONDS, "ENROLL", name);
    } else if (cmd.startsWith("STREAM")) {
        isStreaming = true;
        startRecording(0, "STREAM", "");
    } else if (cmd.startsWith("START")) {
        int seconds = AUTO_RECORD_SECONDS;
        if (cmd.length() > 5) {
            String arg = cmd.substring(5); arg.trim();
            int n = arg.toInt();
            if (n >= 1 && n <= 30) seconds = n;
        }
        startRecording(seconds, "START", "");
    } else if (cmd.startsWith("SUCCESS") || cmd.startsWith("OK") || cmd.startsWith("ERROR")) {
        Serial.printf("✓ Late result: %s\n", cmd.c_str());
    }
}

// ============================================================
//  Init
// ============================================================
void initWiFi() {
    Serial.print("WiFi");
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.println(" → " + WiFi.localIP().toString());
}

void initI2S() {
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate          = SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = DMA_BUF_COUNT,
        .dma_buf_len          = DMA_BUF_LEN,
        .use_apll             = false,
    };
    i2s_pin_config_t pins = {
        .bck_io_num   = I2S_SCK,
        .ws_io_num    = I2S_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = I2S_SD
    };
    i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
    i2s_set_pin(I2S_PORT, &pins);
    i2s_zero_dma_buffer(I2S_PORT);
    i2s_start(I2S_PORT);
    Serial.println("I2S Ready");
}

void initUDP() {
    udp.begin(cmdPort);
    Serial.printf("UDP: broadcast audio → :%d | commands ← :%d\n",
                  relayPort, cmdPort);
    sendText("HELLO");   // announce ourselves to whoever is listening on LAN
}

// ============================================================
//  Recording trigger
// ============================================================
void startRecording(int seconds, const String& mode, const String& user) {
    if (isRecording) { Serial.println("Already recording — ignored"); return; }

    size_t junk; void* p;
    while ((p = xRingbufferReceive(audioRingBuf, &junk, 0)) != NULL)
        vRingbufferReturnItem(audioRingBuf, p);

    recordingDurationMs = (unsigned long)seconds * 1000UL;
    recordingStartMs    = millis();

    if (seconds > 0)
        Serial.printf("🎙️  %s %s: %ds | heap: %d\n", mode.c_str(), user.c_str(), seconds, ESP.getFreeHeap());
    else
        Serial.printf("🎙️  %s: continuous | heap: %d\n", mode.c_str(), ESP.getFreeHeap());

    isRecording = true;
}

// ============================================================
//  I2S Task — Core 0
// ============================================================
void i2sTask(void* param) {
    static int32_t raw[DMA_BUF_LEN];
    static int16_t pcm[DMA_BUF_LEN];
    while (true) {
        if (!isRecording) {
            size_t b; i2s_read(I2S_PORT, raw, sizeof(raw), &b, pdMS_TO_TICKS(50));
            continue;
        }
        size_t bytesIn = 0;
        i2s_read(I2S_PORT, raw, sizeof(raw), &bytesIn, portMAX_DELAY);
        int valid = bytesIn / 4;
        for (int i = 0; i < valid; i++) {
            int32_t v = (raw[i] >> 8) * 8;
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
            pcm[i] = (int16_t)v;
        }
        if (xRingbufferSend(audioRingBuf, pcm, valid * 2, pdMS_TO_TICKS(20)) != pdTRUE)
            Serial.println("Ring overflow");
    }
}

// ============================================================
//  Setup
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("=== Memonic UDP relay mode ===");
    Serial.printf("Free @boot: %d\n", ESP.getFreeHeap());

    initWiFi();
    initI2S();

    audioRingBuf = xRingbufferCreate(RING_BUFFER_SIZE, RINGBUF_TYPE_NOSPLIT);
    if (!audioRingBuf) { Serial.println("Ring buffer FAIL"); while(true) delay(1000); }

    xTaskCreatePinnedToCore(i2sTask, "i2s", 4096, NULL, 5, &i2sTaskHandle, 0);

    initUDP();

    Serial.printf("Ready | heap: %d\n", ESP.getFreeHeap());
    Serial.println("Waiting for commands from relay...");
}

// ============================================================
//  Loop — Core 1
// ============================================================
void loop() {
    // ── WiFi watchdog ────────────────────────────────────────
    if (WiFi.status() != WL_CONNECTED) {
        static unsigned long lastTry = 0;
        if (millis() - lastTry > 5000) {
            lastTry = millis();
            Serial.println("WiFi lost — reconnecting");
            WiFi.reconnect();
        }
        delay(100);
        return;
    }

    // ── Check incoming UDP commands from relay ───────────────
    int pktSize = udp.parsePacket();
    if (pktSize > 0) {
        char buf[256] = {0};
        udp.read(buf, min(pktSize, 255));
        String cmd = String(buf); cmd.trim();
        if (cmd.length() > 0) handleCommand(cmd);
    }

    if (!isRecording) { delay(5); return; }

    // ── Wall-clock stop (ENROLL / START only) ───────────────
    static uint8_t sendBuf[SEND_BATCH_BYTES];
    static size_t  sendLen = 0;

    auto flush = [&]() {
        if (sendLen > 0) sendBin(sendBuf, sendLen);
        sendLen = 0;
    };

    if (!isStreaming && recordingDurationMs > 0) {
        unsigned long elapsed = millis() - recordingStartMs;
        if (elapsed >= recordingDurationMs) {
            isRecording = false;
            size_t itemSize; void* item;
            unsigned long t = millis();
            while (millis() - t < 300) {
                item = xRingbufferReceive(audioRingBuf, &itemSize, pdMS_TO_TICKS(20));
                if (!item) break;
                if (sendLen + itemSize > SEND_BATCH_BYTES) flush();
                memcpy(sendBuf + sendLen, item, itemSize);
                sendLen += itemSize;
                vRingbufferReturnItem(audioRingBuf, item);
            }
            flush();
            sendText("STOP");
            Serial.printf("Stop at %lums | heap: %d\n", elapsed, ESP.getFreeHeap());
            return;
        }
    }

    // ── Drain ring buffer → batch → UDP ─────────────────────
    for (int i = 0; i < 16; i++) {
        size_t itemSize;
        void* item = xRingbufferReceive(audioRingBuf, &itemSize, 0);
        if (!item) break;
        if (sendLen + itemSize > SEND_BATCH_BYTES) {
            vRingbufferReturnItem(audioRingBuf, item);
            break;
        }
        memcpy(sendBuf + sendLen, item, itemSize);
        sendLen += itemSize;
        vRingbufferReturnItem(audioRingBuf, item);
    }
    if (sendLen >= SEND_BATCH_BYTES - 512) flush();
}
