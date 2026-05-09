#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <driver/i2s.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

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

// ✅ ลด AUTO เหลือ 2s (64KB) เพราะไม่ต้องการ accuracy สูงเท่า enroll
// ENROLL คง 3s (96KB) เพราะต้องการ voice profile ที่แม่นยำ
#define AUTO_RECORD_SECONDS    2
#define ENROLL_RECORD_SECONDS  3

const int auto_total_samples   = SAMPLE_RATE * AUTO_RECORD_SECONDS;   // 32,000 → 64KB
const int enroll_total_samples = SAMPLE_RATE * ENROLL_RECORD_SECONDS; // 48,000 → 96KB

// ✅ ไม่มี global audio_buffer อีกต่อไป
// malloc เฉพาะช่วงอัดเสียง → free ทันทีหลังส่ง
// ช่วง idle: RAM ว่างให้ WiFi/BLE ใช้งานเต็มที่

BLECharacteristic* pCharacteristic = NULL;
bool deviceConnected = false;
bool isRecording     = false;

void recordAndSend(int total_samples, String mode, String enrollUser = "");

// --- BLE Callbacks ---
class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        deviceConnected = true;
        Serial.println("📱 BLE Client Connected");
    }
    void onDisconnect(BLEServer* pServer) {
        deviceConnected = false;
        BLEDevice::startAdvertising();
        Serial.println("📱 BLE Client Disconnected, restarting advertising");
    }
};

class MyCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pCharacteristic) {
        String value = pCharacteristic->getValue().c_str();
        if (value.length() > 0) {
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
            }
        }
    }
};

