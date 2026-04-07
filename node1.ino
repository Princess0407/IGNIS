#include <WiFi.h>
#include <esp_now.h>

#define MQ2 34
#define BUZZER 25

// Node 2
uint8_t peer1[] = {0x00, 0x70, 0x07, 0xE1, 0xBA, 0x1C};
// Node 3
uint8_t peer2[] = {0xA4, 0xF0, 0x0F, 0x5C, 0x31, 0xC0};

int threshold = 1400;
bool fireState = false;

void onReceive(const esp_now_recv_info *info, const uint8_t *data, int len) {
  String msg = "";
  for (int i = 0; i < len; i++) msg += (char)data[i];

  if (msg == "FIRE") digitalWrite(BUZZER, HIGH);
  else if (msg == "SAFE") digitalWrite(BUZZER, LOW);
}

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(1000);

  esp_now_init();
  esp_now_register_recv_cb(onReceive);

  esp_now_peer_info_t peer = {};
  peer.channel = 0;
  peer.encrypt = false;

  memcpy(peer.peer_addr, peer1, 6);
  esp_now_add_peer(&peer);

  memcpy(peer.peer_addr, peer2, 6);
  esp_now_add_peer(&peer);
}

void loop() {
  int value = analogRead(MQ2);

  Serial.print("Node1 Gas: ");
  Serial.println(value);

  if (value > threshold && !fireState) {
    char msg[] = "FIRE";
    esp_now_send(peer1, (uint8_t *)msg, sizeof(msg));
    esp_now_send(peer2, (uint8_t *)msg, sizeof(msg));

    digitalWrite(BUZZER, HIGH);
    fireState = true;
  }

  else if (value <= threshold && fireState) {
    char msg[] = "SAFE";
    esp_now_send(peer1, (uint8_t *)msg, sizeof(msg));
    esp_now_send(peer2, (uint8_t *)msg, sizeof(msg));

    digitalWrite(BUZZER, LOW);
    fireState = false;
  }

  delay(300);
}