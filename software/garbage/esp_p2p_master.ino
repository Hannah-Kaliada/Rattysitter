#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WebServer.h>

const char* ssid = "Clown";
const char* password = "12345678";

// MAC адрес ESP8266
uint8_t peerAddress[] = {0xD8, 0xBF, 0xC0, 0xCA, 0xCB, 0x1A};
const uint8_t peerChannel = 11;  // канал ESP8266

WebServer server(80);
String logBuffer;
unsigned long lastSend = 0;
uint8_t wifiChannel = 11;  // канал точки доступа

void getCurrentWiFiChannel() {
  wifi_second_chan_t second;
  esp_wifi_get_channel(&wifiChannel, &second);
  Serial.printf("📡 Current Wi-Fi channel: %d\n", wifiChannel);
}

void onReceive(const esp_now_recv_info *info, const uint8_t *data, int len) {
  String msg = String((char*)data);
  Serial.printf("📨 From %02X:%02X:%02X:%02X:%02X:%02X: %s\n",
                info->src_addr[0], info->src_addr[1], info->src_addr[2],
                info->src_addr[3], info->src_addr[4], info->src_addr[5],
                msg.c_str());
  logBuffer += "RX: " + msg + "<br>";
}

void onSent(const uint8_t *mac, esp_now_send_status_t status) {
  String s = status == ESP_NOW_SEND_SUCCESS ? "✅ OK" : "❌ FAIL";
  Serial.println("[ESP-NOW] " + s);
  logBuffer += "[ESP-NOW] " + s + "<br>";
}

String htmlPage() {
  return R"rawliteral(
  <html><head><meta charset='utf-8'>
  <style>
  body{background:#111;color:#0f0;font-family:monospace;}
  #log{border:1px solid #0f0;padding:10px;height:60vh;overflow-y:auto;}
  </style></head>
  <body>
  <h2>ESP32 Bridge Chat</h2>
  <div id='log'></div><br>
  <input id='msg'><button onclick='send()'>Send</button>
  <script>
  async function load(){document.getElementById('log').innerHTML=await (await fetch('/log')).text();}
  async function send(){let m=document.getElementById('msg').value;await fetch('/send?m='+m);document.getElementById('msg').value='';setTimeout(load,500);}
  setInterval(load,2000);load();
  </script></body></html>)rawliteral";
}

void handleRoot(){ server.send(200, "text/html", htmlPage()); }
void handleLog(){ server.send(200, "text/html", logBuffer); }

void handleSend(){
  String msg = server.arg("m");
  if (!msg.length()) return;
  
  // --- Переключаем канал на ESP8266 ---
  esp_wifi_set_channel(peerChannel, WIFI_SECOND_CHAN_NONE);
  esp_now_send(peerAddress, (uint8_t*)msg.c_str(), msg.length() + 1);
  delay(50); // ждём передачи
  esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);  // назад
  
  logBuffer += "[WEB->SEND] " + msg + "<br>";
  Serial.println("[WEB->SEND] " + msg);
  server.send(200, "text/plain", "OK");
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.println("\n✅ Connected!");
  Serial.print("IP: "); Serial.println(WiFi.localIP());
  Serial.print("MAC: "); Serial.println(WiFi.macAddress());
  getCurrentWiFiChannel();

  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ ESP-NOW init failed!");
    return;
  }

  esp_now_register_recv_cb(onReceive);
  esp_now_register_send_cb(onSent);

  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, peerAddress, 6);
  peer.channel = peerChannel;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
  Serial.println("✅ ESP-NOW ready");

  server.on("/", handleRoot);
  server.on("/log", handleLog);
  server.on("/send", handleSend);
  server.begin();
  Serial.println("🌐 Web server started!");
}

void loop() {
  server.handleClient();

  if (millis() - lastSend > 5000) {
    lastSend = millis();
    String msg = "Ping from ESP32";
    esp_wifi_set_channel(peerChannel, WIFI_SECOND_CHAN_NONE);
    esp_now_send(peerAddress, (uint8_t*)msg.c_str(), msg.length() + 1);
    delay(50);
    esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);
  }
}
