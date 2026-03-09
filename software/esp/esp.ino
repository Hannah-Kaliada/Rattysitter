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

#define PCF8574_ADDRESS 0x20
#define TOUCH_PIN 0
#define DEBOUNCE_DELAY 50
#define BUTTON_UPDATE_INTERVAL 10

#define LED_R_CH 0
#define LED_G_CH 1
#define LED_B_CH 2


#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C


Adafruit_PCF8574 pcf;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Adafruit_PWMServoDriver pca(0x40);

QueueHandle_t soundEventQueue = nullptr;
TaskHandle_t soundTaskHandle = nullptr;


Ticker buttonTicker;
volatile bool checkButtonFlag = false;


bool streaming = false;
unsigned long lastFrame = 0;
const int FRAME_INTERVAL = 10; // ~10 FPS

void IRAM_ATTR onButtonTimer() {
    checkButtonFlag = true;
}

WebSocketsClient ws;


const char* ssid = "Clown";
const char* password = "12345678";


const char* ws_host = "10.241.52.96";
const uint16_t ws_port = 8080;
const char* ws_path = "/ws";


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
unsigned long lastSensorUpdate = 0;


#define SENSOR_INTERVAL 60000


float ahtTemp = 0;
float ahtHum = 0;

void soundTask(void *pvParameters) {
    while (1) {
        if (detectSound()) {
            // Отправляем сигнал в основную задачу (просто число, например 1)
            int evt = 1;
            xQueueSend(soundEventQueue, &evt, 0);
        }
        // Небольшая задержка для экономии ресурсов (можно подобрать)
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}


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
  config.fb_count = 1;


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
    .dma_buf_len = 256
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


void sendPhoto() {


  camera_fb_t * fb = esp_camera_fb_get();


  if (!fb) return;


  ws.sendBIN(fb->buf, fb->len);


  esp_camera_fb_return(fb);
}


void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {


  if(type == WStype_TEXT) {


    String cmd = String((char*)payload);


if(cmd == "stream_on"){
    streaming = true;
}

else if(cmd == "stream_off"){
    streaming = false;
}

else if(cmd == "photo"){
    sendPhoto();
}


    else if(cmd == "get_sensors") {


      char msg[64];
      sprintf(msg,"temp:%.1f hum:%.1f",ahtTemp,ahtHum);
      ws.sendTXT(msg);


    }


    else if(cmd.startsWith("rgb")) {


      int r,g,b;
      sscanf(cmd.c_str(),"rgb %d %d %d",&r,&g,&b);
      setRGB(r,g,b);


    }


    else if(cmd == "reboot") {


      ESP.restart();


    }
  }
}


void setup() {


  Serial.begin(115200);


  WiFi.begin(ssid,password);


  while(WiFi.status()!=WL_CONNECTED)
    delay(500);

  initCamera();
  Wire.begin(26,27);
  initI2S();
soundEventQueue = xQueueCreate(5, sizeof(int)); // очередь на 5 элементов
xTaskCreatePinnedToCore(
    soundTask,      // функция задачи
    "SoundTask",    // имя
    4096,           // размер стека
    NULL,           // параметры
    1,              // приоритет (меньше, чем у основной задачи)
    &soundTaskHandle,
    0               // ядро 0 (обычно для фоновых задач)
);
  Wire.begin(26,27);
  Wire.setClock(100000);
  pca.begin();
  pca.setPWMFreq(1000);
  setRGB(0,0,0);
  if (pcf.begin(PCF8574_ADDRESS, &Wire)) {
    Serial.println("Adafruit PCF8574 OK на пинах 21 и 22");
  
    pcf.pinMode(TOUCH_PIN, INPUT_PULLUP); 
  } else {
    Serial.println("Adafruit PCF8574 ERROR");
  }

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



  ws.begin(ws_host,ws_port,ws_path);
  ws.onEvent(webSocketEvent);
  ws.setReconnectInterval(2000);
  buttonTicker.attach_ms(20, onButtonTimer);

}


void loop() {


  ws.loop();


  unsigned long now = millis();

    // Проверка по флагу от таймера (быстрее чем в основном цикле)
    if (checkButtonFlag) {
        checkButtonFlag = false;
        
        static int lastButtonState = HIGH;
        static unsigned long lastDebounceTime = 0;
        
        int reading = pcf.digitalRead(TOUCH_PIN);
        
        if (reading != lastButtonState) {
            lastDebounceTime = now;
        }
        
        if ((now - lastDebounceTime) > DEBOUNCE_DELAY) {
            static int buttonState = HIGH;
            static bool lastSentState = false;
            
            if (reading != buttonState) {
                buttonState = reading;
                
                if (buttonState == HIGH) {
                    setRGB(4095, 0, 0);
                    lastSentState = true;
                } else {
                    setRGB(0, 0, 0);
                    lastSentState = false;
                }
            }
        }
        
        lastButtonState = reading;
    }

  int soundEvent;
    if (xQueueReceive(soundEventQueue, &soundEvent, 0) == pdTRUE) {
        if (millis() - lastTrigger > REFRACTORY_MS) {
            lastTrigger = millis();
            ws.sendTXT("sound");
        }
    }


  if(now-lastSensorUpdate>SENSOR_INTERVAL) {

    lastSensorUpdate=now;

    if(readAHT10(ahtTemp,ahtHum)) {
      char msg[64];
      sprintf(msg,"temp:%.1f hum:%.1f",ahtTemp,ahtHum);
      ws.sendTXT(msg);
    }
  }
if(streaming && millis() - lastFrame > FRAME_INTERVAL){
    lastFrame = millis();
    sendPhoto();
}
}
