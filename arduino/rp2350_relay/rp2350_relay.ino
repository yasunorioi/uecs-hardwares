// rp2350_relay.ino - Waveshare 8ch Relay + DI Node (RP2350B)
// Board: RP2350-ETH-8DI-8RO / RP2350-POE-ETH-8DI-8RO
// Relay: GPIO17-24 直接制御 (TCA9554不要)
// DI:    GPIO9-16, フォトカプラ絶縁, アクティブLOW, 割り込み検知
// RTC:   PCF85063 (I2C0: SDA=GPIO6, SCL=GPIO7)
// Comm:  W5500 SPI1 (CS=GPIO33, RST=GPIO25, SCK=GPIO34, MOSI=GPIO35, MISO=GPIO36)
// Framework: arduino-pico (Earle Philhower)
//
// Libraries: arduino-pico 4.5.2+, PubSubClient 2.8+,
//            ArduinoJson 7.x, NTPClient 3.2.1, W5500lwIP (arduino-pico内蔵)
//            SensirionI2cSht4x (optional)

#include <SPI.h>
#include <W5500lwIP.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>        // v7.x
#include <LittleFS.h>
#include <Wire.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <SensirionI2cSht4x.h>

#include "sw_watchdog.h"        // Pico SDK: repeating_timer + watchdog_reboot (そのまま流用)
#include "sensor_registry.h"   // I2C センサーアドレス/HA フィールド定義

// ========== Firmware Version ==========
const char* FW_VERSION = "1.0.0";
const char* FW_NAME    = "rp2350_relay";

// ========== Default Configuration ==========
const char* DEFAULT_MQTT_BROKER  = "192.168.15.14";
const int   DEFAULT_MQTT_PORT    = 1883;
const char* DEFAULT_HOUSE_ID     = "h2";
const char* DEFAULT_NODE_ID      = "waveshare_relay_01";
const char* DEFAULT_IP           = "";          // 空=DHCP
const char* DEFAULT_SUBNET       = "255.255.255.0";
const char* DEFAULT_GATEWAY      = "192.168.15.1";
const char* DEFAULT_DNS          = "8.8.8.8";

// ========== W5500 SPI1 Pins (GPIO33-36, RP2350B高番号GPIO) ==========
// 実機検証TODO: SPI1/SPI0どちらが正しいか要確認
const int W5500_CS   = 33;
const int W5500_RST  = 25;
const int W5500_SCK  = 34;
const int W5500_MOSI = 35;
const int W5500_MISO = 36;
// W5500 INT: GPIO8 (公式コードに定義なし。実機確認TODO)
const int W5500_INT  = -1;

// ========== I2C0 Pins (RTC PCF85063) ==========
const int I2C_SDA = 6;
const int I2C_SCL = 7;

// ========== PCF85063 RTC ==========
const uint8_t PCF85063_ADDR = 0x51;

// ========== Relay GPIO Pins (GPIO17-24 直接制御) ==========
const int RELAY_PINS[8] = {17, 18, 19, 20, 21, 22, 23, 24};

// ========== DI GPIO Pins (GPIO9-16, アクティブLOW) ==========
const int DI_PINS[8] = {9, 10, 11, 12, 13, 14, 15, 16};

// ========== Timing ==========
const int          SENSOR_INTERVAL      = 10;       // seconds: heartbeat publish
const int          ETH_CONNECT_TIMEOUT  = 15;       // seconds
const int          MQTT_RECONNECT_ATTEMPTS = 3;
const int          MQTT_RECONNECT_DELAY    = 5;     // seconds
const unsigned long REBOOT_INTERVAL       = 600000UL; // 10分 (Tier 3 WDT)
const int          MQTT_FAIL_THRESHOLD    = 3;
const unsigned long NTP_SYNC_INTERVAL     = 3600000UL; // 1時間

// ========== HW WDT (Tier 1) ==========
const int HW_WDT_TIMEOUT_MS = 8000;  // 8秒 (設計書 Tier 1)

// ========== Global State ==========
uint8_t      relayState          = 0x00;
unsigned long relayDurationEnd[8] = {0};

