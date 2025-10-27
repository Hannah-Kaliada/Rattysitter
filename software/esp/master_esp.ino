#include <WiFi.h>
#include <esp_now.h>

uint8_t receiverAddress[] = {0xD8, 0xBF, 0xC0, 0xCA, 0xCB, 0x1A};

void onSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }
  esp_now_register_send_cb(onSent);

  esp_now_peer_info_t peerInfo{};
  memcpy(peerInfo.peer_addr, receiverAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer");
    return;
  }
}

void loop() {
  const char *msg = "Hello from ESP32-CAM!";
  esp_err_t result = esp_now_send(receiverAddress, (uint8_t *)msg, strlen(msg) + 1);
  if (result == ESP_OK) {
    Serial.println("Message sent!");
  } else {
    Serial.println("Error sending message");
  }
  delay(2000);
}
