#include <WiFi.h>
#include <esp_now.h>

#define MQ2 34
#define BUZZER 25

// Node 1 MAC
uint8_t peer1[] = {0x1C, 0xC3, 0xAB, 0xB4, 0x6A, 0xAC};

// Node 3 MAC
uint8_t peer2[] = {0xA4, 0xF0, 0x0F, 0x5C, 0x31, 0xC0};

int threshold = 2700;
bool fireState = false;

// 🔁 RECEIVE FUNCTION
void onReceive(const esp_now_recv_info *info, const uint8_t *data, int len) {
  String msg = "";

  for (int i = 0; i < len; i++) {
    msg += (char)data[i];
  }

  Serial.println("Received: " + msg);

  if (msg == "FIRE") {
    digitalWrite(BUZZER, HIGH);
  } 
  else if (msg == "SAFE") {
    digitalWrite(BUZZER, LOW);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(1000);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW Init Failed");
    return;
  }

  esp_now_register_recv_cb(onReceive);

  esp_now_peer_info_t peer = {};
  peer.channel = 0;
  peer.encrypt = false;

  // Add Node 1
  memcpy(peer.peer_addr, peer1, 6);
  esp_now_add_peer(&peer);

  // Add Node 3
  memcpy(peer.peer_addr, peer2, 6);
  esp_now_add_peer(&peer);
}

void loop() {
  int value = analogRead(MQ2);

  Serial.print("Node2 Gas: ");
  Serial.println(value);

  // 🔥 SEND ONLY WHEN STATE CHANGES
  if (value > threshold && fireState == false) {
    char msg[] = "FIRE";

    esp_now_send(peer1, (uint8_t *)msg, sizeof(msg));
    esp_now_send(peer2, (uint8_t *)msg, sizeof(msg));

    digitalWrite(BUZZER, HIGH);
    fireState = true;

    Serial.println("🔥 NODE2 FIRE SENT");
  }

  else if (value <= threshold && fireState == true) {
    char msg[] = "SAFE";

    esp_now_send(peer1, (uint8_t *)msg, sizeof(msg));
    esp_now_send(peer2, (uint8_t *)msg, sizeof(msg));

    digitalWrite(BUZZER, LOW);
    fireState = false;

    Serial.println("✅ NODE2 SAFE SENT");
  }

  delay(300);
}