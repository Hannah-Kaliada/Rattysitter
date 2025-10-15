#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiUDP.h>
#include <driver/i2s.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <RTClib.h>
#include <Adafruit_AHTX0.h>
#include <BH1750.h>
#include <time.h>

#define SDA_PIN 33
#define SCL_PIN 32
#define OLED_ADDR 0x3C
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
RTC_DS3231 rtc;
Adafruit_AHTX0 aht;
BH1750 lightMeter;

SemaphoreHandle_t i2cMutex;

#define I2S_SD   21
#define I2S_WS   22
#define I2S_SCK  26
#define I2S_PORT I2S_NUM_0

#define LED_R_PIN 4
#define LED_G_PIN 16
#define LED_B_PIN 2

WiFiMulti wifiMulti;
WiFiUDP udpOut, udpIn;
const char* udpServerIP = "172.20.10.3";
const uint16_t udpPortOut = 12345;
const uint16_t udpPortIn  = 12346;

int16_t sBuffer[1024];
char udpBuffer[255];

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3 * 3600;
const int daylightOffset_sec = 0;

void micTask(void* parameter) {
  size_t bytesIn = 0;

  i2s_config_t cfg = {
    .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = 0,
    .dma_buf_count = 8,
    .dma_buf_len = 1024,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };
  i2s_driver_install(I2S_PORT, &cfg, 0, nullptr);

  i2s_pin_config_t pin_cfg = {
    .bck_io_num   = I2S_SCK,
    .ws_io_num    = I2S_WS,
    .data_out_num = -1,
    .data_in_num  = I2S_SD
  };
  i2s_set_pin(I2S_PORT, &pin_cfg);
  i2s_start(I2S_PORT);

  udpOut.begin(WiFi.localIP(), udpPortOut);

  while (true) {
    if (i2s_read(I2S_PORT, sBuffer, sizeof(sBuffer), &bytesIn, portMAX_DELAY) == ESP_OK && bytesIn > 0) {
      udpOut.beginPacket(udpServerIP, udpPortOut);
      udpOut.write((uint8_t*)sBuffer, bytesIn);
      udpOut.endPacket();
    }
    delay(10);
  }
}

void udpCommandTask(void* parameter) {
  udpIn.begin(udpPortIn);
  Serial.printf("[UDP] Listening on port %d\n", udpPortIn);

  while (true) {
    int len = udpIn.parsePacket();
    if (len > 0) {
      int n = udpIn.read(udpBuffer, sizeof(udpBuffer) - 1);
      if (n > 0) {
        udpBuffer[n] = '\0';
        Serial.printf("[UDP] Received: %s\n", udpBuffer);

        int r = 0, g = 0, b = 0;
        if (sscanf(udpBuffer, "R:%d;G:%d;B:%d", &r, &g, &b) == 3) {
          r = constrain(r, 0, 255);
          g = constrain(g, 0, 255);
          b = constrain(b, 0, 255);
          analogWrite(LED_R_PIN, r);
          analogWrite(LED_G_PIN, g);
          analogWrite(LED_B_PIN, b);
          Serial.printf("[LED] Set color R:%d G:%d B:%d\n", r, g, b);
        }
      }
    }
    delay(10);
  }
}

unsigned long lastUpdate = 0;
void updateDisplay() {
  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(100))) {
    sensors_event_t humidity, temp;
    aht.getEvent(&humidity, &temp);

    float lux = lightMeter.readLightLevel();

    time_t nowSecs = time(nullptr);
    struct tm* timeinfo = localtime(&nowSecs);

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    display.setCursor(0, 0);
    display.printf("Time: %02d:%02d:%02d\n", timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
    display.printf("Date: %02d/%02d/%04d\n", timeinfo->tm_mday, timeinfo->tm_mon + 1, timeinfo->tm_year + 1900);
    display.printf("Temp: %.1f C\n", temp.temperature);
    display.printf("Hum:  %.1f %%\n", humidity.relative_humidity);
    display.printf("Light: %.1f lx", lux);

    display.display();
    xSemaphoreGive(i2cMutex);
  }
}

void sensorSendTask(void* parameter) {
  WiFiUDP udpSensor;
  udpSensor.begin(WiFi.localIP(), 12347);

  char sensorPayload[128];

  while (true) {
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(100))) {
      sensors_event_t humidity, temp;
      aht.getEvent(&humidity, &temp);
      float lux = lightMeter.readLightLevel();
      xSemaphoreGive(i2cMutex);

      snprintf(sensorPayload, sizeof(sensorPayload),
               "TEMP:%.1f;HUM:%.1f;LUX:%.1f",
               temp.temperature,
               humidity.relative_humidity,
               lux);

      udpSensor.beginPacket(udpServerIP, 12347);
      udpSensor.write((uint8_t*)sensorPayload, strlen(sensorPayload));
      udpSensor.endPacket();

      Serial.printf("[SENSOR] Sent: %s\n", sensorPayload);
    }

    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin(SDA_PIN, SCL_PIN);
  i2cMutex = xSemaphoreCreateMutex();

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED not found");
    while (true) delay(10);
  }

  if (!rtc.begin()) {
    Serial.println("RTC not found");
    while (true) delay(10);
  }

  if (!aht.begin()) {
    Serial.println("AHT10 not found");
    while (true) delay(10);
  }

  if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    Serial.println("BH1750 not found");
    while (true) delay(10);
  }

  delay(200);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Init OK. Connecting WiFi...");
  display.display();

  pinMode(LED_R_PIN, OUTPUT);
  pinMode(LED_G_PIN, OUTPUT);
  pinMode(LED_B_PIN, OUTPUT);

  wifiMulti.addAP("Cock", "xxxxxxxx");
  Serial.println("Connecting to WiFi...");
  while (wifiMulti.run() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\n[WiFi] Connected:");
  Serial.println(WiFi.localIP());

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("WiFi OK");
  display.display();

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("Waiting for NTP time sync...");
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("\nTime synchronized");

  xTaskCreatePinnedToCore(micTask, "micTask", 8192, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(udpCommandTask, "udpCommandTask", 4096, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(sensorSendTask, "sensorSendTask", 4096, nullptr, 1, nullptr, 1);
}

void loop() {
  if (millis() - lastUpdate > 500) {
    updateDisplay();
    lastUpdate = millis();
  }
}