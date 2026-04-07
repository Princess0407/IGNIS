// ============================================================
//  NODE A — Fire Alarm Mesh Node (Origin + Captive Portal)
//  Features: ESP-NOW Broadcast, Unique Message IDs,
//            Broadcast Storm Prevention (hopCount + seen IDs),
//            Dynamic Baseline Calibration, EMA Filter,
//            Captive Portal Emergency Wi-Fi AP
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
const char* AP_SSID     = "EVACUATE BUILDING!!";
const char* AP_PASSWORD = "";           // Open network — no password
const byte  DNS_PORT    = 53;
IPAddress   AP_IP(192, 168, 4, 1);
IPAddress   AP_SUBNET(255, 255, 255, 0);

DNSServer  dnsServer;
WebServer  webServer(80);
bool       portalActive = false;

// ── Message Struct ───────────────────────────────────────────
// Sent over ESP-NOW.
typedef struct FireMessage {
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
const float EMA_ALPHA = 0.2; // Smoothing factor (0=no update, 1=no smoothing)
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

  Serial.printf("[NodeA] Relayed msg from %s | ID:%u | Fire:%d | Hops left:%d\n",
                msg.nodeID, msg.messageID, msg.isFire, msg.hopCount);

  // Actuate locally
  if (msg.isFire) {
    digitalWrite(BUZZER, HIGH);
    if (!portalActive) startCaptivePortal();
  } else {
    digitalWrite(BUZZER, LOW);
    // Note: we intentionally leave the portal up until manual reset
    // so latecomers still see the evacuation page.
  }

  // Relay forward only if hops remain
  if (msg.hopCount > 0) {
    msg.hopCount--;
    esp_now_send(broadcastMAC, (uint8_t *)&msg, sizeof(FireMessage));
  }
}