bool diState[8]     = {false};
bool diPrevState[8] = {false};
volatile bool diInterruptFlag = false;
unsigned long diLastDebounce  = 0;
const unsigned long DI_DEBOUNCE_MS = 50;

bool sht40_detected = false;
float g_sht40_temp  = NAN;
float g_sht40_hum   = NAN;

unsigned long ntpEpoch  = 0;   // NTP同期後のUTC epoch
unsigned long ntpMillis = 0;   // 同期時のmillis()

int mqttFailCount = 0;
int loopCount     = 0;

// ========== Objects ==========
Wiznet5500lwIP eth(W5500_CS, SPI1, W5500_INT);
WiFiClient     wifiClient;
PubSubClient   mqttClient(wifiClient);
WiFiUDP        ntpUDP;
NTPClient      timeClient(ntpUDP, "pool.ntp.org", 0);  // UTC
SensirionI2cSht4x sht4x;

String houseId;
String nodeId;
String mqttBroker;
int    mqttPort;
String mqttClientId;

// ========== Function Declarations ==========
void loadConfig();
void initEthernet();
void connectMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void publishHADiscovery();
void publishRelayState();
void publishDIState();
void publishSensorData();
void setRelay(uint8_t ch, bool on);
void initRelaysOff();
bool readDI();
void scanI2CSensors();
void readSensors();
void syncNTP();
bool rtcGetTime(struct tm* t);
bool rtcSetTime(struct tm* t);
unsigned long getCurrentEpoch();
void rebootWithReason(const char* reason);

// ============================================================
// DI Interrupt (共有ISR — 全ch共通フラグ)
// ============================================================
void diISR() {
  diInterruptFlag = true;
}

// ============================================================
// PCF85063 RTC helpers
// ============================================================
static uint8_t bcd2dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static uint8_t dec2bcd(uint8_t d) { return ((d / 10) << 4) | (d % 10); }

bool rtcGetTime(struct tm* t) {
  Wire.beginTransmission(PCF85063_ADDR);
  Wire.write(0x04);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)PCF85063_ADDR, (uint8_t)7) != 7) return false;

  t->tm_sec  = bcd2dec(Wire.read() & 0x7F);   // OSビットマスク
  t->tm_min  = bcd2dec(Wire.read() & 0x7F);
  t->tm_hour = bcd2dec(Wire.read() & 0x3F);
  t->tm_mday = bcd2dec(Wire.read() & 0x3F);
  Wire.read();                                  // weekday skip
  t->tm_mon  = bcd2dec(Wire.read() & 0x1F) - 1;
  t->tm_year = bcd2dec(Wire.read()) + 100;
  t->tm_isdst = 0;
  return true;
}

bool rtcSetTime(struct tm* t) {
  Wire.beginTransmission(PCF85063_ADDR);
  Wire.write(0x04);
  Wire.write(dec2bcd(t->tm_sec));
  Wire.write(dec2bcd(t->tm_min));
  Wire.write(dec2bcd(t->tm_hour));
  Wire.write(dec2bcd(t->tm_mday));
  Wire.write(0);
  Wire.write(dec2bcd(t->tm_mon + 1));
  Wire.write(dec2bcd(t->tm_year - 100));
  return Wire.endTransmission() == 0;
}

// NTP同期後にmillis()ベースで現在epochを返す (arduino-picoにtime()なし)
unsigned long getCurrentEpoch() {
  if (ntpEpoch == 0) return 0;
  return ntpEpoch + (millis() - ntpMillis) / 1000;
}

