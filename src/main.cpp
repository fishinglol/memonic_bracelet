#include <Arduino.h>
#include <driver/i2s.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>

// ============================================================
//  Memonic — ESP32-S3 Super Mini  (NO BLE — saves ~60KB RAM)
//
//  Architecture: server-driven WSS streaming
//  ─────────────────────────────────────────
//   Boot: WiFi → WSS connect → idle, listen for server command
//   Server sends "ENROLL <name>" or "START"
//     → I2S task on Core 0 fills ring buffer
//     → Main loop on Core 1 drains ring buffer → sendBIN
//     → After N seconds: send "STOP" → wait result
//
//  Memory budget (vs old BLE version):
//   - Removed BLE stack: +~60KB free heap
//   - Persistent WSS:    -~40KB
//   - Net free heap:     ~150-180KB during enroll (was ~67KB)
// ============================================================

// --- WiFi Config ---
const char* ssid       = "Fais";
const char* password   = "12345678";
const char* serverHost = "8001-01kkh2et3bdjymj2fjq6jabg8k.cloudspaces.litng.ai";
const int   serverPort = 443;
const char* serverPath = "/ws/audio";

// --- INMP441 Pins ---
#define I2S_SCK  5
#define I2S_WS   4
#define I2S_SD   6
#define I2S_PORT I2S_NUM_0

// --- Audio Config ---
#define SAMPLE_RATE           16000
#define AUTO_RECORD_SECONDS   2
#define ENROLL_RECORD_SECONDS 3

// DMA: 8 × 256 × 4B = 8KB internal RAM = 128ms safety against I2S overrun
#define DMA_BUF_COUNT 8
#define DMA_BUF_LEN   256

// Ring buffer between I2S task and network: 8KB = 256ms of audio buffering
#define RING_BUFFER_SIZE 8192

// --- Globals ---
WebSocketsClient    webSocket;
RingbufHandle_t     audioRingBuf = nullptr;
TaskHandle_t        i2sTaskHandle = nullptr;

volatile bool wsConnected   = false;
volatile bool isRecording   = false;
volatile int  targetSamples = 0;
volatile int  sentSamples   = 0;

// --- Forward Declarations ---
void startRecording(int seconds, const String& mode, const String& user);
void i2sTask(void* param);

// ============================================================
//  WebSocket Event — server sends commands here
// ============================================================
void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            wsConnected = false;
            isRecording = false;
            Serial.println("☁️  WS disconnected");
            break;

        case WStype_CONNECTED:
            wsConnected = true;
            Serial.printf("☁️  WS connected | heap: %d\n", ESP.getFreeHeap());
            // Tell server we're alive (server uses this to mark bracelet online)
            webSocket.sendTXT("HELLO");
            break;

        case WStype_TEXT: {
            String cmd = String((char*)payload).substring(0, length);
            cmd.trim();
            Serial.printf("⬅️  Server: %s\n", cmd.c_str());

            if (isRecording) {
                // Server sent SUCCESS/ERROR/OK — final result
                Serial.printf("✓ Result: %s\n", cmd.c_str());
                break;
            }

            if (cmd.startsWith("ENROLL ")) {
                String name = cmd.substring(7);
                name.trim();
                startRecording(ENROLL_RECORD_SECONDS, "ENROLL", name);
            } else if (cmd.equalsIgnoreCase("START")) {
                startRecording(AUTO_RECORD_SECONDS, "START", "");
            } else if (cmd.startsWith("SUCCESS") || cmd.startsWith("OK") || cmd.startsWith("ERROR")) {
                // Result while not recording (late delivery) — just log
                Serial.printf("✓ Late result: %s\n", cmd.c_str());
            }
            break;
        }
        default: break;
    }
}

// ============================================================
//  Init
// ============================================================
void initWiFi() {
    Serial.print("📶 WiFi");
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
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
    Serial.println("✅ I2S Ready (DMA 8x256)");
}

void initWebSocket() {
    webSocket.beginSSL(serverHost, serverPort, serverPath);
    webSocket.onEvent(onWsEvent);
    webSocket.setReconnectInterval(3000);
    webSocket.enableHeartbeat(15000, 3000, 2);
    Serial.println("☁️  WSS connecting...");
}

