// ============================================================
//  NODE C — Fire Alarm Mesh Node (Relay)
// ============================================================

#include <WiFi.h>
#include <esp_now.h>

// ── Pin Definitions ──────────────────────────────────────────
#define MQ2    34
#define BUZZER 25

// ── ESP-NOW Broadcast MAC ────────────────────────────────────
uint8_t broadcastMAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ── Node Identity ────────────────────────────────────────────
#define NODE_ID "NODE_C"

// ── Message Struct ───────────────────────────────────────────
typedef struct FireMessage {
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
bool     fireState    = false;
float    emaValue     = 0.0;
float    baseline     = 0.0;
const float EMA_ALPHA = 0.2;
const float FIRE_PCT  = 0.25; // Bumped slightly to prevent warm-up false alarms
unsigned long lastReadTime = 0; // For non-blocking timer

// ── Helper ───────────────────────────────────────────────────
uint32_t newMessageID() {
  return (uint32_t)esp_random();
}

// ── ESP-NOW Receive Callback ─────────────────────────────────
void onReceive(const esp_now_recv_info *info, const uint8_t *data, int len) {
  if (len != sizeof(FireMessage)) return;

  FireMessage msg;
  memcpy(&msg, data, sizeof(msg));

  if (alreadySeen(msg.messageID)) return;
  markSeen(msg.messageID);

  Serial.printf("[NodeC] From %s | ID:%u | Fire:%d | Hops:%d\n",
                msg.nodeID, msg.messageID, msg.isFire, msg.hopCount);

  // Actuate buzzer, but DO NOT change fireState here
  if (msg.isFire) {
    digitalWrite(BUZZER, HIGH);
  } else {
    digitalWrite(BUZZER, LOW);
  }

  // Relay forward
  if (msg.hopCount > 0) {
    msg.hopCount--;
    esp_now_send(broadcastMAC, (uint8_t *)&msg, sizeof(msg));
  }
}

// ── ESP-NOW Send Callback (FIXED v3 Signature) ───────────────
void onSent(const esp_now_send_info_t *info, esp_now_send_status_t status) {
  Serial.printf("[NodeC] Send status: %s\n",
                status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

// ── Calibration ──────────────────────────────────────────────
void calibrateBaseline() {
  Serial.println("[NodeC] Calibrating Baseline...");
  long sum = 0;
  for (int i = 0; i < 50; i++) {
    sum += analogRead(MQ2);
    delay(100);
  }
  baseline = sum / 50.0;
  emaValue = baseline;
  Serial.printf("[NodeC] Baseline set: %.1f\n", baseline);
}

// ── Setup ────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  calibrateBaseline();

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_register_recv_cb(onReceive);
  esp_now_register_send_cb(onSent);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, broadcastMAC, 6);
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  Serial.println("[NodeC] Ready");
}

// ── Loop ─────────────────────────────────────────────────────
void loop() {
  // Non-blocking timer for sensor reads (every 300ms)
  if (millis() - lastReadTime >= 300) {
    lastReadTime = millis();

    int raw = analogRead(MQ2);
    emaValue = EMA_ALPHA * raw + (1 - EMA_ALPHA) * emaValue;
    float threshold = baseline * (1 + FIRE_PCT);

    // 🔥 FIRE DETECTED
    if (emaValue > threshold && !fireState) {
      fireState = true;
      digitalWrite(BUZZER, HIGH);

      FireMessage msg;
      strcpy(msg.nodeID, NODE_ID);
      msg.messageID = newMessageID();
      msg.hopCount = 3;
      msg.isFire = true;
      msg.smokeLevel = emaValue;

      markSeen(msg.messageID);

      // Triple-send to guarantee delivery
      for (int i = 0; i < 3; i++) {
        esp_now_send(broadcastMAC, (uint8_t *)&msg, sizeof(msg));
        delay(20);
      }

      Serial.println("🔥 FIRE SENT");
    }
    
    // ✅ SAFE DETECTED (Resets the alarm)
    else if (emaValue <= threshold && fireState) {
      fireState = false;
      digitalWrite(BUZZER, LOW);

      FireMessage msg;
      strcpy(msg.nodeID, NODE_ID);
      msg.messageID = newMessageID();
      msg.hopCount = 3;
      msg.isFire = false;
      msg.smokeLevel = emaValue;

      markSeen(msg.messageID);
      
      esp_now_send(broadcastMAC, (uint8_t *)&msg, sizeof(msg));
      
      Serial.println("✅ SAFE SENT");
    }
  }
}