// ============================================================
// NTP Sync → ntpEpoch + PCF85063 RTC更新
// ============================================================
void syncNTP() {
  timeClient.begin();
  int retries = 5;
  while (!timeClient.update() && --retries > 0) {
    delay(1000);
    timeClient.forceUpdate();
  }
  if (retries == 0) {
    Serial.println("NTP: sync failed");
    return;
  }

  ntpEpoch  = timeClient.getEpochTime();
  ntpMillis = millis();
  Serial.printf("NTP: epoch=%lu (%s)\n", ntpEpoch, timeClient.getFormattedTime().c_str());

  // PCF85063 RTC書き込み (UTC)
  // 簡易変換: epoch → struct tm 相当 (arduino-picoでgmtime_r不安定なため手動)
  unsigned long e = ntpEpoch;
  struct tm t;
  t.tm_sec  = e % 60; e /= 60;
  t.tm_min  = e % 60; e /= 60;
  t.tm_hour = e % 24; e /= 24;
  // Zeller's / day-of-year計算省略: 年月日は省略しRTCには時分秒のみ書き込む
  // 完全な日付設定は実機検証TODO
  if (rtcSetTime(&t)) {
    Serial.println("RTC: time written (hms only)");
  }
}

// ============================================================
// Relay Control (GPIO直接)
// ============================================================
void initRelaysOff() {
  for (int i = 0; i < 8; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], LOW);
  }
  relayState = 0x00;
  Serial.println("All relays OFF (safe startup state)");
}

void setRelay(uint8_t ch, bool on) {
  if (ch < 1 || ch > 8) return;
  uint8_t idx = ch - 1;
  digitalWrite(RELAY_PINS[idx], on ? HIGH : LOW);
  if (on) relayState |=  (1 << idx);
  else    relayState &= ~(1 << idx);
  Serial.printf("Relay CH%d %s  (state=0x%02X)\n", ch, on ? "ON" : "OFF", relayState);
}

// ============================================================
// Digital Input
// ============================================================
bool readDI() {
  bool changed = false;
  for (int i = 0; i < 8; i++) {
    diState[i] = !digitalRead(DI_PINS[i]);  // アクティブLOW反転
    if (diState[i] != diPrevState[i]) {
      changed = true;
      Serial.printf("DI%d: %s\n", i + 1, diState[i] ? "ON" : "OFF");
    }
  }
  memcpy(diPrevState, diState, sizeof(diState));
  return changed;
}

// ============================================================
// I2C Sensor Scan
// ============================================================
void scanI2CSensors() {
  Serial.println("I2C scan:");
  for (uint8_t addr = 1; addr < 127; addr++) {
    if (addr == PCF85063_ADDR) continue;  // RTC skip
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  0x%02X -> ", addr);
      bool matched = false;
      for (int i = 0; i < SENSOR_REGISTRY_SIZE; i++) {
        if (SENSOR_REGISTRY[i].addr == addr) {
          Serial.printf("%s\n", SENSOR_REGISTRY[i].name);
          if (SENSOR_REGISTRY[i].type == SENSOR_SHT40) sht40_detected = true;
          matched = true;
          break;
        }
      }
      if (!matched) Serial.println("unknown");
    }
  }
  if (sht40_detected) {
    sht4x.begin(Wire, 0x44);
    Serial.println("SHT40 initialized");
  }
  Serial.printf("Sensors: SHT40=%d\n", sht40_detected);
}

void readSensors() {
  if (!sht40_detected) return;
  float temp, hum;
  uint16_t err;
  char msg[64];
  err = sht4x.measureHighPrecision(temp, hum);
  if (err) {
    errorToString(err, msg, sizeof(msg));
    Serial.printf("SHT40 error: %s\n", msg);
  } else {
    g_sht40_temp = temp;
    g_sht40_hum  = hum;
  }
}

