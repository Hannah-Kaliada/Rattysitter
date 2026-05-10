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
#include <GyverBME280.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#define PCF8574_ADDRESS 0x20
#define OLED_ADDR 0x3C
#define AHT10_ADDR 0x38
#define BME280_ADDR 0x76
#define PCA9685_ADDR 0x40

#define TOUCH_PIN 0
#define LED_R_CH 0
#define LED_G_CH 1
#define LED_B_CH 2

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
#define MIC_WS   14
#define MIC_DATA 42
#define MIC_BCLK 41

#define SAMPLE_RATE 16000
#define SOUND_THRESHOLD 1000
#define REFRACTORY_MS 1000

#define DEBOUNCE_DELAY 50
#define BUTTON_UPDATE_INTERVAL 10
#define SD_PHOTO_INTERVAL 30000
#define FRAME_INTERVAL 10

#define SENSOR_INTERVAL 60000
#define DISPLAY_UPDATE_INTERVAL 1000
#define TELEGRAM_POLL_INTERVAL 2000

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

#define AUDIO_SAMPLES 2048                 
#define AUDIO_BYTES (AUDIO_SAMPLES * 2)   

Adafruit_PCF8574 pcf;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Adafruit_PWMServoDriver pca(PCA9685_ADDR);
GyverBME280 bme;

uint8_t audioBuf[AUDIO_BYTES];

xSemaphoreHandle cameraMutex = nullptr;          
xSemaphoreHandle i2sMutex = nullptr;            
SemaphoreHandle_t wsMutex = nullptr;
QueueHandle_t soundEventQueue = nullptr;
QueueHandle_t audioQueue = nullptr;
TaskHandle_t soundTaskHandle = nullptr;

typedef struct {
    uint8_t* data;
    size_t len;
} audio_packet_t;

Ticker buttonTicker;
volatile bool checkButtonFlag = false;

bool streaming = false;
bool audioStreaming = false;
bool bmeInitialized = false;
bool ahtInitialized = false;

unsigned long lastI2SInit = 0;
unsigned long lastSDPhoto = 0;        
unsigned long lastFrame = 0;
unsigned long lastTrigger = 0;
unsigned long lastSensorUpdate = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastTelegramAlert = 0;
unsigned long lastTelegramCheck = 0;

volatile bool fl = 0;

float currentTemp = 0;
float currentHum = 0;
float currentPressure = 0;

WebSocketsClient ws;

const char* ssid = "Clown";
const char* password = "12345678";

const char* ws_host = "10.122.132.96";
const uint16_t ws_port = 8080;
const char* ws_path = "/ws";

const char* botToken = "7644572719:AAHr0MIQVg7U5_2dibiUGIIfrVqIulMdI9c";
const char* chatID = "949226271";

float tempMin = 18.0;
float tempMax = 26.0;
float humMin = 40.0;
float humMax = 60.0;
float pressureMin = 745.0;
float pressureMax = 765.0;

long lastMessageId = 0;

bool initCamera();
void setRGB(uint16_t r, uint16_t g, uint16_t b);
bool initI2S();
void savePhotoToSD();
bool detectSound();
void sendPhoto();
void sendPhotoToTelegram(String chatId);
void sendBinaryPacket(uint16_t type, const uint8_t* data, uint16_t len);
void sendAudio();
void updateDisplay();
bool initBME280();
bool initAHT10();
bool readAHT10(float &temperature, float &humidity);
void readSensors(float &temperature, float &humidity, float &pressure);
const char* getDayOfWeek(int wday);
void sendTelegramMessage(String chatId, String message);
void checkAndSendAlert(float temp, float hum, float pres);
String getAlertMessage(float temp, float hum, float pres);
void checkTelegramMessages();

void IRAM_ATTR onButtonTimer() {
    checkButtonFlag = true;
}

const char* getDayOfWeek(int wday) {
    switch(wday) {
        case 0: return "Sun";
        case 1: return "Mon";
        case 2: return "Tue";
        case 3: return "Wed";
        case 4: return "Thu";
        case 5: return "Fri";
        case 6: return "Sat";
        default: return "???";
    }
}

