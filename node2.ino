// ============================================================
//  NODE B — Fire Alarm Mesh Node (Relay)
//  Features: ESP-NOW Broadcast, Unique Message IDs,
//            Broadcast Storm Prevention (hopCount + seen IDs),
//            Dynamic Baseline Calibration, EMA Filter
// ============================================================

#include <WiFi.h>
#include <esp_now.h>

// ── Pin Definitions ──────────────────────────────────────────
#define MQ2    34
#define BUZZER 25

// ── ESP-NOW Broadcast MAC ────────────────────────────────────
uint8_t broadcastMAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ── Node Identity ────────────────────────────────────────────
#define NODE_ID "NODE_B"

// ── Message Struct ───────────────────────────────────────────
typedef struct __attribute__((packed)) FireMessage {
  char     nodeID[8];
  uint32_t messageID;
  uint8_t  hopCount;
  bool     isFire;
  uint16_t smokeLevel;
} FireMessage;

// ── Broadcast Storm Prevention ───────────────────────────────
#define SEEN_ID_BUFFER 10
uint32_t seenIDs[SEEN_ID_BUFFER];
uint8_t  seenIndex = 0;

bool alreadySeen(uint32_t id) {
  for (int i = 0; i < SEEN_ID_BUFFER; i++)
    if (seenIDs[i] == id) return true;
  return false;
}

void markSeen(uint32_t id) {
  seenIDs[seenIndex] = id;
  seenIndex = (seenIndex + 1) % SEEN_ID_BUFFER;
}

// ── State ────────────────────────────────────────────────────
bool  fireState   = false;
float emaValue    = 0.0;
float baseline    = 0.0;
const float EMA_ALPHA = 0.2;
const float FIRE_PCT  = 0.20;

uint32_t newMessageID() {
  return (uint32_t)esp_random();
}

// ── ESP-NOW Receive Callback ─────────────────────────────────
void onReceive(const esp_now_recv_info *info, const uint8_t *data, int len) {
  if (len != sizeof(FireMessage)) return;

  FireMessage msg;
  memcpy(&msg, data, sizeof(FireMessage));

  if (alreadySeen(msg.messageID)) return;
  markSeen(msg.messageID);

  Serial.printf("[NodeB] Relayed msg from %s | ID:%u | Fire:%d | Hops left:%d\n",
                msg.nodeID, msg.messageID, msg.isFire, msg.hopCount);

  if (msg.isFire) {
    digitalWrite(BUZZER, HIGH);
    fireState = true;
  } else {
    digitalWrite(BUZZER, LOW);
    fireState = false;
  }

  // Relay if hops remain
  if (msg.hopCount > 0) {
    msg.hopCount--;
    esp_now_send(broadcastMAC, (uint8_t *)&msg, sizeof(FireMessage));
  }
}

void onSent(const uint8_t *mac, esp_now_send_status_t status) {
  Serial.printf("[NodeB] Send status: %s\n",
                status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

// ── Dynamic Baseline Calibration ────────────────────────────
void calibrateBaseline() {
  Serial.println("[NodeB] Calibrating baseline (10s)...");
  long sum = 0;
  const int SAMPLES = 100;
  for (int i = 0; i < SAMPLES; i++) {
    sum += analogRead(MQ2);
    delay(100);
  }
  baseline = (float)sum / SAMPLES;
  emaValue = baseline;
  Serial.printf("[NodeB] Baseline set: %.1f\n", baseline);
}

// ── Setup ────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, LOW);

  randomSeed(analogRead(0));

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(300);

  calibrateBaseline();

  if (esp_now_init() != ESP_OK) {
    Serial.println("[NodeB] ESP-NOW init FAILED");
    return;
  }
  esp_now_register_recv_cb(onReceive);
  esp_now_register_send_cb(onSent);

  esp_now_peer_info_t peer = {};
  peer.channel = 0;
  peer.encrypt = false;
  memcpy(peer.peer_addr, broadcastMAC, 6);
  esp_now_add_peer(&peer);

  memset(seenIDs, 0, sizeof(seenIDs));

  Serial.println("[NodeB] Ready.");
}

// ── Main Loop ────────────────────────────────────────────────
void loop() {
  int   raw = analogRead(MQ2);
  emaValue  = EMA_ALPHA * raw + (1.0 - EMA_ALPHA) * emaValue;
  float fireThreshold = baseline * (1.0 + FIRE_PCT);

  Serial.printf("[NodeB] Raw:%d  EMA:%.1f  Baseline:%.1f  Threshold:%.1f\n",
                raw, emaValue, baseline, fireThreshold);

  if (emaValue > fireThreshold && !fireState) {
    fireState = true;
    digitalWrite(BUZZER, HIGH);

    FireMessage msg;
    strncpy(msg.nodeID, NODE_ID, sizeof(msg.nodeID));
    msg.messageID  = newMessageID();
    msg.hopCount   = 3;
    msg.isFire     = true;
    msg.smokeLevel = (uint16_t)emaValue;

    markSeen(msg.messageID);
    esp_now_send(broadcastMAC, (uint8_t *)&msg, sizeof(FireMessage));

    Serial.printf("[NodeB] 🔥 FIRE sent | MsgID:%u\n", msg.messageID);
  }
  else if (emaValue <= fireThreshold && fireState) {
    fireState = false;
    digitalWrite(BUZZER, LOW);

    FireMessage msg;
    strncpy(msg.nodeID, NODE_ID, sizeof(msg.nodeID));
    msg.messageID  = newMessageID();
    msg.hopCount   = 3;
    msg.isFire     = false;
    msg.smokeLevel = (uint16_t)emaValue;

    markSeen(msg.messageID);
    esp_now_send(broadcastMAC, (uint8_t *)&msg, sizeof(FireMessage));

    Serial.printf("[NodeB] ✅ SAFE sent | MsgID:%u\n", msg.messageID);
  }

  delay(300);
}