// ============================================================
// Configuration (LittleFS)
// ============================================================
void loadConfig() {
  String ipStr = DEFAULT_IP;

  if (LittleFS.exists("/config.json")) {
    File file = LittleFS.open("/config.json", "r");
    if (file) {
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, file);
      file.close();

      if (!err) {
        Serial.println("Config loaded from /config.json");
        houseId    = (const char*)(doc["house_id"]    | DEFAULT_HOUSE_ID);
        nodeId     = (const char*)(doc["node_id"]     | DEFAULT_NODE_ID);
        mqttBroker = (const char*)(doc["mqtt_broker"] | DEFAULT_MQTT_BROKER);
        mqttPort   = doc["mqtt_port"] | DEFAULT_MQTT_PORT;
        ipStr      = (const char*)(doc["ip"]          | DEFAULT_IP);
        mqttClientId = String("rp2350-") + nodeId;

        // Static IP設定 (ip フィールドがあれば適用)
        if (ipStr.length() > 0) {
          IPAddress ip, subnet, gw, dns;
          ip.fromString(ipStr);
          subnet.fromString((const char*)(doc["subnet"] | DEFAULT_SUBNET));
          gw.fromString((const char*)(doc["gateway"]    | DEFAULT_GATEWAY));
          dns.fromString((const char*)(doc["dns"]       | DEFAULT_DNS));
          eth.config(ip, gw, subnet, dns);
          Serial.printf("Static IP: %s\n", ipStr.c_str());
        }
        return;
      }
      Serial.printf("Config parse error: %s\n", err.c_str());
    }
  }

  Serial.println("Using default configuration (DHCP)");
  houseId      = DEFAULT_HOUSE_ID;
  nodeId       = DEFAULT_NODE_ID;
  mqttBroker   = DEFAULT_MQTT_BROKER;
  mqttPort     = DEFAULT_MQTT_PORT;
  mqttClientId = String("rp2350-") + nodeId;
}

// ============================================================
// Ethernet (W5500 SPI1)
// ============================================================
void initEthernet() {
  // W5500 HW Reset
  pinMode(W5500_RST, OUTPUT);
  digitalWrite(W5500_RST, LOW);
  delay(100);
  digitalWrite(W5500_RST, HIGH);
  delay(500);

  // SPI1 カスタムピン (GPIO33-36)
  // 実機検証TODO: SPI1/SPI0の確認が必要
  SPI1.setSCK(W5500_SCK);
  SPI1.setTX(W5500_MOSI);
  SPI1.setRX(W5500_MISO);
  SPI1.begin();

  lwipPollingPeriod(5);
  eth.begin();

  Serial.println("ETH: waiting for DHCP/link...");
  unsigned long start = millis();
  while (!eth.connected()) {
    if (millis() - start > (unsigned long)ETH_CONNECT_TIMEOUT * 1000UL) {
      Serial.println("ETH: timeout");
      rebootWithReason("eth_dhcp_timeout");
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.printf("ETH IP: %s  GW: %s\n",
                eth.localIP().toString().c_str(),
                eth.gatewayIP().toString().c_str());
}

// ============================================================
// MQTT
// ============================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String topicStr = String(topic);
  String prefix   = String("agriha/") + houseId + "/relay/";

  if (!topicStr.startsWith(prefix)) return;

  String rest     = topicStr.substring(prefix.length());
  int    slashIdx = rest.indexOf('/');
  if (slashIdx < 0) return;

  int ch = rest.substring(0, slashIdx).toInt();
  if (ch < 1 || ch > 8) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.printf("MQTT parse error: %s\n", err.c_str());
    return;
  }

  int         value        = doc["value"]        | -1;
  int         duration_sec = doc["duration_sec"] | 0;
  const char* reason       = doc["reason"]       | "mqtt";

  if (value == 1) {
    setRelay(ch, true);
    relayDurationEnd[ch - 1] = (duration_sec > 0)
      ? millis() + (unsigned long)duration_sec * 1000UL
      : 0;
    Serial.printf("CH%d ON duration=%ds reason=%s\n", ch, duration_sec, reason);
  } else if (value == 0) {
    setRelay(ch, false);
    relayDurationEnd[ch - 1] = 0;
    Serial.printf("CH%d OFF reason=%s\n", ch, reason);
  }

  publishRelayState();
}

void connectMQTT() {
  for (int attempt = 0; attempt < MQTT_RECONNECT_ATTEMPTS; attempt++) {
    Serial.printf("MQTT %d/%d: %s:%d\n",
                  attempt + 1, MQTT_RECONNECT_ATTEMPTS,
                  mqttBroker.c_str(), mqttPort);

    if (mqttClient.connect(mqttClientId.c_str())) {
      Serial.println("MQTT connected");
      String subTopic = String("agriha/") + houseId + "/relay/+/set";
      mqttClient.subscribe(subTopic.c_str(), 1);
      Serial.printf("Subscribed: %s\n", subTopic.c_str());
      return;
    }

    Serial.printf("MQTT failed, state=%d\n", mqttClient.state());
    if (attempt < MQTT_RECONNECT_ATTEMPTS - 1) delay(MQTT_RECONNECT_DELAY * 1000);
  }

  rebootWithReason("mqtt_connect_failed");
}

