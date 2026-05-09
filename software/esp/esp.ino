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

unsigned long lastI2SInit =0;
volatile bool fl = 0;
xSemaphoreHandle cameraMutex;          
xSemaphoreHandle i2sMutex;            
unsigned long lastSDPhoto = 0;        
const unsigned long SD_PHOTO_INTERVAL = 30000;
bool audioStreaming = false;

#define AUDIO_SAMPLES 2048                 
#define AUDIO_BYTES (AUDIO_SAMPLES * 2)   
uint8_t audioBuf[AUDIO_BYTES];

Adafruit_PCF8574 pcf;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Adafruit_PWMServoDriver pca(0x40);

QueueHandle_t soundEventQueue = nullptr;
TaskHandle_t soundTaskHandle = nullptr;

// NEW: очередь для передачи аудиоданных из звуковой задачи в главный цикл
QueueHandle_t audioQueue = nullptr;
typedef struct {
    uint8_t* data;
    size_t len;
} audio_packet_t;

// NEW: рекурсивный мьютекс для WebSocket
SemaphoreHandle_t wsMutex = nullptr;

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

const char* ws_host = "10.14.63.96";
const uint16_t ws_port = 8080;
const char* ws_path = "/ws";

#define PWDN_GPIO_NUM -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 15
#define SIOD_GPIO_NUM 4
#define SIOC_GPIO_NUM 5

#define Y2_GPIO_NUM 11
#define Y3_GPIO_NUM 9
#define Y4_GPIO_NUM 8
#define Y5_GPIO_NUM 10
#define Y6_GPIO_NUM 12
#define Y7_GPIO_NUM 18
#define Y8_GPIO_NUM 17
#define Y9_GPIO_NUM 16

#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM 7
#define PCLK_GPIO_NUM 13

#define I2S_PORT I2S_NUM_1
#define SAMPLE_RATE 16000
#define MIC_WS   14
#define MIC_DATA 42
#define MIC_BCLK 41

#define SOUND_THRESHOLD 1000
#define REFRACTORY_MS 1000

unsigned long lastTrigger = 0;
unsigned long lastSensorUpdate = 0;

#define SENSOR_INTERVAL 60000

float ahtTemp = 0;
float ahtHum = 0;

// Прототипы функций
bool initCamera();
void setRGB(uint16_t r, uint16_t g, uint16_t b);
bool initI2S();
void savePhotoToSD();
bool detectSound();
void sendPhoto();
void sendBinaryPacket(uint16_t type, const uint8_t* data, uint16_t len);
void sendAudio();

