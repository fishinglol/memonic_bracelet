#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <driver/i2s.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

// ============================================================
//  Memonic — ESP32-S3 Super Mini (No PSRAM)
//  Strategy: dynamic malloc/free per recording session
//  RAM budget (idle ~200KB free):
//    AUTO   2s = 64KB → ผ่านสบาย (~110KB เหลือหลัง malloc)
//    ENROLL 3s = 96KB → fallback เหลือ 2s ถ้า heap ตึง
// ============================================================

// --- WiFi Config ---
const char* ssid       = "Fais";
const char* password   = "12345678";
const char* serverHost = "8001-01kkh2et3bdjymj2fjq6jabg8k.cloudspaces.litng.ai";
const int   serverPort = 443;
const String serverPath = "/audio";

// --- BLE UUIDs ---
#define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
#define CHARACTERISTIC_UUID "abcd1234-ab12-ab12-ab12-abcdef123456"

// --- INMP441 Pins ---
#define I2S_SCK  5
#define I2S_WS   4
#define I2S_SD   6
#define I2S_PORT I2S_NUM_0

// --- Audio Config ---
#define SAMPLE_RATE            16000

// AUTO  2s = 32,000 samples = 64KB  ← ปลอดภัย
// ENROLL 3s = 48,000 samples = 96KB ← พยายามก่อน, fallback เหลือ 2s ถ้า RAM ตึง
#define AUTO_RECORD_SECONDS    2
#define ENROLL_RECORD_SECONDS  3
#define FALLBACK_RECORD_SECONDS 2   // ใช้เมื่อ heap ไม่พอสำหรับ ENROLL 3s

const int auto_total_samples    = SAMPLE_RATE * AUTO_RECORD_SECONDS;
const int enroll_total_samples  = SAMPLE_RATE * ENROLL_RECORD_SECONDS;
const int fallback_total_samples = SAMPLE_RATE * FALLBACK_RECORD_SECONDS;

// DMA: 4 × 64 × 4 bytes = 1,024 bytes internal RAM
// (เดิม 8 × 256 × 4 = 8,192 bytes → ประหยัด ~7KB)
#define DMA_BUF_COUNT 4
#define DMA_BUF_LEN   64

// Heartbeat interval
#define HEARTBEAT_INTERVAL_MS 30000UL

// --- Globals ---
BLECharacteristic* pCharacteristic = NULL;
bool deviceConnected = false;
bool isRecording     = false;

// --- Forward Declarations ---
void recordAndSend(int total_samples, String mode, String enrollUser = "");
void sendHeartbeat();
void notifyBLE(const char* msg);

// ============================================================
//  BLE Callbacks
// ============================================================
class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        deviceConnected = true;
        Serial.println("📱 BLE Client Connected");
    }
    void onDisconnect(BLEServer* pServer) {
        deviceConnected = false;
        BLEDevice::startAdvertising();
        Serial.println("📱 BLE Client Disconnected — restarting advertising");
    }
};

class MyCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pChar) {
        String value = pChar->getValue().c_str();
        if (value.length() == 0) return;

        String cmd = value;
        cmd.trim();
        Serial.printf("📥 BLE Command: %s\n", cmd.c_str());

        if (cmd.startsWith("ENROLL ")) {
            String name = cmd.substring(7);
            name.trim();
            recordAndSend(enroll_total_samples, "ENROLL", name);
        } else if (cmd.equals("START")) {
            recordAndSend(auto_total_samples, "START");
        } else if (cmd.equals("RESET")) {
            isRecording = false;
            Serial.println("🔄 RESET received");
        }
    }
};

// ============================================================
//  Helpers
// ============================================================
void notifyBLE(const char* msg) {
    if (pCharacteristic) {
        pCharacteristic->setValue(msg);
        pCharacteristic->notify();
    }
}

// ============================================================
//  Init
// ============================================================
void initWiFi() {
    Serial.print("📶 Connecting to WiFi");
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\n✅ WiFi Connected: " + WiFi.localIP().toString());
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
    i2s_start(I2S_PORT);
    Serial.println("✅ I2S Initialized");
}