// --- Init ---
void initWiFi() {
    Serial.print("Connecting to WiFi");
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
        // ✅ ลด DMA: 4 × 64 × 4 bytes = 1,024 bytes (เดิม 8 × 256 × 4 = 8,192 bytes)
        // ประหยัด ~7KB internal RAM ไม่กระทบเสียงเพราะ recording loop อ่านเร็วกว่า I2S เติม
        .dma_buf_count        = 4,
        .dma_buf_len          = 64,
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

void writeWavHeader(byte* header, int dataLength) {
    unsigned int sampleRate = SAMPLE_RATE;
    unsigned int byteRate   = SAMPLE_RATE * 2;
    unsigned int fileSize   = 36 + dataLength;

    header[0]='R'; header[1]='I'; header[2]='F'; header[3]='F';
    header[4]=(byte)(fileSize);         header[5]=(byte)(fileSize>>8);
    header[6]=(byte)(fileSize>>16);     header[7]=(byte)(fileSize>>24);
    header[8]='W'; header[9]='A'; header[10]='V'; header[11]='E';
    header[12]='f'; header[13]='m'; header[14]='t'; header[15]=' ';
    header[16]=16; header[17]=0; header[18]=0; header[19]=0;
    header[20]=1;  header[21]=0;
    header[22]=1;  header[23]=0;
    header[24]=(byte)(sampleRate);      header[25]=(byte)(sampleRate>>8);
    header[26]=(byte)(sampleRate>>16);  header[27]=(byte)(sampleRate>>24);
    header[28]=(byte)(byteRate);        header[29]=(byte)(byteRate>>8);
    header[30]=(byte)(byteRate>>16);    header[31]=(byte)(byteRate>>24);
    header[32]=2; header[33]=0;
    header[34]=16; header[35]=0;
    header[36]='d'; header[37]='a'; header[38]='t'; header[39]='a';
    header[40]=(byte)(dataLength);      header[41]=(byte)(dataLength>>8);
    header[42]=(byte)(dataLength>>16);  header[43]=(byte)(dataLength>>24);
}

void recordAndSend(int total_samples, String mode, String enrollUser) {
    if (isRecording) return;
    isRecording = true;

    int needed_bytes = total_samples * sizeof(int16_t);
    Serial.printf("🧠 Free heap before alloc: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("📦 Requesting %d bytes for audio buffer...\n", needed_bytes);

    // ✅ Allocate เฉพาะตอนจะใช้งาน ไม่จองทิ้งไว้ตลอด
    int16_t* audio_buffer = (int16_t*)malloc(needed_bytes);

    if (!audio_buffer) {
        Serial.println("❌ malloc failed — not enough RAM");
        Serial.printf("   Free heap: %d bytes, Needed: %d bytes\n", ESP.getFreeHeap(), needed_bytes);
        isRecording = false;
        if (pCharacteristic) {
            pCharacteristic->setValue("ERROR_RAM");
            pCharacteristic->notify();
        }
        return;
    }
    Serial.printf("✅ Buffer allocated | Free heap remaining: %d bytes\n", ESP.getFreeHeap());

    // --- RECORD: อัดเสียงลง buffer ก่อน (ไม่มี network interrupt เสียงไม่กระตุก) ---
    Serial.printf("🎙️ Recording %ds (%d samples)...\n",
                  total_samples / SAMPLE_RATE, total_samples);

    int32_t i2s_chunk[64]; // match dma_buf_len
    size_t  bytesIn;
    int     samples_read = 0;

    while (samples_read < total_samples && isRecording) {
        i2s_read(I2S_PORT, i2s_chunk, sizeof(i2s_chunk), &bytesIn, portMAX_DELAY);
        int valid = bytesIn / 4;
        for (int i = 0; i < valid && samples_read < total_samples; i++) {
            int32_t val = (i2s_chunk[i] >> 8) * 8; // Gain x8
            audio_buffer[samples_read++] = (int16_t)constrain(val, -32768, 32767);
        }
    }
    Serial.printf("✅ Recorded %d samples\n", samples_read);

    // --- SEND: ส่งหลังอัดเสร็จ (record first, send later) ---
    if (!isRecording) {
        // ถูก RESET ระหว่างอัด
        Serial.println("⚠️ Recording cancelled by RESET");
        free(audio_buffer);
        isRecording = false;
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("❌ WiFi Disconnected");
        free(audio_buffer);
        isRecording = false;
        if (pCharacteristic) { pCharacteristic->setValue("ERROR_WIFI"); pCharacteristic->notify(); }
        return;
    }

    Serial.println("☁️ Connecting to server...");
    WiFiClientSecure client;
    client.setInsecure();
    // ✅ Optimize SSL buffers to fit in remaining RAM (96KB used by audio)
    client.setBufferSizes(1024, 512); 

    if (!client.connect(serverHost, serverPort)) {
        Serial.println("❌ Connection to server failed!");
        free(audio_buffer);
        isRecording = false;
        if (pCharacteristic) { pCharacteristic->setValue("ERROR_SERVER"); pCharacteristic->notify(); }
        return;
    }

    // Build multipart
    String boundary = "----ESP32Boundary";
    int dataLen     = samples_read * 2; // 16-bit PCM

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

    // HTTP Request Headers
    client.print(String("POST ") + serverPath + " HTTP/1.1\r\n");
    client.print(String("Host: ") + serverHost + "\r\n");
    client.print("Content-Type: multipart/form-data; boundary=" + boundary + "\r\n");
    client.print("Content-Length: " + String(totalLen) + "\r\n");
    client.print("Connection: close\r\n\r\n");

    // WAV Header + Audio Data
    byte wavHeader[44];
    writeWavHeader(wavHeader, dataLen);
    client.print(head);
    client.write(wavHeader, 44);

    // Stream audio in 2KB chunks
    int offset    = 0;
    int chunkSize = 2048;
    while (offset < dataLen) {
        int toSend = min(chunkSize, dataLen - offset);
        client.write((uint8_t*)audio_buffer + offset, toSend);
        offset += toSend;
    }
    client.print(tail);

    // ✅ Free ทันทีหลังส่งข้อมูลเสร็จ ไม่ต้องรอ response
    // ทำให้ RAM ว่างเร็วที่สุด
    free(audio_buffer);
    audio_buffer = nullptr;
    Serial.printf("🗑️ Buffer freed | Free heap: %d bytes\n", ESP.getFreeHeap());

    // Read Response
    Serial.println("⏳ Waiting for server response...");
    while (client.connected() && !client.available()) { delay(10); }
    while (client.connected()) {
        String line = client.readStringUntil('\n');
        if (line == "\r") break; // end of HTTP headers
    }
    String response = client.readString();
    Serial.printf("✅ Server Response: %s\n", response.c_str());

    if (pCharacteristic) {
        pCharacteristic->setValue("SUCCESS");
        pCharacteristic->notify();
    }

    client.stop();
    isRecording = false;
}

void sendHeartbeat() {
    if (WiFi.status() == WL_CONNECTED) {
        WiFiClientSecure client;
        client.setInsecure();
        if (client.connect(serverHost, serverPort)) {
            String body = "{\"bracelet\":\"Connected\",\"dock\":\"Connected\"}";
            client.print(String("POST /update HTTP/1.1\r\n") +
                         "Host: " + serverHost + "\r\n" +
                         "Content-Type: application/json\r\n" +
                         "Content-Length: " + String(body.length()) + "\r\n" +
                         "Connection: close\r\n\r\n" +
                         body);
            client.stop();
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("============================");
    Serial.println("  Memonic — ESP32-S3 Mini");
    Serial.println("============================");
    Serial.printf("🧠 Total heap: %d bytes\n", ESP.getHeapSize());
    Serial.printf("🧠 Free heap at boot: %d bytes\n", ESP.getFreeHeap());

    // ✅ ไม่ต้อง allocate buffer ที่นี่อีกต่อไป

    initWiFi();
    sendHeartbeat(); // First report to cloud
    Serial.printf("🧠 Free heap after WiFi: %d bytes\n", ESP.getFreeHeap());
    delay(500);

    // BLE Init
    BLEDevice::init("Memonic");
    BLEServer* pServer = BLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());

    BLEService* pService = pServer->createService(SERVICE_UUID);
    pCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ |
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

    Serial.printf("🧠 Free heap after BLE: %d bytes\n", ESP.getFreeHeap());
    Serial.println("✅ BLE Advertising: 'Memonic'");

    initI2S();
    Serial.printf("🚀 System Ready | Free heap: %d bytes\n", ESP.getFreeHeap());
    Serial.println("   AUTO  record: " + String(AUTO_RECORD_SECONDS) + "s = " + String(auto_total_samples * 2 / 1024) + "KB");
    Serial.println("   ENROLL record: " + String(ENROLL_RECORD_SECONDS) + "s = " + String(enroll_total_samples * 2 / 1024) + "KB");
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("⚠️ WiFi lost, reconnecting...");
        WiFi.begin(ssid, password);
        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 5000) {
            delay(100);
        }
    }

    // Periodic heartbeat every 30 seconds
    static unsigned long lastHeartbeat = 0;
    if (millis() - lastHeartbeat > 30000) {
        sendHeartbeat();
        lastHeartbeat = millis();
    }

    delay(1000);
}