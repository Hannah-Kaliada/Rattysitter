#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include "FS.h"
#include "SD_MMC.h"
#include "driver/i2s.h"
#include "time.h"
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define LED_R_CH 0
#define LED_G_CH 1
#define LED_B_CH 2

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

Adafruit_PWMServoDriver pca(0x40);

const char* ssid = "Clown";
const char* password = "12345678";
const char* serverUrl = "http://10.241.52.96:8080/upload";

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


#define SOUND_THRESHOLD 1000
#define REFRACTORY_MS 1000


unsigned long lastTrigger = 0;
unsigned long lastMinuteSave = 0;
unsigned long lastSensorUpdate = 0;
unsigned long lastSend = 0;


#define SAVE_INTERVAL 60000
#define SENSOR_INTERVAL 60000


float ahtTemp = 0;
float ahtHum = 0;

bool readAHT10(float &temperature, float &humidity) {


  Wire.beginTransmission(0x38);
  Wire.write(0xAC);
  Wire.write(0x33);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;


  delay(80);


  Wire.requestFrom(0x38, 6);
  if (Wire.available() != 6) return false;


  uint8_t data[6];
  for (int i = 0; i < 6; i++) data[i] = Wire.read();


  uint32_t rawHum = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (data[3] >> 4);
  uint32_t rawTemp = ((uint32_t)(data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | data[5];


  humidity = (rawHum * 100.0) / 1048576.0;
  temperature = ((rawTemp * 200.0) / 1048576.0) - 50;


  return true;
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
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 12;
  config.fb_count = 2;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = CAMERA_FB_IN_PSRAM;


  return (esp_camera_init(&config) == ESP_OK);
}


void setRGB(uint16_t r, uint16_t g, uint16_t b) {
  pca.setPWM(LED_R_CH, 0, r);
  pca.setPWM(LED_G_CH, 0, g);
  pca.setPWM(LED_B_CH, 0, b);
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


  return true;
}


void deinitI2S() {
  i2s_stop(I2S_PORT);
  i2s_driver_uninstall(I2S_PORT);
}


bool detectSound() {


  uint8_t buffer[512];
  size_t bytesRead;


  if (i2s_read(I2S_PORT, buffer, sizeof(buffer), &bytesRead, 20) != ESP_OK)
    return false;


  int16_t *samples = (int16_t*)buffer;
  int count = bytesRead / 2;


  long sum = 0;
  for (int i = 0; i < count; i++)
    sum += abs(samples[i]);


  int rms = sum / count;


  return (rms > SOUND_THRESHOLD);
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


void saveToSD(camera_fb_t *fb) {
  if (!SD_MMC.begin()) return;

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


  SD_MMC.end();
}

void sendFrame(camera_fb_t *fb) {
  HTTPClient http;
  http.begin(serverUrl);
  http.addHeader("Content-Type", "image/jpeg");
  http.POST(fb->buf, fb->len);
  http.end();
}

void setup() {


  Serial.begin(115200);


  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
    delay(500);


  initCamera();

  configTime(3, 0, "pool.ntp.org", "time.nist.gov");

  initI2S();
  Wire.begin(26,27);
  Wire.setClock(100000);
  lastMinuteSave = millis();
  pca.begin();
  pca.setPWMFreq(1000);
  setRGB(0,0,0);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED init failed");
    while (true);
  }

  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);
  display.println("System OK");
  display.display();

}

void loop() {


  unsigned long now = millis();

  if (now - lastTrigger > REFRACTORY_MS) {
    if (detectSound()) {


      lastTrigger = now;


      camera_fb_t * fb = esp_camera_fb_get();
      if (fb) {
        deinitI2S();
        saveToSD(fb);
        initI2S();
        esp_camera_fb_return(fb);
      }
    }
  }

  if (now - lastSend > 200) {
    lastSend = now;


    camera_fb_t * fb = esp_camera_fb_get();
    if (fb) {
      sendFrame(fb);
      esp_camera_fb_return(fb);
    }
  }

  if (now - lastMinuteSave > SAVE_INTERVAL) {
    lastMinuteSave += SAVE_INTERVAL;


    camera_fb_t * fb = esp_camera_fb_get();
    if (fb) {
      deinitI2S();
      saveToSD(fb);
      initI2S();
      esp_camera_fb_return(fb);
    }
  }

  if (now - lastSensorUpdate > SENSOR_INTERVAL) {
    lastSensorUpdate += SENSOR_INTERVAL;

    if (readAHT10(ahtTemp, ahtHum)) {
      Serial.printf("[AHT10] T=%.1fC H=%.1f%%\n", ahtTemp, ahtHum);
      setRGB(4095,0,0);
      delay(1000);
      setRGB(0,0,0);
    }
  }
}
