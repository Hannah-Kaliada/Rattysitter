#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"
#include "driver/i2s.h"
#include "time.h"
#include <WiFi.h>

#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

#define I2S_PORT I2S_NUM_1
#define SAMPLE_RATE 16000
#define MIC_WS   15
#define MIC_DATA 13
#define MIC_BCLK 2

#define SOUND_THRESHOLD 2500
#define REFRACTORY_MS 5000

unsigned long lastTrigger = 0;
void startCameraServer();

const char* ssid = "Clown";
const char* password = "12345678";

void syncTime() {
    Serial.println("[NTP] Sync...");
    configTime(3, 0, "pool.ntp.org", "time.nist.gov");
    time_t now = time(nullptr);
    while (now < 100000) {
        delay(500);
        Serial.print(".");
        now = time(nullptr);
    }
    Serial.println("\n[NTP] Done");
}


String getDateFolder() {
  struct tm timeinfo;
  getLocalTime(&timeinfo);
  char folder[16];
  strftime(folder, sizeof(folder), "/%Y-%m-%d", &timeinfo);
  return String(folder);
}


String getFileName(String folder) {
  int index = 1;
  while (true) {
    char name[32];
    sprintf(name, "%s/%04d.jpg", folder.c_str(), index);
    if (!SD_MMC.exists(name))
      return String(name);
    index++;
  }
}

bool initI2S() {
  i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 256,
    .use_apll = false
  };

  i2s_pin_config_t pins = {
    .bck_io_num = MIC_BCLK,
    .ws_io_num = MIC_WS,
    .data_out_num = -1,
    .data_in_num = MIC_DATA
  };

  if (i2s_driver_install(I2S_PORT, &config, 0, NULL) != ESP_OK)
    return false;

  i2s_set_pin(I2S_PORT, &pins);
  i2s_zero_dma_buffer(I2S_PORT);
  Serial.println("[I2S] Init OK");
  return true;
}


void deinitI2S() {
  i2s_stop(I2S_PORT);
  i2s_driver_uninstall(I2S_PORT);
  pinMode(MIC_WS, INPUT);
  pinMode(MIC_DATA, INPUT);
  pinMode(MIC_BCLK, INPUT);
  Serial.println("[I2S] Deinit");
}


bool detectSound() {
  uint8_t buffer[512];
  size_t bytesRead;


  if (i2s_read(I2S_PORT, buffer, sizeof(buffer), &bytesRead, 50) != ESP_OK)
    return false;
  int16_t *samples = (int16_t*)buffer;
  int count = bytesRead / 2;


  long sum = 0;
  for (int i = 0; i < count; i++)
    sum += abs(samples[i]);


  int rms = sum / count;


  if (rms > SOUND_THRESHOLD) {
    Serial.printf("[SOUND] RMS=%d\n", rms);
    return true;
  }
  return false;
}

bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_SVGA;
  config.jpeg_quality = 12;
  config.fb_count = 2;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = CAMERA_FB_IN_PSRAM;


  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("[CAM] Init failed");
    return false;
  }


  Serial.println("[CAM] Init OK");
  return true;
}


void deinitCamera() {
  esp_camera_deinit();
  Serial.println("[CAM] Deinit");
}


bool initSD() {
  if (!SD_MMC.begin()) {
    Serial.println("[SD] Mount failed");
    return false;
  }
  Serial.println("[SD] Init OK");
  return true;
}


void deinitSD() {
  SD_MMC.end();
  pinMode(2, INPUT);
  pinMode(4, INPUT);
  pinMode(12, INPUT);
  pinMode(13, INPUT);
  pinMode(14, INPUT);
  pinMode(15, INPUT);
  Serial.println("[SD] Deinit");
}

void capturePhoto() {

  camera_fb_t * fb = NULL;

  do {
    fb = esp_camera_fb_get();
    if (!fb) delay (30);
  } while (!fb);

  String folder = getDateFolder();
  if (!SD_MMC.exists(folder))
    SD_MMC.mkdir(folder);
  String path = getFileName(folder);

  File file = SD_MMC.open(path.c_str(), FILE_WRITE);
  if (file) {
    file.write(fb->buf, fb->len);
    file.close();
    Serial.println("[SD] Saved: " + path);
  }


  esp_camera_fb_return(fb);
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("===== SOUND TRIGGER CAM =====");


  if (!initI2S()) {
    Serial.println("I2S failed");
    while (1);
  }
      WiFi.begin(ssid, password);
    Serial.print("[WiFi] Connecting");


    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }


    Serial.println("\n[WiFi] Connected");
    Serial.print("[WiFi] IP: ");
    Serial.println(WiFi.localIP());
    syncTime();
  initCamera();
  sensor_t * s = esp_camera_sensor_get();
  s->set_whitebal(s, 1);
  s->set_wb_mode(s, 1);
  s->set_quality(s, 10);
  s->set_framesize(s, FRAMESIZE_SVGA);

  Serial.println("[CAM] Sunny WB + UXGA set");
  startCameraServer();
  Serial.println("[HTTP] Server started");
}

void loop() {


  if (millis() - lastTrigger < REFRACTORY_MS)
    return;
  if (detectSound()) {

    lastTrigger = millis();

    deinitI2S();
    delay(1000);

    if (initSD()) {
      capturePhoto();
      deinitSD();
    }

    delay(500);
    initI2S();
  }
}


