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
// Sent over ESP-NOW. Packed to minimise radio payload.
typedef struct __attribute__((packed)) FireMessage {
  char     nodeID[8];      // Origin node identifier
  uint32_t messageID;      // Random unique ID
  uint8_t  hopCount;       // Decremented each relay; drop when 0
  bool     isFire;         // true = FIRE, false = SAFE
  uint16_t smokeLevel;     // Raw (EMA-filtered) sensor reading
} FireMessage;

// ── Broadcast Storm Prevention ───────────────────────────────
#define SEEN_ID_BUFFER 10
uint32_t seenIDs[SEEN_ID_BUFFER];
uint8_t  seenIndex = 0;

bool alreadySeen(uint32_t id) {
  for (int i = 0; i < SEEN_ID_BUFFER; i++) {
    if (seenIDs[i] == id) return true;
  }
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
const float EMA_ALPHA = 0.2;  // Smoothing factor 
const float FIRE_PCT  = 0.20; // Trigger if EMA > baseline * (1 + FIRE_PCT)

// ── Helper: generate a random 32-bit message ID ──────────────
uint32_t newMessageID() {
  return (uint32_t)esp_random();
}

// ── ESP-NOW Receive Callback ─────────────────────────────────
void onReceive(const esp_now_recv_info *info, const uint8_t *data, int len) {
  if (len != sizeof(FireMessage)) return;
  
  FireMessage msg;
  memcpy(&msg, data, sizeof(FireMessage));

  // Drop if we already processed this message
  if (alreadySeen(msg.messageID)) return;
  markSeen(msg.messageID);

  Serial.printf("[NodeB] Relayed msg from %s | ID:%u | Fire:%d | Hops left:%d\n",
                msg.nodeID, msg.messageID, msg.isFire, msg.hopCount);

  //FIXED 
  if (msg.isFire) {
    digitalWrite(BUZZER, HIGH);
  } else {
    digitalWrite(BUZZER, LOW);
  }

  // Relay forward only if hops remain
  if (msg.hopCount > 0) {
    msg.hopCount--;
    esp_now_send(broadcastMAC, (uint8_t *)&msg, sizeof(FireMessage));
  }
}

// ── ESP-NOW Send Callback (optional debug) ───────────────────
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

  // Seed random for message IDs
  randomSeed(analogRead(0));

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(300);

  // Calibrate sensor before initialising network
  calibrateBaseline();

  // ESP-NOW init
  if (esp_now_init() != ESP_OK) {
    Serial.println("[NodeB] ESP-NOW init FAILED");
    return;
  }
  esp_now_register_recv_cb(onReceive);
  esp_now_register_send_cb(onSent);

  // Register broadcast peer
  esp_now_peer_info_t peer = {};
  peer.channel = 0;
  peer.encrypt = false;
  memcpy(peer.peer_addr, broadcastMAC, 6);
  esp_now_add_peer(&peer);

  // Clear seen-ID buffer
  memset(seenIDs, 0, sizeof(seenIDs));

  Serial.println("[NodeB] Ready.");
}

// ── Main Loop ────────────────────────────────────────────────
void loop() {
  // ── EMA filter ──────────────────────────────────────────
  int   raw = analogRead(MQ2);
  emaValue  = EMA_ALPHA * raw + (1.0 - EMA_ALPHA) * emaValue;
  float fireThreshold = baseline * (1.0 + FIRE_PCT);

  // ── Fire detection ───────────────────────────────────────
  if (emaValue > fireThreshold && !fireState) {
    fireState = true;
    digitalWrite(BUZZER, HIGH);

    // Build and broadcast fire message
    FireMessage msg;
    strncpy(msg.nodeID, NODE_ID, sizeof(msg.nodeID));
    msg.messageID  = newMessageID();
    msg.hopCount   = 3;
    msg.isFire     = true;
    msg.smokeLevel = (uint16_t)emaValue;

    markSeen(msg.messageID);

    // TRIPLE-SEND FIX
    for(int i = 0; i < 3; i++) {
      esp_now_send(broadcastMAC, (uint8_t *)&msg, sizeof(FireMessage));
      delay(20);
    }

    Serial.printf("[NodeB] FIRE sent | MsgID:%u\n", msg.messageID);
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

    Serial.printf("[NodeB] SAFE sent | MsgID:%u\n", msg.messageID);
  }

  delay(300);
}