// ============================================================
// Publish Functions
// ============================================================
void publishRelayState() {
  JsonDocument doc;
  for (int i = 1; i <= 8; i++) {
    doc[String("ch") + i] = (relayState >> (i - 1)) & 1;
  }
  doc["ts"]      = getCurrentEpoch();
  doc["node_id"] = nodeId;
  doc["uptime"]  = millis() / 1000;

  char buffer[256];
  serializeJson(doc, buffer);

  String topic = String("agriha/") + houseId + "/relay/state";
  mqttClient.publish(topic.c_str(), buffer, true);
}

void publishDIState() {
  JsonDocument doc;
  for (int i = 0; i < 8; i++) {
    doc[String("di") + (i + 1)] = diState[i] ? 1 : 0;
  }
  doc["ts"] = getCurrentEpoch();

  char buffer[160];
  serializeJson(doc, buffer);

  String topic = String("agriha/") + houseId + "/di/state";
  mqttClient.publish(topic.c_str(), buffer, true);
}

void publishSensorData() {
  if (!sht40_detected) return;

  JsonDocument doc;
  if (!isnan(g_sht40_temp)) doc["sht40_temperature"] = round(g_sht40_temp * 100) / 100.0;
  if (!isnan(g_sht40_hum))  doc["sht40_humidity"]    = round(g_sht40_hum  * 10)  / 10.0;
  doc["house_id"] = houseId;
  doc["node_id"]  = nodeId;
  doc["ts"]       = getCurrentEpoch();

  char buffer[160];
  serializeJson(doc, buffer);

  String topic = String("agriha/") + houseId + "/sensor/relay_node/state";
  mqttClient.publish(topic.c_str(), buffer);
}

// ============================================================
// HA MQTT Auto Discovery
// ============================================================
void publishHADiscovery() {
  const char* PREFIX = "homeassistant";

  // デバイス情報 (全エンティティ共通)
  JsonDocument deviceDoc;
  JsonArray ids = deviceDoc["identifiers"].to<JsonArray>();
  ids.add(nodeId);
  deviceDoc["name"]         = String("Waveshare Relay ") + houseId;
  deviceDoc["model"]        = "RP2350-POE-ETH-8DI-8RO";
  deviceDoc["manufacturer"] = "Waveshare";
  deviceDoc["sw_version"]   = FW_VERSION;

  String relayStateTopic = String("agriha/") + houseId + "/relay/state";
  String diTopic         = String("agriha/") + houseId + "/di/state";

  // --- 8ch リレー switch エンティティ ---
  for (int ch = 1; ch <= 8; ch++) {
    String uid      = String(nodeId.c_str()) + "_ch" + ch;
    String topic    = String(PREFIX) + "/switch/" + uid + "/config";
    String cmdTopic = String("agriha/") + houseId + "/relay/" + ch + "/set";

    JsonDocument doc;
    doc["name"]     = String("Relay CH") + ch;
    doc["stat_t"]   = relayStateTopic;
    doc["cmd_t"]    = cmdTopic;
    doc["val_tpl"]  = String("{{ value_json.ch") + ch + " }}";
    doc["pl_on"]    = "{\"value\":1}";
    doc["pl_off"]   = "{\"value\":0}";
    doc["stat_on"]  = 1;
    doc["stat_off"] = 0;
    doc["uniq_id"]  = uid;
    doc["dev"]      = deviceDoc;

    char buffer[768];
    serializeJson(doc, buffer);
    mqttClient.publish(topic.c_str(), buffer, true);
    Serial.printf("HA switch/%s\n", uid.c_str());
    delay(100);
  }

  // --- 8ch DI binary_sensor エンティティ ---
  for (int di = 1; di <= 8; di++) {
    String uid   = String(nodeId.c_str()) + "_di" + di;
    String topic = String(PREFIX) + "/binary_sensor/" + uid + "/config";

    JsonDocument doc;
    doc["name"]    = String("DI") + di;
    doc["stat_t"]  = diTopic;
    doc["val_tpl"] = String("{{ value_json.di") + di + " }}";
    doc["pl_on"]   = 1;
    doc["pl_off"]  = 0;
    doc["dev_cla"] = "power";
    doc["uniq_id"] = uid;
    doc["dev"]     = deviceDoc;

    char buffer[512];
    serializeJson(doc, buffer);
    mqttClient.publish(topic.c_str(), buffer, true);
    Serial.printf("HA binary_sensor/%s\n", uid.c_str());
    delay(100);
  }

  // --- I2Cセンサー (検出分のみ) ---
  if (sht40_detected) {
    String sensorTopic = String("agriha/") + houseId + "/sensor/relay_node/state";

    for (int i = 0; i < HA_FIELDS_SIZE; i++) {
      if (HA_FIELDS[i].source != SENSOR_SHT40) continue;

      String uid   = String(nodeId.c_str()) + "_" + HA_FIELDS[i].key;
      String topic = String(PREFIX) + "/sensor/" + uid + "/config";

      JsonDocument doc;
      doc["name"]         = HA_FIELDS[i].name;
      doc["stat_t"]       = sensorTopic;
      doc["val_tpl"]      = String("{{ value_json.") + HA_FIELDS[i].key + " }}";
      doc["unit_of_meas"] = HA_FIELDS[i].unit;
      doc["dev_cla"]      = HA_FIELDS[i].dev_class;
      if (HA_FIELDS[i].icon) doc["ic"] = HA_FIELDS[i].icon;
      doc["uniq_id"]      = uid;
      doc["dev"]          = deviceDoc;

      char buffer[512];
      serializeJson(doc, buffer);
      mqttClient.publish(topic.c_str(), buffer, true);
      Serial.printf("HA sensor/%s\n", uid.c_str());
      delay(100);
    }
  }

  Serial.println("HA Discovery published");
}