String urlencode(String str) {
    String encodedString = "";
    char c;
    char code0;
    char code1;
    for (int i = 0; i < str.length(); i++) {
        c = str.charAt(i);
        if (c == ' ') {
            encodedString += "%20";
        } else if (isalnum(c)) {
            encodedString += c;
        } else {
            code1 = (c & 0xf) + '0';
            if ((c & 0xf) > 9) {
                code1 = (c & 0xf) - 10 + 'A';
            }
            c = (c >> 4) & 0xf;
            code0 = c + '0';
            if (c > 9) {
                code0 = c - 10 + 'A';
            }
            encodedString += '%';
            encodedString += code0;
            encodedString += code1;
        }
    }
    return encodedString;
}

void sendTelegramMessage(String chatId, String message) {
    HTTPClient http;
    
    String encodedMessage = urlencode(message);
    String url = "https://api.telegram.org/bot" + String(botToken) + 
                 "/sendMessage?chat_id=" + chatId + "&text=" + encodedMessage;
    
    http.begin(url);
    http.setTimeout(5000);
    
    int httpCode = http.GET();
    
    if (httpCode > 0) {
        if (httpCode == HTTP_CODE_OK) {
            Serial.println("Telegram message sent");
        } else {
            Serial.printf("HTTP error: %d\n", httpCode);
        }
    } else {
        Serial.printf("Connection error: %s\n", http.errorToString(httpCode).c_str());
    }
    
    http.end();
}

void sendPhotoToTelegram(String chatId) {
    if (xSemaphoreTake(cameraMutex, portMAX_DELAY) != pdTRUE) return;
    
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("Camera capture failed");
        xSemaphoreGive(cameraMutex);
        sendTelegramMessage(chatId, "Failed to capture photo");
        return;
    }
    
    HTTPClient http;
    String url = "https://api.telegram.org/bot" + String(botToken) + "/sendPhoto";
    
    http.begin(url);
    http.addHeader("Content-Type", "multipart/form-data; boundary=----WebKitFormBoundary7MA4YWxkTrZu0gW");
    
    String boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
    
    String beginBoundary = "--" + boundary + "\r\n";
    beginBoundary += "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n";
    beginBoundary += chatId + "\r\n";
    
    beginBoundary += "--" + boundary + "\r\n";
    beginBoundary += "Content-Disposition: form-data; name=\"photo\"; filename=\"photo.jpg\"\r\n";
    beginBoundary += "Content-Type: image/jpeg\r\n\r\n";
    
    String endBoundary = "\r\n--" + boundary + "--\r\n";
    
    int bodyLength = beginBoundary.length() + fb->len + endBoundary.length();
    
    uint8_t* postData = (uint8_t*)malloc(bodyLength);
    if (!postData) {
        Serial.println("Failed to allocate memory");
        esp_camera_fb_return(fb);
        xSemaphoreGive(cameraMutex);
        return;
    }
    
    size_t idx = 0;
    memcpy(postData + idx, beginBoundary.c_str(), beginBoundary.length());
    idx += beginBoundary.length();
    
    memcpy(postData + idx, fb->buf, fb->len);
    idx += fb->len;
    
    memcpy(postData + idx, endBoundary.c_str(), endBoundary.length());
    idx += endBoundary.length();
    
    int httpCode = http.POST(postData, idx);
    
    if (httpCode == HTTP_CODE_OK) {
        Serial.println("Photo sent to Telegram successfully!");
    } else {
        Serial.printf("Failed to send photo, HTTP code: %d\n", httpCode);
    }
    
    free(postData);
    esp_camera_fb_return(fb);
    http.end();
    xSemaphoreGive(cameraMutex);
}

String getAlertMessage(float temp, float hum, float pres) {
    String message = "System Alert\n\n";
    
    bool alert = false;
    
    if (temp < tempMin) {
        message += "Low temperature!\n";
        message += "   Current: " + String(temp, 1) + "C\n";
        message += "   Normal: " + String(tempMin, 1) + "-" + String(tempMax, 1) + "C\n\n";
        alert = true;
    } else if (temp > tempMax) {
        message += "High temperature!\n";
        message += "   Current: " + String(temp, 1) + "C\n";
        message += "   Normal: " + String(tempMin, 1) + "-" + String(tempMax, 1) + "C\n\n";
        alert = true;
    }
    
    if (hum < humMin) {
        message += "Low humidity!\n";
        message += "   Current: " + String(hum, 1) + "%\n";
        message += "   Normal: " + String(humMin, 1) + "-" + String(humMax, 1) + "%\n\n";
        alert = true;
    } else if (hum > humMax) {
        message += "High humidity!\n";
        message += "   Current: " + String(hum, 1) + "%\n";
        message += "   Normal: " + String(humMin, 1) + "-" + String(humMax, 1) + "%\n\n";
        alert = true;
    }
    
    if (pres < pressureMin) {
        message += "Low pressure!\n";
        message += "   Current: " + String(pres, 1) + " mmHg\n";
        message += "   Normal: " + String(pressureMin, 1) + "-" + String(pressureMax, 1) + " mmHg\n\n";
        alert = true;
    } else if (pres > pressureMax) {
        message += "High pressure!\n";
        message += "   Current: " + String(pres, 1) + " mmHg\n";
        message += "   Normal: " + String(pressureMin, 1) + "-" + String(pressureMax, 1) + " mmHg\n\n";
        alert = true;
    }
    
    if (!alert) {
        message = "";
    }
    
    return message;
}

