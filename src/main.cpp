#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <driver/i2s.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// --- WiFi Config ---
const char* ssid = "Fais";
const char* password = "12345678";
const char* serverUrl = "https://8001-01kkh2et3bdjymj2fjq6jabg8k.cloudspaces.litng.ai/audio";

// --- BLE UUIDs ---
#define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
#define CHARACTERISTIC_UUID "abcd1234-ab12-ab12-ab12-abcdef123456"

// --- INMP441 Pins (LOLIN S3 Mini) ---
#define I2S_SCK  5
#define I2S_WS   4
#define I2S_SD   6
#define I2S_PORT I2S_NUM_0

// --- Audio Config ---
#define SAMPLE_RATE          16000
#define AUTO_RECORD_SECONDS  5
#define ENROLL_RECORD_SECONDS 7

const int auto_total_samples   = SAMPLE_RATE * AUTO_RECORD_SECONDS;
const int enroll_total_samples = SAMPLE_RATE * ENROLL_RECORD_SECONDS;

int16_t* audio_buffer  = NULL;
int      buffer_size   = 0;

BLECharacteristic* pCharacteristic = NULL;
bool deviceConnected = false;
bool isRecording     = false;

// --- Forward Declaration ---
void recordAndSend(int total_samples, String mode, String enrollUser = "");

// --- BLE Callbacks ---
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) { deviceConnected = true; }
  void onDisconnect(BLEServer* pServer) { deviceConnected = false; BLEDevice::startAdvertising(); }
};

class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    String value = pCharacteristic->getValue().c_str();
    if (value.length() > 0) {
      String cmd = value; cmd.trim();
      Serial.printf("📥 BLE Command: %s\n", cmd.c_str());

      if (cmd.startsWith("ENROLL ")) {
        String name = cmd.substring(7); name.trim();
        recordAndSend(enroll_total_samples, "ENROLL", name);
      } else if (cmd.equals("START")) {
        recordAndSend(auto_total_samples, "START");
      } else if (cmd.equals("RESET")) {
        isRecording = false;
      }
    }
  }
};

void initWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ WiFi Connected");
}

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
  i2s_pin_config_t pins = { .bck_io_num = I2S_SCK, .ws_io_num = I2S_WS, .data_out_num = I2S_PIN_NO_CHANGE, .data_in_num = I2S_SD };
  i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
  i2s_set_pin(I2S_PORT, &pins);
  i2s_start(I2S_PORT);
}

void writeWavHeader(byte* header, int dataLength) {
  unsigned int sampleRate = SAMPLE_RATE;
  unsigned int byteRate = SAMPLE_RATE * 2;
  header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
  unsigned int fileSize = 36 + dataLength;
  header[4] = (byte)(fileSize & 0xFF); header[5] = (byte)((fileSize >> 8) & 0xFF);
  header[6] = (byte)((fileSize >> 16) & 0xFF); header[7] = (byte)((fileSize >> 24) & 0xFF);
  header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';
  header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
  header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0;
  header[20] = 1; header[21] = 0; header[22] = 1; header[23] = 0;
  header[24] = (byte)(sampleRate & 0xFF); header[25] = (byte)((sampleRate >> 8) & 0xFF);
  header[26] = (byte)((sampleRate >> 16) & 0xFF); header[27] = (byte)((sampleRate >> 24) & 0xFF);
  header[28] = (byte)(byteRate & 0xFF); header[29] = (byte)((byteRate >> 8) & 0xFF);
  header[30] = (byte)((byteRate >> 16) & 0xFF); header[31] = (byte)((byteRate >> 24) & 0xFF);
  header[32] = 2; header[33] = 0; header[34] = 16; header[35] = 0;
  header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
  header[40] = (byte)(dataLength & 0xFF); header[41] = (byte)((dataLength >> 8) & 0xFF);
  header[42] = (byte)((dataLength >> 16) & 0xFF); header[43] = (byte)((dataLength >> 24) & 0xFF);
}