// ============================================================
//  WAV Header
// ============================================================
void writeWavHeader(byte* h, int dataLength) {
    unsigned int sr       = SAMPLE_RATE;
    unsigned int byteRate = SAMPLE_RATE * 2;
    unsigned int fileSize = 36 + dataLength;

    // RIFF
    h[0]='R'; h[1]='I'; h[2]='F'; h[3]='F';
    h[4]=(byte)(fileSize);      h[5]=(byte)(fileSize>>8);
    h[6]=(byte)(fileSize>>16);  h[7]=(byte)(fileSize>>24);
    h[8]='W'; h[9]='A'; h[10]='V'; h[11]='E';
    // fmt
    h[12]='f'; h[13]='m'; h[14]='t'; h[15]=' ';
    h[16]=16; h[17]=0; h[18]=0; h[19]=0;   // chunk size
    h[20]=1;  h[21]=0;                      // PCM
    h[22]=1;  h[23]=0;                      // mono
    h[24]=(byte)(sr);      h[25]=(byte)(sr>>8);
    h[26]=(byte)(sr>>16);  h[27]=(byte)(sr>>24);
    h[28]=(byte)(byteRate);      h[29]=(byte)(byteRate>>8);
    h[30]=(byte)(byteRate>>16);  h[31]=(byte)(byteRate>>24);
    h[32]=2;  h[33]=0;          // block align
    h[34]=16; h[35]=0;          // bits per sample
    // data
    h[36]='d'; h[37]='a'; h[38]='t'; h[39]='a';
    h[40]=(byte)(dataLength);      h[41]=(byte)(dataLength>>8);
    h[42]=(byte)(dataLength>>16);  h[43]=(byte)(dataLength>>24);
}

