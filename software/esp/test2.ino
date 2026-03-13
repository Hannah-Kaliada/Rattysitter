#include "esp_camera.h"
#include <WiFi.h>
#include <WebSocketsClient.h>
#include "FS.h"
#include "SD_MMC.h"
#include "driver/i2s.h"
#include "time.h"
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_PCF8574.h>
#include <Ticker.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>


#define PCF8574_ADDRESS 0x20
#define TOUCH_PIN 0
#define DEBOUNCE_DELAY 50


#define LED_R_CH 0
#define LED_G_CH 1
#define LED_B_CH 2


#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C


SemaphoreHandle_t cameraMutex;
SemaphoreHandle_t i2sMutex;


Adafruit_PCF8574 pcf;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Adafruit_PWMServoDriver pca(0x40);


WebSocketsClient ws;


bool streaming = false;
bool audioStreaming = false;


unsigned long lastFrame = 0;
const int FRAME_INTERVAL = 100;   // ↓ снижено для стабильности сети (~10 FPS)


#define I2S_PORT I2S_NUM_1
#define SAMPLE_RATE 16000
#define MIC_WS   15
#define MIC_DATA 13
#define MIC_BCLK 2


// ===== АУДИО БЕЗОПАСНЫЙ РАЗМЕР =====
#define AUDIO_SAMPLES 2048                 // ~128 мс
#define AUDIO_BYTES (AUDIO_SAMPLES * 2)    // PCM16
uint8_t audioBuf[AUDIO_BYTES];


const char* ssid = "Clown";
const char* password = "12345678";


const char* ws_host = "10.241.52.96";
const uint16_t ws_port = 8080;
const char* ws_path = "/ws";


// ===== CAMERA PINS =====
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


// =========================================================
// ===================== INIT ==============================
// =========================================================


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
  config.fb_count = 1;
  return esp_camera_init(&config) == ESP_OK;
}


bool initI2S() {
  i2s_driver_uninstall(I2S_PORT);


  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 512
  };


  i2s_pin_config_t pins = {
    .bck_io_num = MIC_BCLK,
    .ws_io_num = MIC_WS,
    .data_out_num = -1,
    .data_in_num = MIC_DATA
  };


  if (i2s_driver_install(I2S_PORT, &cfg, 0, NULL) != ESP_OK) return false;
  if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) return false;


  i2s_zero_dma_buffer(I2S_PORT);
  return true;
}


// =========================================================
// ===================== SENDERS ===========================
// =========================================================


void sendBinaryPacket(uint16_t type, const uint8_t* data, uint16_t len){
  uint8_t* packet = (uint8_t*)malloc(4 + len);
  if(!packet) return;


  packet[0] = type & 0xFF;
  packet[1] = type >> 8;
  packet[2] = len & 0xFF;
  packet[3] = len >> 8;


  memcpy(packet + 4, data, len);


  ws.sendBIN(packet, 4 + len);
  free(packet);
}



void sendPhoto(){
  if (xSemaphoreTake(cameraMutex, portMAX_DELAY) != pdTRUE) return;


  camera_fb_t *fb = esp_camera_fb_get();
  if (fb){
    sendBinaryPacket(1, fb->buf, fb->len);
    esp_camera_fb_return(fb);
  }


  xSemaphoreGive(cameraMutex);
}


void sendAudio(){
  if (!audioStreaming) return;
  if (xSemaphoreTake(i2sMutex, 0) != pdTRUE) return;


  size_t bytesRead = 0;
  if (i2s_read(I2S_PORT, audioBuf, AUDIO_BYTES, &bytesRead, portMAX_DELAY) == ESP_OK){
    sendBinaryPacket(2, audioBuf, bytesRead);
  }


  xSemaphoreGive(i2sMutex);
}


// =========================================================
// ===================== WS EVENTS =========================
// =========================================================


void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  if(type != WStype_TEXT) return;
  String cmd = String((char*)payload);


  if(cmd == "stream_on") streaming = true;
  else if(cmd == "stream_off") streaming = false;
  else if(cmd == "audio_on") audioStreaming = true;
  else if(cmd == "audio_off") audioStreaming = false;
  else if(cmd == "photo") sendPhoto();
}


// =========================================================
// ===================== SETUP =============================
// =========================================================


void setup(){
  Serial.begin(115200);


  WiFi.begin(ssid,password);
  WiFi.setSleep(false);                 // ⭐ критично
  while(WiFi.status()!=WL_CONNECTED) delay(300);


  cameraMutex = xSemaphoreCreateMutex();
  i2sMutex = xSemaphoreCreateMutex();


  initCamera();
  initI2S();


  ws.begin(ws_host, ws_port, ws_path, "arduino");
  ws.onEvent(webSocketEvent);
  ws.setReconnectInterval(2000);
}


// =========================================================
// ===================== LOOP ==============================
// =========================================================


void loop(){
  ws.loop();
  unsigned long now = millis();


  if(streaming && now - lastFrame > FRAME_INTERVAL){
    lastFrame = now;
    sendPhoto();
  }


  sendAudio();
}