void checkAndSendAlert(float temp, float hum, float pres) {
    unsigned long now = millis();
    
    if (now - lastTelegramAlert < 60000) return;
    
    String alertMsg = getAlertMessage(temp, hum, pres);
    if (alertMsg.length() > 0) {
        sendTelegramMessage(chatID, alertMsg);
        lastTelegramAlert = now;
        
        delay(1000);
        sendPhotoToTelegram(chatID);
    }
}

void checkTelegramMessages() {
    HTTPClient http;
    
    String url = "https://api.telegram.org/bot" + String(botToken) + "/getUpdates";
    if (lastMessageId > 0) {
        url += "?offset=" + String(lastMessageId + 1);
    }
    
    http.begin(url);
    http.setTimeout(5000);
    
    int httpCode = http.GET();
    
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        
        // Простой парсинг JSON без библиотеки
        int msgStart = payload.indexOf("\"message_id\":");
        while (msgStart > 0) {
            int msgIdStart = msgStart + 13;
            int msgIdEnd = payload.indexOf(",", msgIdStart);
            String msgIdStr = payload.substring(msgIdStart, msgIdEnd);
            long msgId = msgIdStr.toInt();
            
            if (msgId > lastMessageId) {
                lastMessageId = msgId;
                
                // Ищем chat_id
                int chatIdStart = payload.indexOf("\"chat\":{\"id\":", msgIdStart);
                if (chatIdStart > 0) {
                    int chatIdValStart = chatIdStart + 13;
                    int chatIdValEnd = payload.indexOf(",", chatIdValStart);
                    String chatIdStr = payload.substring(chatIdValStart, chatIdValEnd);
                    
                    // Ищем текст сообщения
                    int textStart = payload.indexOf("\"text\":\"", msgIdStart);
                    if (textStart > 0) {
                        int textValStart = textStart + 8;
                        int textValEnd = payload.indexOf("\"", textValStart);
                        String text = payload.substring(textValStart, textValEnd);
                        
                        handleTelegramCommand(chatIdStr, text);
                    }
                }
            }
            
            msgStart = payload.indexOf("\"message_id\":", msgIdEnd);
        }
    }
    
    http.end();
}

void handleTelegramCommand(String chatId, String text) {
    text.toLowerCase();
    text.trim();
    
    if (text == "/start" || text == "/help") {
        String helpMsg = "ESP32 Camera System\n\n";
        helpMsg += "Available commands:\n";
        helpMsg += "/photo - Take photo\n";
        helpMsg += "/sensors - Sensor data\n";
        helpMsg += "/status - System status\n";
        helpMsg += "/ip - Network info\n";
        helpMsg += "/help - This message\n";
        
        sendTelegramMessage(chatId, helpMsg);
    }
    else if (text == "/photo") {
        sendTelegramMessage(chatId, "Taking photo...");
        sendPhotoToTelegram(chatId);
    }
    else if (text == "/sensors") {
        String msg = "Sensor data:\n";
        readSensors(currentTemp, currentHum, currentPressure);
        msg += "Temperature: " + String(currentTemp, 1) + " C\n";
        msg += "Humidity: " + String(currentHum, 1) + " %\n";
        msg += "Pressure: " + String(currentPressure, 1) + " mmHg\n";
        
        if (currentTemp < tempMin || currentTemp > tempMax) {
            msg += "Warning: Temperature out of range\n";
        }
        if (currentHum < humMin || currentHum > humMax) {
            msg += "Warning: Humidity out of range\n";
        }
        if (currentPressure < pressureMin || currentPressure > pressureMax) {
            msg += "Warning: Pressure out of range\n";
        }
        
        sendTelegramMessage(chatId, msg);
    }
    else if (text == "/status") {
        String msg = "System Status:\n";
        msg += "Streaming: " + String(streaming ? "ON" : "OFF") + "\n";
        msg += "Audio: " + String(audioStreaming ? "ON" : "OFF") + "\n";
        msg += "Sensors: ";
        if (ahtInitialized && bmeInitialized) {
            msg += "OK\n";
        } else {
            msg += "Partial failure\n";
            if (!ahtInitialized) msg += "- AHT10 not found\n";
            if (!bmeInitialized) msg += "- BME280 not found\n";
        }
        
        sendTelegramMessage(chatId, msg);
    }
    else if (text == "/ip") {
        String msg = "IP Address: " + WiFi.localIP().toString() + "\n";
        msg += "MAC: " + WiFi.macAddress();
        sendTelegramMessage(chatId, msg);
    }
    else {
        sendTelegramMessage(chatId, "Unknown command. Use /help for list of commands.");
    }
}