// ============================================================
//  Core: Record → Send
// ============================================================
void recordAndSend(int total_samples, String mode, String enrollUser) {
    if (isRecording) return;
    isRecording = true;

    // ----------------------------------------------------------
    // STEP 1: Smart malloc with fallback
    // ----------------------------------------------------------
    int needed_bytes = total_samples * sizeof(int16_t);

    // heap_caps_get_largest_free_block = block จริงที่ contiguous ที่สุด
    // ต่างจาก getFreeHeap() ที่นับรวม fragmented blocks
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    Serial.println("─────────────────────────────────");
    Serial.printf("🧠 Free heap    : %d bytes\n", ESP.getFreeHeap());
    Serial.printf("🧠 Largest block: %d bytes\n", largest_block);
    Serial.printf("📦 Need         : %d bytes (%ds)\n",
                  needed_bytes, total_samples / SAMPLE_RATE);

    // ถ้า block ใหญ่สุดไม่พอ → ลองลดเป็น 2s (fallback)
    if (largest_block < (size_t)needed_bytes) {
        if (mode == "ENROLL") {
            int fallback_bytes = fallback_total_samples * sizeof(int16_t);
            if (largest_block >= (size_t)fallback_bytes) {
                Serial.printf("⚠️  RAM tight — ENROLL falling back %ds → %ds\n",
                              ENROLL_RECORD_SECONDS, FALLBACK_RECORD_SECONDS);
                total_samples = fallback_total_samples;
                needed_bytes  = fallback_bytes;
            } else {
                Serial.println("❌ Not enough RAM even for 2s — aborting");
                isRecording = false;
                notifyBLE("ERROR_RAM");
                return;
            }
        } else {
            Serial.println("❌ Not enough RAM for AUTO record — aborting");
            isRecording = false;
            notifyBLE("ERROR_RAM");
            return;
        }
    }

    int16_t* audio_buffer = (int16_t*)malloc(needed_bytes);
    if (!audio_buffer) {
        // malloc ล้มเหลวทั้งที่ block ดูพอ → heap fragmented หนัก
        Serial.println("❌ malloc failed (fragmentation) — aborting");
        isRecording = false;
        notifyBLE("ERROR_RAM");
        return;
    }
    Serial.printf("✅ Buffer allocated %d bytes | Remaining: %d bytes\n",
                  needed_bytes, ESP.getFreeHeap());

    // ----------------------------------------------------------
    // STEP 2: Record — I2S อ่านลง buffer ล้วนๆ ไม่มี network เลย
    // เสียงไม่กระตุกแน่นอนเพราะไม่มี interrupt จาก WiFi/TLS
    // ----------------------------------------------------------
    Serial.printf("🎙️  Recording %ds (%d samples)...\n",
                  total_samples / SAMPLE_RATE, total_samples);

    int32_t i2s_chunk[DMA_BUF_LEN]; // 64 × 4 = 256 bytes บน stack
    size_t  bytesIn;
    int     samples_read = 0;

    while (samples_read < total_samples && isRecording) {
        i2s_read(I2S_PORT, i2s_chunk, sizeof(i2s_chunk), &bytesIn, portMAX_DELAY);
        int valid = bytesIn / 4;
        for (int i = 0; i < valid && samples_read < total_samples; i++) {
            // INMP441: 32-bit left-justified → shift >> 8, gain x8
            int32_t val = (i2s_chunk[i] >> 8) * 8;
            audio_buffer[samples_read++] = (int16_t)constrain(val, -32768, 32767);
        }
    }
    Serial.printf("✅ Recorded %d samples\n", samples_read);

    // ถูก RESET ระหว่างอัด
    if (!isRecording) {
        Serial.println("⚠️  Recording cancelled by RESET");
        free(audio_buffer);
        return;
    }

    // ----------------------------------------------------------
    // STEP 3: WiFi check
    // ----------------------------------------------------------
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("❌ WiFi disconnected");
        free(audio_buffer);
        isRecording = false;
        notifyBLE("ERROR_WIFI");
        return;
    }

    // ----------------------------------------------------------
    // STEP 4: TLS connect
    // setBufferSizes(rx, tx): ลด mbedTLS buffer จาก default ~32KB → ~1.5KB
    // ประหยัด ~30KB ช่วง TLS handshake ซึ่งสำคัญมากตอนที่ audio_buffer กิน 96KB อยู่
    // ----------------------------------------------------------
    Serial.println("☁️  Connecting to server...");
    WiFiClientSecure client;
    client.setInsecure();
    
    // ✅ บังคับใช้ Buffer ขนาดเล็ก เพื่อให้ TLS Handshake สำเร็จในแรมที่เหลือเพียง 12KB
    client.setBufferSizes(1024, 512); 

    if (!client.connect(serverHost, serverPort)) {
        Serial.println("❌ TLS connection failed");
        free(audio_buffer);
        isRecording = false;
        notifyBLE("ERROR_SERVER");
        return;
    }
    Serial.println("✅ TLS Connected");

    // ----------------------------------------------------------
    // STEP 5: Build multipart metadata (ไม่ใช้ RAM เพิ่ม เป็นแค่ String)
    // ----------------------------------------------------------
    String boundary = "----ESP32Boundary";
    int    dataLen  = samples_read * 2; // int16 = 2 bytes per sample

    String head = "--" + boundary + "\r\n"
                  "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
                  "Content-Type: audio/wav\r\n\r\n";

    String tail = "\r\n";
    if (mode == "ENROLL") {
        tail += "--" + boundary + "\r\n"
                "Content-Disposition: form-data; name=\"enroll_user\"\r\n\r\n"
                + enrollUser + "\r\n";
    }
    tail += "--" + boundary + "--\r\n";

    int totalLen = head.length() + 44 + dataLen + tail.length();

    // ----------------------------------------------------------
    // STEP 6: Send HTTP headers
    // ----------------------------------------------------------
    client.print(String("POST ") + serverPath + " HTTP/1.1\r\n");
    client.print(String("Host: ") + serverHost + "\r\n");
    client.print("Content-Type: multipart/form-data; boundary=" + boundary + "\r\n");
    client.print("Content-Length: " + String(totalLen) + "\r\n");
    client.print("Connection: close\r\n\r\n");

    // ----------------------------------------------------------
    // STEP 7: Send WAV header + audio data in 2KB chunks
    // ----------------------------------------------------------
    byte wavHeader[44];
    writeWavHeader(wavHeader, dataLen);
    client.print(head);
    client.write(wavHeader, 44);

    int offset    = 0;
    int chunkSize = 2048;
    while (offset < dataLen) {
        int toSend = min(chunkSize, dataLen - offset);
        client.write((uint8_t*)audio_buffer + offset, toSend);
        offset += toSend;
    }
    client.print(tail);

    // ----------------------------------------------------------
    // STEP 8: Free buffer ทันทีหลังส่งข้อมูลครบ ไม่ต้องรอ response
    // ทำให้ RAM ว่างเร็วที่สุด — คืน 96KB กลับก่อน read response
    // ----------------------------------------------------------
    free(audio_buffer);
    audio_buffer = nullptr;
    Serial.printf("🗑️  Buffer freed | Free heap: %d bytes\n", ESP.getFreeHeap());

    // ----------------------------------------------------------
    // STEP 9: Read server response
    // ----------------------------------------------------------
    Serial.println("⏳ Waiting for server response...");
    unsigned long timeout = millis();
    while (client.connected() && !client.available()) {
        if (millis() - timeout > 10000) {
            Serial.println("❌ Server response timeout");
            client.stop();
            isRecording = false;
            notifyBLE("ERROR_TIMEOUT");
            return;
        }
        delay(10);
    }

    // ข้าม HTTP headers
    while (client.connected()) {
        String line = client.readStringUntil('\n');
        if (line == "\r") break;
    }

    String response = client.readString();
    Serial.printf("✅ Server: %s\n", response.c_str());

    client.stop();

    notifyBLE("SUCCESS");
    Serial.println("─────────────────────────────────");
    isRecording = false;
}

