#include <ESP8266WiFi.h>
#include <espnow.h>

void onDataRecv(uint8_t *mac, uint8_t *data, uint8_t len) {
  Serial.print("Received from: ");
  for (int i = 0; i < 6; i++) {
    Serial.print(mac[i], HEX);
    if (i < 5) Serial.print(":");
  }
  Serial.print(" | Message: ");
  Serial.write(data, len);
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != 0) {
    Serial.println("ESP-NOW init failed!");
    return;
  }
  esp_now_set_self_role(ESP_NOW_ROLE_SLAVE);
  esp_now_register_recv_cb(onDataRecv);
  Serial.println("ESP8266 ready to receive!");
}

void loop() {}
