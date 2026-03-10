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
#define BUTTON_UPDATE_INTERVAL 10

#define LED_R_CH 0
#define LED_G_CH 1
#define LED_B_CH 2


#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C



xSemaphoreHandle cameraMutex;           // мьютекс для доступа к камере
xSemaphoreHandle i2sMutex;              // мьютекс для доступа к I2S
unsigned long lastSDPhoto = 0;           // время последнего сохранения на SD
const unsigned long SD_PHOTO_INTERVAL = 60000; // интервал 1 минута


Adafruit_PCF8574 pcf;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Adafruit_PWMServoDriver pca(0x40);

QueueHandle_t soundEventQueue = nullptr;
TaskHandle_t soundTaskHandle = nullptr;


Ticker buttonTicker;
volatile bool checkButtonFlag = false;


bool streaming = false;
unsigned long lastFrame = 0;
const int FRAME_INTERVAL = 10; 

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
            int evt = 1;
            xQueueSend(soundEventQueue, &evt, 0);
        }
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
    // Пытаемся удалить предыдущий драйвер (если был)
    esp_err_t err = i2s_driver_uninstall(I2S_PORT);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.println("I2S uninstall failed");
        return false;
    }

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

    if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK)
        return false;

    i2s_zero_dma_buffer(I2S_PORT);
    return true;
}


void savePhotoToSD() {
    // 1. Захватываем мьютекс I2S – блокируем звук
    xSemaphoreTake(i2sMutex, portMAX_DELAY);

    // 2. Деинициализируем I2S (освобождаем пины)
    esp_err_t err = i2s_driver_uninstall(I2S_PORT);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.println("I2S uninstall error");
    }
    delay(10); // небольшая пауза

    // 3. Инициализируем SD-карту
    if (!SD_MMC.begin()) {
        Serial.println("SD Card Mount Failed");
        // Возвращаем I2S обратно
        initI2S();
        xSemaphoreGive(i2sMutex);
        return;
    }

    if (SD_MMC.cardType() == CARD_NONE) {
        Serial.println("No SD card attached");
        SD_MMC.end();
        initI2S();
        xSemaphoreGive(i2sMutex);
        return;
    }

    // 4. Захватываем мьютекс камеры
    if (xSemaphoreTake(cameraMutex, portMAX_DELAY) != pdTRUE) {
        SD_MMC.end();
        initI2S();
        xSemaphoreGive(i2sMutex);
        return;
    }

    // 5. Делаем паузу для стабилизации камеры после переключения пинов
    delay(50);

    // 6. Пытаемся получить кадр (с повторными попытками)
    camera_fb_t *fb = NULL;
    for (int retry = 0; retry < 3; retry++) {
        fb = esp_camera_fb_get();
        if (fb) break;
        delay(10);
    }

    if (!fb) {
        Serial.println("Camera capture failed after retries");
        xSemaphoreGive(cameraMutex);
        SD_MMC.end();
        initI2S();
        xSemaphoreGive(i2sMutex);
        return;
    }

    // 7. Генерируем имя файла (с временем или millis)
    char filename[32];
    time_t now = time(nullptr);
    if (now < 8 * 3600 * 2) { // время не синхронизировано
        sprintf(filename, "/photo_%lu.jpg", millis());
    } else {
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        strftime(filename, sizeof(filename), "/photo_%Y%m%d_%H%M%S.jpg", &timeinfo);
    }

    // 8. Записываем на SD
    File file = SD_MMC.open(filename, FILE_WRITE);
    if (!file) {
        Serial.println("Failed to create file");
    } else {
        file.write(fb->buf, fb->len);
        file.close();
        Serial.printf("Saved photo: %s\n", filename);
    }

    // 9. Возвращаем буфер камеры и отпускаем мьютекс камеры
    esp_camera_fb_return(fb);
    xSemaphoreGive(cameraMutex);

    // 10. Завершаем работу с SD
    SD_MMC.end();

    // 11. Переинициализируем I2S
    if (!initI2S()) {
        Serial.println("I2S reinit failed");
        // В этом случае звук не будет работать, но можно попытаться ещё раз позже
        // Возможно, стоит перезагрузить систему? Пока оставляем как есть.
    }

    // 12. Отпускаем мьютекс I2S – звук снова доступен
    xSemaphoreGive(i2sMutex);
}

bool detectSound() {
    if (xSemaphoreTake(i2sMutex, portMAX_DELAY) != pdTRUE) {
        return false; // не удалось захватить мьютекс (маловероятно)
    }

    uint8_t buffer[512];
    size_t bytesRead;
    esp_err_t res = i2s_read(I2S_PORT, buffer, sizeof(buffer), &bytesRead, 20);

    xSemaphoreGive(i2sMutex);

    if (res != ESP_OK) return false;

    int16_t *samples = (int16_t*)buffer;
    int count = bytesRead / 2;
    long sum = 0;
    for (int i = 0; i < count; i++)
        sum += abs(samples[i]);

    int rms = sum / count;
    return (rms > SOUND_THRESHOLD);
}


void sendPhoto() {
    if (xSemaphoreTake(cameraMutex, portMAX_DELAY) == pdTRUE) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            ws.sendBIN(fb->buf, fb->len);
            esp_camera_fb_return(fb);
        } else {
            Serial.println("sendPhoto: camera capture failed");
        }
        xSemaphoreGive(cameraMutex);
    }
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
  cameraMutex = xSemaphoreCreateMutex();
  i2sMutex = xSemaphoreCreateMutex();

  Wire.begin(26,27);
  initI2S();
soundEventQueue = xQueueCreate(5, sizeof(int)); 
xTaskCreatePinnedToCore(
    soundTask,      
    "SoundTask",    
    4096,          
    NULL,   
    1,             
    &soundTaskHandle,
    0          
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

    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    Serial.println("Waiting for NTP time sync...");
    time_t now = time(nullptr);
    int timeout = 0;
    while (now < 8 * 3600 * 2 && timeout < 20) { // ждём до 10 секунд
        delay(500);
        now = time(nullptr);
        timeout++;
    }
    if (now >= 8 * 3600 * 2)
        Serial.println("Time synchronized");
    else
        Serial.println("Time sync failed, using millis() for filenames");

  ws.begin(ws_host,ws_port,ws_path);
  ws.onEvent(webSocketEvent);
  ws.setReconnectInterval(2000);
  buttonTicker.attach_ms(20, onButtonTimer);

  

}


void loop() {


  ws.loop();


  unsigned long now = millis();

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
    if (now - lastSDPhoto >= SD_PHOTO_INTERVAL) {
        lastSDPhoto = now;
        savePhotoToSD();
    }

  if(now-lastSensorUpdate>SENSOR_INTERVAL) {

    lastSensorUpdate=now;

    if(readAHT10(ahtTemp,ahtHum)) {
      char msg[64];
      sprintf(msg,"temp:%.1f hum:%.1f",ahtTemp,ahtHum    );
      ws.sendTXT(msg);
    }
  }
    if (streaming && (now - lastFrame > FRAME_INTERVAL)) {
        lastFrame = now;
        sendPhoto();
    }
}
