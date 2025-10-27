#include <ESP8266WiFi.h>
#include <espnow.h>

uint8_t peerAddress[] = {0x3C, 0x8A, 0x1F, 0xAF, 0x84, 0xB8};

void onReceive(uint8_t *mac, uint8_t *data, uint8_t len) {
  Serial.print("📨 From ESP32: ");
  Serial.write(data, len);
  Serial.println();
}

void onSent(uint8_t *mac, uint8_t sendStatus) {
  Serial.print("Send status: ");
  Serial.println(sendStatus == 0 ? "✅ OK" : "❌ FAIL");
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  wifi_set_channel(11); // наш зафиксированный канал
  
  if (esp_now_init() != 0) {
    Serial.println("❌ ESP-NOW init failed!");
    return;
  }

  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onReceive);
  esp_now_register_send_cb(onSent);
  esp_now_add_peer(peerAddress, ESP_NOW_ROLE_COMBO, 11, NULL, 0);
  
  Serial.println("✅ ESP8266 ready");
}

void loop() {
  static unsigned long t = 0;
  if (millis() - t > 4000) {
    t = millis();
    String msg = "Hi from ESP8266!";
    esp_now_send(peerAddress, (uint8_t*)msg.c_str(), msg.length() + 1);
  }
}