// ============================================================
// Reboot (Pico SDK watchdog)
// ============================================================
void rebootWithReason(const char* reason) {
  Serial.printf("Rebooting: %s\n", reason);

  File file = LittleFS.open("/reboot_reason.txt", "w");
  if (file) { file.print(reason); file.close(); }

  delay(500);
  watchdog_reboot(0, 0, 0);
  while (true) { /* wait */ }
}

// ============================================================
// Setup
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.printf("=== %s v%s ===\n", FW_NAME, FW_VERSION);
  Serial.println("Board: RP2350-POE-ETH-8DI-8RO");

  // --- 安全最優先: 全リレーOFF ---
  initRelaysOff();

  // --- DI ピン初期化 + 割り込み設定 ---
  for (int i = 0; i < 8; i++) {
    pinMode(DI_PINS[i], INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(DI_PINS[i]), diISR, CHANGE);
  }
  Serial.println("DI: GPIO9-16, active LOW, interrupt CHANGE");

  // --- I2C0 (RTC + センサー) ---
  Wire.setSDA(I2C_SDA);
  Wire.setSCL(I2C_SCL);
  Wire.begin();
  delay(200);
  Serial.printf("I2C0: SDA=%d SCL=%d\n", I2C_SDA, I2C_SCL);

  // --- LittleFS ---
  if (!LittleFS.begin()) {
    Serial.println("LittleFS: mount failed, formatting...");
    LittleFS.format();
    if (!LittleFS.begin()) {
      Serial.println("WARNING: LittleFS unavailable — using defaults");
    } else {
      Serial.println("LittleFS: formatted and mounted");
    }
  } else {
    Serial.println("LittleFS: mounted");
  }

  // --- Config (静的IPはここで eth.config() に反映) ---
  loadConfig();
  Serial.printf("Node=%s House=%s MQTT=%s:%d\n",
                nodeId.c_str(), houseId.c_str(),
                mqttBroker.c_str(), mqttPort);

  // --- Ethernet ---
  initEthernet();

  // --- NTP sync ---
  syncNTP();

  // --- I2C センサースキャン ---
  scanI2CSensors();
  readSensors();

  // --- MQTT ---
  mqttClient.setServer(mqttBroker.c_str(), mqttPort);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(60);
  mqttClient.setBufferSize(1024);
  connectMQTT();

  // --- HA Discovery ---
  publishHADiscovery();

  // --- 初期状態パブリッシュ ---
  publishRelayState();
  readDI();
  publishDIState();
  publishSensorData();

  // --- FWバージョン (retained) ---
  {
    String vTopic   = String("agriha/") + nodeId + "/version";
    String vPayload = String("{\"firmware\":\"") + FW_NAME +
                      "\",\"version\":\"" + FW_VERSION + "\"}";
    mqttClient.publish(vTopic.c_str(), vPayload.c_str(), true);
  }

  // --- Watchdog 3段構え ---
  // Tier 1: HW WDT (Pico SDK)
  watchdog_enable(HW_WDT_TIMEOUT_MS, true);
  Serial.printf("HW WDT: %dms\n", HW_WDT_TIMEOUT_MS);

  // Tier 2: SW WDT (sw_watchdog.h そのまま流用)
  swWdtStart();
  Serial.printf("SW WDT: check=%lums threshold=%d\n",
                SWD_CHECK_MS, SWD_MISS_THRESHOLD);

  Serial.println("=== Setup complete ===\n");
}