// ============================================================
//  Heartbeat — แจ้ง backend ว่า bracelet online
// ============================================================
void sendHeartbeat() {
    if (WiFi.status() != WL_CONNECTED) return;

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(5);

    if (!client.connect(serverHost, serverPort)) return;

    String body = "{\"bracelet\":\"Connected\",\"dock\":\"Connected\"}";
    client.print(String("POST /update HTTP/1.1\r\n") +
                 "Host: " + serverHost + "\r\n" +
                 "Content-Type: application/json\r\n" +
                 "Content-Length: " + String(body.length()) + "\r\n" +
                 "Connection: close\r\n\r\n" +
                 body);

    // รอ response สั้นๆ แล้วปิด
    unsigned long t = millis();
    while (client.connected() && !client.available() && millis() - t < 3000) {
        delay(10);
    }
    client.stop();
    Serial.println("💓 Heartbeat sent");
}

// ============================================================
//  Setup
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("============================");
    Serial.println("  Memonic — ESP32-S3 Mini  ");
    Serial.println("============================");
    Serial.printf("🧠 Total heap   : %d bytes\n", ESP.getHeapSize());
    Serial.printf("🧠 Free at boot : %d bytes\n", ESP.getFreeHeap());

    // 1. WiFi
    initWiFi();
    Serial.printf("🧠 Free after WiFi : %d bytes\n", ESP.getFreeHeap());
    delay(200);

    // First heartbeat
    sendHeartbeat();

    // 2. BLE
    BLEDevice::init("Memonic");
    BLEServer* pServer = BLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());

    BLEService* pService = pServer->createService(SERVICE_UUID);
    pCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ  |
        BLECharacteristic::PROPERTY_WRITE |
        BLECharacteristic::PROPERTY_NOTIFY
    );
    pCharacteristic->setCallbacks(new MyCallbacks());
    pCharacteristic->addDescriptor(new BLE2902());
    pService->start();

    BLEAdvertising* pAdv = BLEDevice::getAdvertising();
    pAdv->addServiceUUID(SERVICE_UUID);
    pAdv->setScanResponse(true);
    pAdv->setMinPreferred(0x06);
    pAdv->setMinPreferred(0x12);
    BLEDevice::startAdvertising();

    Serial.printf("🧠 Free after BLE  : %d bytes\n", ESP.getFreeHeap());
    Serial.println("✅ BLE Advertising: 'Memonic'");

    // 3. I2S
    initI2S();

    // Summary
    Serial.println("─────────────────────────────────");
    Serial.printf("🚀 System Ready | Free heap: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("   AUTO   : %ds = %dKB\n", AUTO_RECORD_SECONDS,   auto_total_samples   * 2 / 1024);
    Serial.printf("   ENROLL : %ds = %dKB (fallback %ds = %dKB)\n",
                  ENROLL_RECORD_SECONDS,   enroll_total_samples   * 2 / 1024,
                  FALLBACK_RECORD_SECONDS, fallback_total_samples * 2 / 1024);
    Serial.println("─────────────────────────────────");
}

// ============================================================
//  Loop
// ============================================================
void loop() {
    // WiFi watchdog
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("⚠️  WiFi lost — reconnecting...");
        WiFi.begin(ssid, password);
        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
            delay(200);
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("✅ WiFi reconnected");
        } else {
            Serial.println("❌ WiFi reconnect failed — will retry next loop");
        }
    }

    // Periodic heartbeat
    static unsigned long lastHeartbeat = 0;
    if (millis() - lastHeartbeat > HEARTBEAT_INTERVAL_MS) {
        sendHeartbeat();
        lastHeartbeat = millis();
    }

    delay(1000);
}