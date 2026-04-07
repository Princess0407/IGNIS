// ============================================================
//  NODE A — Fire Alarm Mesh Node (Origin + Captive Portal)
// ============================================================

#include <WiFi.h>
#include <esp_now.h>
#include <DNSServer.h>
#include <WebServer.h>

// ── Pin Definitions ──────────────────────────────────────────
#define MQ2    34
#define BUZZER 25

// ── ESP-NOW Broadcast MAC ────────────────────────────────────
uint8_t broadcastMAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ── Node Identity ────────────────────────────────────────────
#define NODE_ID "NODE_A"

// ── Captive Portal Config ────────────────────────────────────
const char* AP_SSID     = "🚨 EVACUATE BUILDING 🚨";
const char* AP_PASSWORD = "";
const byte  DNS_PORT    = 53;
IPAddress   AP_IP(192, 168, 4, 1);
IPAddress   AP_SUBNET(255, 255, 255, 0);

DNSServer  dnsServer;
WebServer  webServer(80);
bool       portalActive = false;

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

// ── Captive Portal Control ───────────────────────────────────
const char EVAC_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><body style="background:red;color:white;text-align:center;font-family:sans-serif;margin-top:20%;">
<h1 style="font-size: 50px;">🔥 FIRE DETECTED 🔥</h1>
<h2>EVACUATE IMMEDIATELY</h2>
<p>Do not use elevators. Proceed to the nearest stairs.</p>
</body></html>
)rawliteral";

void handleRoot() {
  webServer.send(200, "text/html", EVAC_PAGE);
}

void handleNotFound() {
  webServer.sendHeader("Location", "http://192.168.4.1/", true);
  webServer.send(302, "text/plain", "");
}

void startCaptivePortal() {
  if (portalActive) return; // Prevent restarting if already up
  
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_IP, AP_SUBNET);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  dnsServer.start(DNS_PORT, "*", AP_IP);

  webServer.on("/", handleRoot);
  webServer.onNotFound(handleNotFound);
  webServer.begin();

  portalActive = true;
  Serial.println("[NodeA] Portal Started");
}

void stopCaptivePortal() {
  if (!portalActive) return;
  
  Serial.println("[NodeA] Stopping Portal...");
  dnsServer.stop();
  webServer.stop();
  WiFi.softAPdisconnect(true);
  portalActive = false;
}

// ── ESP-NOW Receive Callback ─────────────────────────────────
void onReceive(const esp_now_recv_info *info, const uint8_t *data, int len) {
  if (len != sizeof(FireMessage)) return;

  FireMessage msg;
  memcpy(&msg, data, sizeof(FireMessage));

  if (alreadySeen(msg.messageID)) return;
  markSeen(msg.messageID);

  Serial.printf("[NodeA] From %s | ID:%u | Fire:%d | Hops:%d\n",
                msg.nodeID, msg.messageID, msg.isFire, msg.hopCount);

  if (msg.isFire) {
    digitalWrite(BUZZER, HIGH);
    startCaptivePortal();
  } else {
    digitalWrite(BUZZER, LOW);
    stopCaptivePortal();
  }

  if (msg.hopCount > 0) {
    msg.hopCount--;
    esp_now_send(broadcastMAC, (uint8_t *)&msg, sizeof(FireMessage));
  }
}

// ── ESP-NOW Send Callback (FIXED v3 Signature) ───────────────
void onSent(const esp_now_send_info_t *info, esp_now_send_status_t status) {
  Serial.printf("[NodeA] Send status: %s\n",
                status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

// ── Calibration ──────────────────────────────────────────────
void calibrateBaseline() {
  Serial.println("[NodeA] Calibrating Baseline...");
  long sum = 0;
  for (int i = 0; i < 50; i++) {
    sum += analogRead(MQ2);
    delay(100);
  }
  baseline = sum / 50.0;
  emaValue = baseline;
  Serial.printf("[NodeA] Baseline set: %.1f\n", baseline);
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

  Serial.println("[NodeA] Ready");
}

// ── Loop ─────────────────────────────────────────────────────
void loop() {
  // 1. Constantly handle web requests (No blocking delays here!)
  if (portalActive) {
    dnsServer.processNextRequest();
    webServer.handleClient();
  }

  // 2. Non-blocking timer for sensor reads (every 300ms)
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

      startCaptivePortal();
      Serial.println("🔥 FIRE SENT");
    }
    
    // ✅ SAFE DETECTED (Resets the alarm and portal)
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
      
      stopCaptivePortal();
      Serial.println("✅ SAFE SENT");
    }
  }
}