// ============================================================
// Main Loop
// ============================================================
void loop() {
  loopCount++;

  // Watchdog feed (両方)
  watchdog_update();          // Tier 1: HW WDT
  swWdtFeed();                // Tier 2: SW WDT

  // Tier 3: 定期リブート (10分)
  if (millis() >= REBOOT_INTERVAL) {
    rebootWithReason("periodic_10min_reboot");
  }

  // Ethernet 死活確認
  if (!eth.connected()) {
    Serial.println("ETH: disconnected, rebooting...");
    rebootWithReason("eth_disconnected");
  }

  // MQTT チェック + 処理
  if (!mqttClient.connected()) {
    Serial.println("MQTT: disconnected, reconnecting...");
    connectMQTT();
  }
  mqttClient.loop();

  // Duration auto-off チェック
  unsigned long now = millis();
  bool relayChanged = false;
  for (int i = 0; i < 8; i++) {
    if (relayDurationEnd[i] > 0 && now >= relayDurationEnd[i]) {
      setRelay(i + 1, false);
      relayDurationEnd[i] = 0;
      relayChanged = true;
      Serial.printf("CH%d auto-OFF (duration expired)\n", i + 1);
    }
  }
  if (relayChanged) publishRelayState();

  // DI 割り込みフラグ → 状態変化時に即パブリッシュ
  if (diInterruptFlag && (now - diLastDebounce >= DI_DEBOUNCE_MS)) {
    diInterruptFlag = false;
    diLastDebounce  = now;
    if (readDI()) publishDIState();
  }

  // ハートビート (SENSOR_INTERVAL 秒周期)
  static unsigned long lastHeartbeat = 0;
  if (now - lastHeartbeat >= (unsigned long)SENSOR_INTERVAL * 1000UL) {
    lastHeartbeat = now;

    readSensors();

    bool ok = mqttClient.publish("agriha/heartbeat", nodeId.c_str());
    if (!ok) {
      mqttFailCount++;
      Serial.printf("MQTT publish fail: %d/%d\n", mqttFailCount, MQTT_FAIL_THRESHOLD);
      if (mqttFailCount >= MQTT_FAIL_THRESHOLD) {
        rebootWithReason("mqtt_fail_count_exceeded");
      }
    } else {
      mqttFailCount = 0;
    }

    publishRelayState();
    publishDIState();
    publishSensorData();

    Serial.printf("[%d] relay=0x%02X epoch=%lu uptime=%lus\n",
                  loopCount, relayState, getCurrentEpoch(), millis() / 1000);
  }

  // 定期NTP再同期 (1時間)
  static unsigned long lastNtpSync = 0;
  if (now - lastNtpSync >= NTP_SYNC_INTERVAL) {
    lastNtpSync = now;
    syncNTP();
  }

  delay(50);
}