bool initAHT10() {
    Wire.beginTransmission(AHT10_ADDR);
    Wire.write(0xE1);
    Wire.write(0x08);
    Wire.write(0x00);
    if (Wire.endTransmission() != 0) {
        Serial.println("AHT10 init failed");
        return false;
    }
    
    delay(500);
    
    Wire.beginTransmission(AHT10_ADDR);
    if (Wire.endTransmission() != 0) {
        Serial.println("AHT10 not responding after init");
        return false;
    }
    
    Serial.println("AHT10 initialized successfully");
    return true;
}

bool readAHT10(float &temperature, float &humidity) {
    Wire.beginTransmission(AHT10_ADDR);
    Wire.write(0xAC);
    Wire.write(0x33);
    Wire.write(0x00);
    if (Wire.endTransmission() != 0) return false;

    delay(80);

    Wire.requestFrom(AHT10_ADDR, 6);
    if (Wire.available() != 6) return false;

    uint8_t data[6];
    for (int i = 0; i < 6; i++) data[i] = Wire.read();

    if (data[0] & 0x80) {
        Serial.println("AHT10: Sensor busy");
        return false;
    }

    uint32_t rawHum = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (data[3] >> 4);
    uint32_t rawTemp = ((uint32_t)(data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | data[5];

    humidity = (rawHum * 100.0) / 1048576.0;
    temperature = ((rawTemp * 200.0) / 1048576.0) - 50;
    return true;
}

bool initBME280() {
    if (!bme.begin(BME280_ADDR)) {
        Serial.println("BME280/BMP280 not found");
        return false;
    }
    
    Serial.println("BME280/BMP280 initialized for pressure");
    return true;
}

void readSensors(float &temperature, float &humidity, float &pressure) {
    temperature = humidity = pressure = 0;
    
    if (ahtInitialized) {
        if (!readAHT10(temperature, humidity)) {
            Serial.println("Failed to read AHT10");
            temperature = humidity = 0;
        }
    }
    
    if (bmeInitialized) {
        float pressurePa = bme.readPressure();
        pressure = pressurePa / 133.322f;
    }
}

void updateDisplay() {
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    
    display.setTextSize(2);
    display.setCursor(0, 0);
    char timeStr[9];
    strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);
    display.print(timeStr);
    
    display.setTextSize(1);
    display.setCursor(80, 8);
    char secStr[3];
    strftime(secStr, sizeof(secStr), "%S", &timeinfo);
    display.print(secStr);
    
    display.setTextSize(1);
    display.setCursor(0, 18);
    char dateStr[20];
    strftime(dateStr, sizeof(dateStr), "%d.%m.%Y", &timeinfo);
    display.printf("%s %s", dateStr, getDayOfWeek(timeinfo.tm_wday));
    
    display.drawLine(0, 27, 128, 27, SSD1306_WHITE);
    
    if (ahtInitialized || bmeInitialized) {
        float temp, hum, pres;
        readSensors(temp, hum, pres);
        
        display.setCursor(0, 32);
        display.printf("T: %.1f", temp);
        display.print("C");
        
        display.setCursor(64, 32);
        display.printf("H: %.1f%%", hum);
        
        display.setCursor(0, 48);
        if (bmeInitialized) {
            display.printf("P: %.1f mmHg", pres);
        } else {
            display.print("P: --- mmHg");
        }
    } else {
        display.setCursor(10, 40);
        display.setTextSize(2);
        display.println("NO SENSORS");
    }
    
    display.display();
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
        Serial.println("Camera capture failed");
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

void sendBinaryPacket(uint16_t type, const uint8_t* data, uint16_t len){
    uint8_t* packet = (uint8_t*)malloc(4 + len);
    if(!packet) return;

    packet[0] = type & 0xFF;
    packet[1] = type >> 8;
    packet[2] = len & 0xFF;
    packet[3] = len >> 8;

    memcpy(packet + 4, data, len);
    
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
            sprintf(msg,"temp:%.1f hum:%.1f pres:%.1f",currentTemp,currentHum,currentPressure);
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
        else if(cmd == "telegram_photo") {
            sendPhotoToTelegram(chatID);
        }
        else if(cmd == "telegram_test") {
            sendTelegramMessage(chatID, "ESP32 connected! Test message.");
        }
    }
}

void soundTask(void *pvParameters) {
    while (1) {
        if (millis() - lastI2SInit > 1000) {
            sendAudio();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

void setup() {
    Serial.begin(115200);

    WiFi.begin(ssid,password);
    WiFi.setSleep(false);
    while(WiFi.status()!=WL_CONNECTED)
        delay(500);
    
    Serial.println("\nWiFi connected!");
    sendTelegramMessage(chatID, "ESP32 Camera System Started! Use /help for commands.");

    initCamera();
    cameraMutex = xSemaphoreCreateMutex();
    i2sMutex = xSemaphoreCreateMutex();

    Wire.begin(21,47);
    initI2S();

    soundEventQueue = xQueueCreate(5, sizeof(int)); 
    audioQueue = xQueueCreate(20, sizeof(audio_packet_t));
    if (audioQueue == NULL) {
        Serial.println("Failed to create audio queue");
    }
    
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
        Serial.println("Adafruit PCF8574 OK");
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
    display.setCursor(10,20);
    display.println("System OK");
    display.display();
    delay(1000);

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

    ahtInitialized = initAHT10();
    if (!ahtInitialized) {
        Serial.println("Warning: AHT10 not found");
    }
    
    bmeInitialized = initBME280();
    if (!bmeInitialized) {
        Serial.println("Warning: BME280/BMP280 not found");
    }

    ws.begin(ws_host,ws_port,ws_path);
    ws.onEvent(webSocketEvent);
    ws.setReconnectInterval(2000);
    buttonTicker.attach_ms(20, onButtonTimer);
    
    updateDisplay();
    
    Serial.println("Setup completed!");
}

void loop() {
    xSemaphoreTakeRecursive(wsMutex, portMAX_DELAY);
    ws.loop();
    xSemaphoreGiveRecursive(wsMutex);
    
    // Проверка сообщений Telegram каждые 2 секунды
    if (millis() - lastTelegramCheck > TELEGRAM_POLL_INTERVAL) {
        checkTelegramMessages();
        lastTelegramCheck = millis();
    }
    
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
            xSemaphoreTakeRecursive(wsMutex, portMAX_DELAY);
            ws.sendTXT("sound");
            xSemaphoreGiveRecursive(wsMutex);
        }
    }

    if ((now - lastSDPhoto >= SD_PHOTO_INTERVAL) && (!streaming)) {
        lastSDPhoto = now;
        savePhotoToSD();
    }

    if (now - lastDisplayUpdate > DISPLAY_UPDATE_INTERVAL) {
        lastDisplayUpdate = now;
        updateDisplay();
    }

    if (now - lastSensorUpdate > SENSOR_INTERVAL) {
        lastSensorUpdate = now;
        float temp, hum, pres;
        readSensors(temp, hum, pres);
        
        currentTemp = temp;
        currentHum = hum;
        currentPressure = pres;
        
        char msg[64];
        sprintf(msg,"temp:%.1f hum:%.1f pres:%.1f",currentTemp,currentHum,currentPressure);  
        xSemaphoreTakeRecursive(wsMutex, portMAX_DELAY);
        ws.sendTXT(msg);
        xSemaphoreGiveRecursive(wsMutex);
        
        checkAndSendAlert(temp, hum, pres);
    }

    if (streaming && (now - lastFrame > FRAME_INTERVAL)) {
        lastFrame = now;
        sendPhoto();
    }
}