// ── ESP-NOW Send Callback (optional debug) ───────────────────
void onSent(const uint8_t *mac, esp_now_send_status_t status) {
  Serial.printf("[NodeA] Send status: %s\n",
                status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

// ── Captive Portal ───────────────────────────────────────────
// The emergency HTML page served to any phone that connects.
const char EVAC_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>FIRE ALERT</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=Black+Ops+One&family=Share+Tech+Mono&display=swap');

  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

  :root {
    --red:    #FF1A1A;
    --orange: #FF6B00;
    --dark:   #0A0000;
    --white:  #FFFFFF;
  }

  body {
    background: var(--dark);
    color: var(--white);
    font-family: 'Share Tech Mono', monospace;
    min-height: 100vh;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    overflow: hidden;
    animation: bgPulse 0.8s ease-in-out infinite alternate;
  }

  @keyframes bgPulse {
    from { background-color: #0A0000; }
    to   { background-color: #2A0000; }
  }

  .icon {
    font-size: clamp(4rem, 15vw, 8rem);
    animation: iconShake 0.3s ease-in-out infinite alternate;
    filter: drop-shadow(0 0 20px var(--red));
  }

  @keyframes iconShake {
    from { transform: rotate(-5deg) scale(1.0); }
    to   { transform: rotate( 5deg) scale(1.08); }
  }

  h1 {
    font-family: 'Black Ops One', cursive;
    font-size: clamp(2.2rem, 10vw, 5rem);
    color: var(--red);
    text-shadow: 0 0 30px var(--red), 0 0 60px var(--orange);
    text-align: center;
    letter-spacing: 0.05em;
    margin: 0.4em 0;
    animation: textFlicker 1.2s steps(1) infinite;
  }

  @keyframes textFlicker {
    0%, 95%, 100% { opacity: 1; }
    96%            { opacity: 0.4; }
  }

  .sub {
    font-size: clamp(1rem, 4vw, 1.6rem);
    color: var(--orange);
    text-align: center;
    letter-spacing: 0.15em;
    text-transform: uppercase;
    margin-bottom: 2rem;
  }

  .instructions {
    background: rgba(255, 26, 26, 0.12);
    border: 2px solid var(--red);
    border-radius: 8px;
    padding: 1.2rem 2rem;
    max-width: 480px;
    width: 90%;
    text-align: center;
  }

  .instructions p {
    font-size: clamp(0.85rem, 3vw, 1.1rem);
    line-height: 1.8;
    color: #FFD0D0;
  }

  .instructions strong {
    color: var(--white);
  }

  .node-badge {
    margin-top: 2rem;
    font-size: 0.75rem;
    color: #550000;
    letter-spacing: 0.2em;
  }

  .bar {
    width: 90%;
    max-width: 480px;
    height: 6px;
    background: var(--red);
    border-radius: 3px;
    margin: 1.5rem 0 0.5rem;
    animation: barPulse 0.6s ease-in-out infinite alternate;
  }

  @keyframes barPulse {
    from { opacity: 1;   transform: scaleX(1); }
    to   { opacity: 0.4; transform: scaleX(0.96); }
  }
</style>
</head>
<body>
  <div class="icon"></div>
  <h1>FIRE DETECTED</h1>
  <p class="sub">Please Evacuate Immediately</p>
  <div class="bar"></div>
  <div class="instructions">
    <p>
      <strong>LEAVE the building NOW.</strong><br>
      Use the nearest stairwell.<br>
      Do <strong>NOT</strong> use elevators.<br>
      Proceed to the emergency assembly point.<br>
      Call <strong>112 / 101</strong> immediately.
    </p>
  </div>
  <p class="node-badge">ALERT ORIGIN: NODE A &nbsp;|&nbsp; MESH FIRE ALARM SYSTEM</p>
</body>
</html>
)rawliteral";

void handleRoot() {
  webServer.send(200, "text/html", EVAC_PAGE);
}

void handleNotFound() {
  // Redirect ALL unknown URLs to the evacuation page (captive portal trick)
  webServer.sendHeader("Location", "http://192.168.4.1/", true);
  webServer.send(302, "text/plain", "");
}

void startCaptivePortal() {
  Serial.println("[NodeA] Starting Captive Portal AP...");

  // Switch to AP+STA mode so ESP-NOW keeps running on the STA radio
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_IP, AP_SUBNET);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  // DNS: redirect every domain to our IP
  dnsServer.start(DNS_PORT, "*", AP_IP);

  webServer.on("/",         handleRoot);
  webServer.on("/hotspot-detect.html", handleRoot);  // iOS captive portal probe
  webServer.on("/generate_204",        handleRoot);  // Android captive portal probe
  webServer.on("/connecttest.txt",     handleRoot);  // Windows probe
  webServer.onNotFound(handleNotFound);

  webServer.begin();

  portalActive = true;
  Serial.printf("[NodeA] Portal up. SSID: %s  IP: %s\n", AP_SSID, AP_IP.toString().c_str());
}

// ── Dynamic Baseline Calibration ────────────────────────────
void calibrateBaseline() {
  Serial.println("[NodeA] Calibrating baseline (10s)...");
  long sum = 0;
  const int SAMPLES = 100;
  for (int i = 0; i < SAMPLES; i++) {
    sum += analogRead(MQ2);
    delay(100);
  }
  baseline  = (float)sum / SAMPLES;
  emaValue  = baseline;
  Serial.printf("[NodeA] Baseline set: %.1f\n", baseline);
}

// ── Setup ────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, LOW);

  // Seed random for message IDs
  randomSeed(analogRead(0));

  // Start in STA mode for ESP-NOW
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(300);

  // Calibrate sensor before initialising network
  calibrateBaseline();

  // ESP-NOW init
  if (esp_now_init() != ESP_OK) {
    Serial.println("[NodeA] ESP-NOW init FAILED");
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

  Serial.println("[NodeA] Ready.");
}

// ── Main Loop ────────────────────────────────────────────────
void loop() {
  // Service captive portal if active
  if (portalActive) {
    dnsServer.processNextRequest();
    webServer.handleClient();
  }

  // ── EMA filter ──────────────────────────────────────────
  int   raw      = analogRead(MQ2);
  emaValue = EMA_ALPHA * raw + (1.0 - EMA_ALPHA) * emaValue;
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

    // THE TRIPLE-SEND FIX
    for(int i = 0; i < 3; i++) {
      esp_now_send(broadcastMAC, (uint8_t *)&msg, sizeof(FireMessage));
      delay(20);
    }

    if (!portalActive) startCaptivePortal();
    Serial.printf("[NodeA]  FIRE sent | MsgID:%u\n", msg.messageID);
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

    Serial.printf("[NodeA] SAFE sent | MsgID:%u\n", msg.messageID);
  }

  delay(300);
}
