#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <driver/i2s.h>

// --- BLE UUIDs ---
#define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
#define CHARACTERISTIC_UUID "abcd1234-ab12-ab12-ab12-abcdef123456"

// --- INMP441 Pins (LOLIN S3 Mini) ---
#define I2S_SCK  5
#define I2S_WS   4
#define I2S_SD   6
#define I2S_PORT I2S_NUM_0

// --- Button ---
#define BUTTON_PIN 0  // เปลี่ยนตาม pin จริง

// --- Audio Config ---
#define SAMPLE_RATE          16000
#define AUTO_RECORD_SECONDS  5
#define ENROLL_RECORD_SECONDS 7
#define BLE_CHUNK_SIZE       512

const int auto_total_samples   = SAMPLE_RATE * AUTO_RECORD_SECONDS;
const int enroll_total_samples = SAMPLE_RATE * ENROLL_RECORD_SECONDS;

int16_t* audio_buffer  = NULL;
int      buffer_size   = 0; // จะ set ตาม mode

BLECharacteristic* pCharacteristic = NULL;
BLEServer*         pServer         = NULL;
bool deviceConnected = false;
bool isRecording     = false;
bool autoRecordEnabled = true;

// --- Forward Declaration ---
void recordAndSend(int total_samples, String startMarker);

// --- BLE Callbacks ---
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    Serial.println("✅ Phone connected via BLE");
  }
  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    Serial.println("🔴 Phone disconnected — re-advertising...");
    BLEDevice::startAdvertising();
  }
};

class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    String value = pCharacteristic->getValue().c_str();
    if (value.length() > 0) {
      String cmd = value;
      cmd.trim();
      Serial.printf("📥 BLE Command: %s\n", cmd.c_str());

      if (cmd.startsWith("ENROLL ") || cmd.startsWith("enroll ")) {
        String name = cmd.substring(7);
        name.trim();
        Serial.printf("🎙️ BLE ENROLL trigger: '%s'\n", name.c_str());
        // Record 7 seconds for enrollment
        recordAndSend(enroll_total_samples, "ENROLL " + name);
      }
      
      if (cmd.equals("RESET")) {
        Serial.println("🛑 BLE RESET trigger: Aborting recording...");
        isRecording = false;
        autoRecordEnabled = true; // Re-enable on reset
        pCharacteristic->setValue("RESET");
        pCharacteristic->notify();
      }

      if (cmd.equals("DISABLE_AUTO")) {
        Serial.println("🔒 Auto-record DISABLED (Enrollment mode)");
        autoRecordEnabled = false;
      }

      if (cmd.equals("ENABLE_AUTO")) {
        Serial.println("🔓 Auto-record ENABLED (Normal mode)");
        autoRecordEnabled = true;
      }
    }
  }
};

// --- I2S Init ---
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

// --- Record + Send via BLE ---
// mode: "START" = auto record | "ENROLL <name>" = enroll
void recordAndSend(int total_samples, String startMarker) {
  if (isRecording) return;
  isRecording = true;

  Serial.printf("\n[1] Mode: %s | Recording %d samples...\n",
                startMarker.c_str(), total_samples);
  i2s_zero_dma_buffer((i2s_port_t)I2S_PORT);

  // reallocate buffer if needed
  if (buffer_size < total_samples) {
    free(audio_buffer);
    audio_buffer = (int16_t*)malloc(total_samples * sizeof(int16_t));
    if (!audio_buffer) {
      Serial.println("❌ RAM allocation failed!");
      isRecording = false;
      return;
    }
    buffer_size = total_samples;
  }

  // อัดเสียง
  int     samples_read = 0;
  int32_t i2s_chunk[256];
  size_t  bytesIn;

  while (samples_read < total_samples) {
    i2s_read(I2S_PORT, &i2s_chunk, sizeof(i2s_chunk), &bytesIn, portMAX_DELAY);
    int valid = bytesIn / 4;
    for (int i = 0; i < valid && samples_read < total_samples; i++) {
      audio_buffer[samples_read++] = (int16_t)(i2s_chunk[i] >> 8); // ✅ correct shift
    }
  }

  // debug peak
  int32_t peak = 0;
  for (int i = 0; i < total_samples; i++) {
    if (abs(audio_buffer[i]) > peak) peak = abs(audio_buffer[i]);
  }
  Serial.printf("[2] Recorded %d samples | peak: %d\n", total_samples, peak);

  if (!deviceConnected) {
    Serial.println("⚠️ No phone — recorded but not sent");
    isRecording = false;
    return;
  }

  // ส่ง START marker (บอก phone ว่าเป็น mode อะไร)
  pCharacteristic->setValue(startMarker.c_str());
  pCharacteristic->notify();
  delay(50);

  // ส่ง PCM chunks
  const size_t total_bytes = total_samples * sizeof(int16_t);
  size_t offset = 0;
  int chunk_count = 0;

  Serial.println("[3] Sending via BLE...");
  while (offset < total_bytes) {
    if (!deviceConnected) {
      Serial.println("❌ Disconnected mid-send!");
      isRecording = false;
      return;
    }
    size_t to_send = min((size_t)BLE_CHUNK_SIZE, total_bytes - offset);
    pCharacteristic->setValue((uint8_t*)audio_buffer + offset, to_send);
    pCharacteristic->notify();
    offset += to_send;
    chunk_count++;
    delay(20);
  }

  // ส่ง END marker
  pCharacteristic->setValue((uint8_t*)"END", 3);
  pCharacteristic->notify();

  Serial.printf("[4] ✅ Done! %d chunks (%d bytes) + END\n", chunk_count, total_bytes);
  isRecording = false;
}

// --- Setup ---
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("============================");
  Serial.println("  Memonic Bracelet (BLE)");
  Serial.println("============================");
  Serial.println("Commands (Serial Monitor):");
  Serial.println("  ENROLL <name>  — Record voice profile");
  Serial.println("  Button         — Auto record 5s");
  Serial.println("============================");

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // จอง RAM สำหรับ buffer ใหญ่สุด (enroll = 7s)
  audio_buffer = (int16_t*)malloc(enroll_total_samples * sizeof(int16_t));
  if (!audio_buffer) {
    Serial.println("❌ RAM allocation failed!");
    while (true) delay(1000);
  }
  buffer_size = enroll_total_samples;
  Serial.printf("✅ RAM OK: %d bytes\n", enroll_total_samples * (int)sizeof(int16_t));

  initI2S();
  Serial.println("✅ I2S ready");

  // BLE init
  BLEDevice::init("Memonic");
  pServer = BLEDevice::createServer();
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
  BLEDevice::startAdvertising();

  Serial.println("✅ BLE advertising as 'Memonic'");
  Serial.println("⏳ Waiting for button or Serial command...");
}

// --- Loop ---
void loop() {
  // Serial command → ENROLL mode
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.startsWith("ENROLL ") || cmd.startsWith("enroll ")) {
      String name = cmd.substring(7);
      name.trim();
      Serial.printf("🎙️ ENROLL: '%s'\n", name.c_str());
      Serial.println("3..."); delay(1000);
      Serial.println("2..."); delay(1000);
      Serial.println("1..."); delay(1000);
      recordAndSend(enroll_total_samples, "ENROLL " + name);
    }
  }

  // Button → AUTO record (Only if enabled)
  if (digitalRead(BUTTON_PIN) == LOW && !isRecording && autoRecordEnabled) {
    delay(50); // debounce
    if (digitalRead(BUTTON_PIN) == LOW) {
      recordAndSend(auto_total_samples, "START");
    }
  }

  delay(10);
}