void soundTask(void *pvParameters) {
    while (1) {
        if (millis() - lastI2SInit > 1000) {
            sendAudio();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
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
        .dma_buf_count = 8,
        .dma_buf_len = 512
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
    delay(50);
    lastI2SInit = millis();
    return true;
}

void savePhotoToSD() {
    xSemaphoreTake(i2sMutex, portMAX_DELAY);
    esp_err_t err = i2s_driver_uninstall(I2S_PORT);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.println("I2S uninstall error");
    }
    delay(10);
    fl = 1;
    SD_MMC.setPins(39, 38, 40, -1, -1, -1);
    if (!SD_MMC.begin("/sdcard", true)) {
        Serial.println("SD Card Mount Failed");
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
    if (xSemaphoreTake(cameraMutex, portMAX_DELAY) != pdTRUE) {
        SD_MMC.end();
        initI2S();
        xSemaphoreGive(i2sMutex);
        return;
    }

    delay(50);

    camera_fb_t *fb = NULL;
    fb = esp_camera_fb_get();

    if (!fb) {
        Serial.println("Camera capture failed after retries");
        xSemaphoreGive(cameraMutex);
        SD_MMC.end();
        initI2S();
        xSemaphoreGive(i2sMutex);
        fl = 0;
        return;
    }

    char filename[32];
    time_t now = time(nullptr);
    if (now < 8 * 3600 * 2) { 
        sprintf(filename, "/photo_%lu.jpg", millis());
    } else {
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        strftime(filename, sizeof(filename), "/photo_%Y%m%d_%H%M%S.jpg", &timeinfo);
    }

    File file = SD_MMC.open(filename, FILE_WRITE);
    if (!file) {
        Serial.println("Failed to create file");
    } else {
        file.write(fb->buf, fb->len);
        file.close();
        Serial.printf("Saved photo: %s\n", filename);
    }
    esp_camera_fb_return(fb);
    xSemaphoreGive(cameraMutex);

    SD_MMC.end();

    if (!initI2S()) {
        Serial.println("I2S reinit failed");
    }
    xSemaphoreGive(i2sMutex);
    fl = 0;
}

bool detectSound() {
    if (xSemaphoreTake(i2sMutex, portMAX_DELAY) != pdTRUE) {
        return false; 
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

// NEW: отправка бинарного пакета с защитой мьютексом
void sendBinaryPacket(uint16_t type, const uint8_t* data, uint16_t len){
    uint8_t* packet = (uint8_t*)malloc(4 + len);
    if(!packet) return;

    packet[0] = type & 0xFF;
    packet[1] = type >> 8;
    packet[2] = len & 0xFF;
    packet[3] = len >> 8;

    memcpy(packet + 4, data, len);
    
    // Защита WebSocket
    xSemaphoreTakeRecursive(wsMutex, portMAX_DELAY);
    ws.sendBIN(packet, 4 + len);
    xSemaphoreGiveRecursive(wsMutex);
    
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
        uint8_t* packetData = (uint8_t*)malloc(bytesRead);
        if (packetData) {
            memcpy(packetData, audioBuf, bytesRead);
            audio_packet_t packet = { packetData, bytesRead };
            // Если очередь полна, освобождаем буфер
            if (xQueueSend(audioQueue, &packet, 0) != pdTRUE) {
                free(packetData);
            }
        }
    }
    xSemaphoreGive(i2sMutex);
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
            // Защита WebSocket
            xSemaphoreTakeRecursive(wsMutex, portMAX_DELAY);
            ws.sendTXT(msg);
            xSemaphoreGiveRecursive(wsMutex);
        }
        else if(cmd.startsWith("rgb")) {
            int r,g,b;
            sscanf(cmd.c_str(),"rgb %d %d %d",&r,&g,&b);
            setRGB(r,g,b);
        }
        else if(cmd == "reboot") {
            ESP.restart();
        }
        else if(cmd == "audio_on") audioStreaming = true;
        else if(cmd == "audio_off") audioStreaming = false;
    }
}

void setup() {
    Serial.begin(115200);

    WiFi.begin(ssid,password);
    WiFi.setSleep(false);
    while(WiFi.status()!=WL_CONNECTED)
        delay(500);

    initCamera();
    cameraMutex = xSemaphoreCreateMutex();
    i2sMutex = xSemaphoreCreateMutex();

    Wire.begin(21,47);
    initI2S();

    soundEventQueue = xQueueCreate(5, sizeof(int)); 
    // NEW: создаём очередь для аудиопакетов (увеличена до 20)
    audioQueue = xQueueCreate(20, sizeof(audio_packet_t));
    if (audioQueue == NULL) {
        Serial.println("Failed to create audio queue");
    }
    
    // NEW: создаём рекурсивный мьютекс для WebSocket
    wsMutex = xSemaphoreCreateRecursiveMutex();
    if (wsMutex == NULL) {
        Serial.println("Failed to create wsMutex");
    }

    xTaskCreatePinnedToCore(
        soundTask,      
        "SoundTask",    
        8192,           
        NULL,   
        1,             
        &soundTaskHandle,
        0          
    );

    Wire.begin(21,47);
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
    while (now < 8 * 3600 * 2 && timeout < 20) { 
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
    // Защищённый вызов ws.loop
    xSemaphoreTakeRecursive(wsMutex, portMAX_DELAY);
    ws.loop();
    xSemaphoreGiveRecursive(wsMutex);
    
    // Обработка очереди аудио (без мьютекса, т.к. отправка внутри sendBinaryPacket защищена)
    audio_packet_t packet;
    while (xQueueReceive(audioQueue, &packet, 0) == pdTRUE) {
        sendBinaryPacket(2, packet.data, packet.len);
        free(packet.data);
    }

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
            // Защищённая отправка
            xSemaphoreTakeRecursive(wsMutex, portMAX_DELAY);
            ws.sendTXT("sound");
            xSemaphoreGiveRecursive(wsMutex);
        }
    }

    if ((now - lastSDPhoto >= SD_PHOTO_INTERVAL)&&(!streaming)) {
        lastSDPhoto = now;
        savePhotoToSD();
    }

    if(now-lastSensorUpdate>SENSOR_INTERVAL) {
        lastSensorUpdate=now;
        if(readAHT10(ahtTemp,ahtHum)) {
            char msg[64];
            sprintf(msg,"temp:%.1f hum:%.1f",ahtTemp,ahtHum);  
            xSemaphoreTakeRecursive(wsMutex, portMAX_DELAY);
            ws.sendTXT(msg);
            xSemaphoreGiveRecursive(wsMutex);
        } 
    }

    if (streaming && (now - lastFrame > FRAME_INTERVAL)) {
        lastFrame = now;
        sendPhoto();
    }
}
