#include <ESP8266WiFi.h> // - esp8266
//#include <WiFi.h> - esp32

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  delay(100);
  Serial.print("ESP8266 MAC: ");
  Serial.println(WiFi.macAddress());
}

void loop() {}