void recordAndSend(int total_samples, String mode, String enrollUser) {
  if (isRecording) return;
  isRecording = true;
  Serial.printf("🎙️ Recording %d samples...\n", total_samples);

  if (buffer_size < total_samples) {
    free(audio_buffer);
    audio_buffer = (int16_t*)malloc(total_samples * sizeof(int16_t));
    if (!audio_buffer) { Serial.println("❌ RAM Failed"); isRecording = false; return; }
    buffer_size = total_samples;
  }

  int samples_read = 0;
  int32_t i2s_chunk[256];
  size_t bytesIn;
  while (samples_read < total_samples && isRecording) {
    i2s_read(I2S_PORT, &i2s_chunk, sizeof(i2s_chunk), &bytesIn, portMAX_DELAY);
    int valid = bytesIn / 4;
    for (int i = 0; i < valid && samples_read < total_samples; i++) {
      int32_t val = (i2s_chunk[i] >> 8) * 8; // Gain x8
      audio_buffer[samples_read++] = (int16_t)constrain(val, -32768, 32767);
    }
  }

  if (WiFi.status() != WL_CONNECTED) { Serial.println("❌ WiFi Disconnected"); isRecording = false; return; }

  Serial.println("☁️ Uploading to Cloud...");
  WiFiClientSecure client;
  client.setInsecure(); // For lighting.ai SSL
  HTTPClient http;
  http.begin(client, serverUrl);

  String boundary = "----ESP32Boundary";
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

  int dataLen = total_samples * 2;
  byte header[44];
  writeWavHeader(header, dataLen);

  // Construct Multipart Body
  String head = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
  String tail = "\r\n";
  if (mode == "ENROLL") {
    tail += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"enroll_user\"\r\n\r\n" + enrollUser + "\r\n";
  }
  tail += "--" + boundary + "--\r\n";

  int totalLen = head.length() + 44 + dataLen + tail.length();
  http.addHeader("Content-Length", String(totalLen));

  int res = http.POST((uint8_t*)NULL, totalLen); // Start streaming POST
  if (res > 0) {
    WiFiClient* stream = http.getStreamPtr();
    stream->print(head);
    stream->write(header, 44);
    stream->write((uint8_t*)audio_buffer, dataLen);
    stream->print(tail);
    String response = http.getString();
    Serial.printf("✅ Server: %s\n", response.c_str());
    if (pCharacteristic) { pCharacteristic->setValue(response.c_str()); pCharacteristic->notify(); }
  } else {
    Serial.printf("❌ POST failed: %s\n", http.errorToString(res).c_str());
  }
  http.end();
  isRecording = false;
}

void sendHeartbeat() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    // URL from your config
    http.begin(client, "https://8001-01kkh2et3bdjymj2fjq6jabg8k.cloudspaces.litng.ai/update");
    http.addHeader("Content-Type", "application/json");
    http.POST("{\"bracelet\":\"Connected\",\"dock\":\"Connected\"}");
    http.end();
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("============================");
  Serial.println("  Memonic (WiFi + BLE)");
  Serial.println("============================");

  // 1. Start BLE First (Priority for Phone Connection)
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
  Serial.println("✅ BLE Advertising: 'Memonic'");

  // 2. Initialize Hardware & WiFi
  initI2S();
  initWiFi(); 
  sendHeartbeat(); // First report

  // Allocate RAM for buffer if not done yet
  if (!audio_buffer) {
    audio_buffer = (int16_t*)malloc(enroll_total_samples * sizeof(int16_t));
    buffer_size = enroll_total_samples;
  }

  Serial.println("🚀 System Ready");
}

unsigned long lastHeartbeat = 0;
void loop() {
  // WiFi maintenance
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ WiFi lost, reconnecting...");
    WiFi.begin(ssid, password);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 5000) {
      delay(100);
    }
  }

  // Periodic heartbeat every 30 seconds
  if (millis() - lastHeartbeat > 30000) {
    sendHeartbeat();
    lastHeartbeat = millis();
  }
  
  delay(1000);
}