// ============================================================
//  Recording trigger (called from WS event when server sends command)
// ============================================================
void startRecording(int seconds, const String& mode, const String& user) {
    if (!wsConnected) return;
    if (isRecording) {
        Serial.println("⚠️  Already recording — ignored");
        return;
    }

    // Drain any stale samples from ring buffer
    size_t junk;
    void* p;
    while ((p = xRingbufferReceive(audioRingBuf, &junk, 0)) != NULL) {
        vRingbufferReturnItem(audioRingBuf, p);
    }

    targetSamples = SAMPLE_RATE * seconds;
    sentSamples   = 0;

    Serial.printf("🎙️  %s%s%s: %ds (%d samples) | heap: %d\n",
                  mode.c_str(),
                  user.length() ? " " : "",
                  user.c_str(),
                  seconds, targetSamples, ESP.getFreeHeap());

    isRecording = true;  // i2sTask + main loop start streaming now
}

// ============================================================
//  I2S Task — Core 0
//  Continuously reads I2S; pushes int16 PCM into ring buffer when recording
// ============================================================
void i2sTask(void* param) {
    static int32_t raw[DMA_BUF_LEN];
    static int16_t pcm[DMA_BUF_LEN];

    while (true) {
        if (!isRecording) {
            // Keep DMA flushed so first samples after start aren't stale
            size_t bytesIn;
            i2s_read(I2S_PORT, raw, sizeof(raw), &bytesIn, pdMS_TO_TICKS(50));
            continue;
        }

        size_t bytesIn = 0;
        i2s_read(I2S_PORT, raw, sizeof(raw), &bytesIn, portMAX_DELAY);
        int valid = bytesIn / 4;

        for (int i = 0; i < valid; i++) {
            // INMP441: 32-bit left-justified → shift >>8 then gain x8
            int32_t v = (raw[i] >> 8) * 8;
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
            pcm[i] = (int16_t)v;
        }

        if (xRingbufferSend(audioRingBuf, pcm, valid * 2, pdMS_TO_TICKS(20)) != pdTRUE) {
            Serial.println("⚠️  Ring overflow");
        }
    }
}

// ============================================================
//  Setup
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("============================");
    Serial.println("  Memonic — Streaming WSS  ");
    Serial.println("  (No BLE — server driven) ");
    Serial.println("============================");
    Serial.printf("Total heap : %d\n", ESP.getHeapSize());
    Serial.printf("Free @boot : %d\n", ESP.getFreeHeap());

    initWiFi();
    Serial.printf("Free after WiFi : %d\n", ESP.getFreeHeap());

    initI2S();

    audioRingBuf = xRingbufferCreate(RING_BUFFER_SIZE, RINGBUF_TYPE_NOSPLIT);
    if (!audioRingBuf) {
        Serial.println("❌ Ring buffer alloc failed");
        while (true) delay(1000);
    }

    xTaskCreatePinnedToCore(i2sTask, "i2s", 4096, NULL, 5, &i2sTaskHandle, 0);

    initWebSocket();

    Serial.println("─────────────────────────────────");
    Serial.printf("🚀 Ready | heap: %d\n", ESP.getFreeHeap());
    Serial.printf("   ENROLL : %ds   AUTO : %ds\n", ENROLL_RECORD_SECONDS, AUTO_RECORD_SECONDS);
    Serial.println("   Waiting for server command...");
    Serial.println("─────────────────────────────────");
}

// ============================================================
//  Loop — Core 1
//  Pumps WebSocket + drains ring buffer → sendBIN
// ============================================================
void loop() {
    webSocket.loop();

    // WiFi auto-reconnect
    if (WiFi.status() != WL_CONNECTED) {
        static unsigned long lastTry = 0;
        if (millis() - lastTry > 5000) {
            lastTry = millis();
            Serial.println("⚠️  WiFi lost — reconnecting");
            WiFi.reconnect();
        }
        delay(100);
        return;
    }

    if (!isRecording) {
        delay(5);
        return;
    }

    // Drain ring buffer → WSS
    size_t itemSize;
    void* item = xRingbufferReceive(audioRingBuf, &itemSize, pdMS_TO_TICKS(20));
    if (item) {
        if (wsConnected) {
            webSocket.sendBIN((uint8_t*)item, itemSize);
            sentSamples += itemSize / 2;
        }
        vRingbufferReturnItem(audioRingBuf, item);
    }

    // Reached target → stop, drain remaining, send STOP
    if (sentSamples >= targetSamples) {
        isRecording = false;

        unsigned long drainStart = millis();
        while (millis() - drainStart < 200) {
            item = xRingbufferReceive(audioRingBuf, &itemSize, pdMS_TO_TICKS(20));
            if (!item) break;
            if (wsConnected) webSocket.sendBIN((uint8_t*)item, itemSize);
            vRingbufferReturnItem(audioRingBuf, item);
        }

        if (wsConnected) webSocket.sendTXT("STOP");
        Serial.printf("✅ Sent %d samples | heap: %d\n", sentSamples, ESP.getFreeHeap());
        sentSamples = 0;
    }
}
