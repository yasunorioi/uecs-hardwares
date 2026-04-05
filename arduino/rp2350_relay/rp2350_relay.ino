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
//            SensirionI2cSht4x (optional), LEAmDNS (arduino-pico内蔵)

#include <SPI.h>
#include <W5500lwIP.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>        // v7.x
#include <LittleFS.h>
#include <Wire.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <SensirionI2cSht4x.h>
#include <LEAmDNS.h>

#include "sw_watchdog.h"        // Pico SDK: repeating_timer + watchdog_reboot (そのまま流用)
#include "sensor_registry.h"   // I2C センサーアドレス/HA フィールド定義

// ========== Firmware Version ==========
const char* FW_VERSION = "1.2.0";  // +mDNS +graceful sensor degradation +config WebUI
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
const bool  DEFAULT_MDNS_ENABLED = true;

// ========== W5500 SPI1 Pins (GPIO33-36, RP2350B高番号GPIO) ==========
// 実機検証TODO: SPI1/SPI0どちらが正しいか要確認
const int W5500_CS   = 33;
const int W5500_RST  = 25;
const int W5500_SCK  = 34;
const int W5500_MOSI = 35;
const int W5500_MISO = 36;
// W5500 INT: GPIO8 (公式コードに定義なし。実機確認TODO)
const int W5500_INT  = -1;

// ========== RS485 UART1 Pins (GPIO4/5, auto-direction transceiver) ==========
const int RS485_TX = 4;
const int RS485_RX = 5;
const int RS485_DEFAULT_BAUD = 9600;

// ========== I2C0 Pins (RTC PCF85063) ==========
const int I2C_SDA = 6;
const int I2C_SCL = 7;

// ========== PCF85063 RTC ==========
const uint8_t PCF85063_ADDR = 0x51;

// ========== Relay GPIO Pins (GPIO17-24 直接制御) ==========
const int RELAY_PINS[8] = {17, 18, 19, 20, 21, 22, 23, 24};

// ========== DI GPIO Pins (GPIO9-16, アクティブLOW) ==========
const int DI_PINS[8] = {9, 10, 11, 12, 13, 14, 15, 16};

// ========== Channel Aliases (§8.1 RO, §8.2 DI) ==========
const char* RO_NAMES[8] = {
  "側窓A開", "側窓A閉", "側窓B開", "側窓B閉",
  "電磁弁", "循環扇1", "循環扇2", "予備"
};
const char* RO_KEYS[8] = {
  "side_window_a_open", "side_window_a_close",
  "side_window_b_open", "side_window_b_close",
  "solenoid_valve", "fan_1", "fan_2", "spare"
};
const char* DI_NAMES[8] = {
  "灌水パルス1", "灌水パルス2",
  "側窓A開端", "側窓A閉端", "側窓B開端", "側窓B閉端",
  "予備DI7", "予備DI8"
};

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
volatile bool     diInterruptFlag  = false;
volatile uint32_t diPulseCount[2]  = {0, 0};  // DI1-2 パルスカウント (flow meter)
unsigned long diLastDebounce  = 0;
const unsigned long DI_DEBOUNCE_MS = 50;

bool  sht40_detected    = false;
int   sht40_error_count = 0;     // consecutive read errors; >= 3 → disable until reboot
float g_sht40_temp      = NAN;
float g_sht40_hum       = NAN;

bool mdns_enabled = DEFAULT_MDNS_ENABLED;

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
WiFiServer        webServer(80);

String houseId;
String nodeId;
String mqttBroker;
int    mqttPort;
String mqttClientId;
int    rs485Baud = RS485_DEFAULT_BAUD;

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
void publishDIPulse();
void handleWebClient();
void sendDashboard(WiFiClient& client);
void sendAPIState(WiFiClient& client);
void sendAPIConfig(WiFiClient& client);
void sendConfigPage(WiFiClient& client);
void handleRelayPost(WiFiClient& client, int ch, const String& body);
void handleConfigPost(WiFiClient& client, const String& body);
void initRS485();
void pollDrainSensor();
uint16_t modbusCalcCRC(const uint8_t* data, size_t len);

// ============================================================
// DI Interrupt
// DI1-2: FALLING edge パルスカウント (流量計)
// DI3-8: CHANGE 状態変化フラグ (リミットスイッチ等)
// ============================================================
void diPulseISR1() { diPulseCount[0]++; }
void diPulseISR2() { diPulseCount[1]++; }
void diISR() { diInterruptFlag = true; }

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
// Wire.setTimeout prevents bus-stuck hang during scan.
// ============================================================
void scanI2CSensors() {
  Wire.setTimeout(10);  // 10ms per transaction; prevents hang on stuck bus
  Serial.println("I2C scan:");
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    if (addr == PCF85063_ADDR) continue;  // RTC skip
    Wire.beginTransmission(addr);
    uint8_t rc = Wire.endTransmission();
    if (rc == 0) {
      found++;
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
  if (found == 0) {
    Serial.println("I2C scan: no devices found (sensor not connected — continuing)");
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
    sht40_error_count++;
    Serial.printf("SHT40 error (%d/3): %s\n", sht40_error_count, msg);
    if (sht40_error_count >= 3) {
      sht40_detected = false;
      Serial.println("SHT40: disabled after 3 consecutive errors (will not retry until reboot)");
    }
  } else {
    sht40_error_count = 0;
    g_sht40_temp = temp;
    g_sht40_hum  = hum;
  }
}

// ============================================================
// RS485 / Modbus RTU — Drain Sensor Polling Stub
// UART1 (Serial2): TX=GPIO4, RX=GPIO5, auto-direction transceiver
// Protocol: TBD (farmer sensor). Stub sends Modbus FC03 reg0x0000 to addr 0x01.
// ============================================================

// Modbus CRC16 (inline — no extra library)
uint16_t modbusCalcCRC(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      if (crc & 0x0001) crc = (crc >> 1) ^ 0xA001;
      else               crc >>= 1;
    }
  }
  return crc;
}

void initRS485() {
  Serial2.setTX(RS485_TX);
  Serial2.setRX(RS485_RX);
  Serial2.begin(rs485Baud);
  Serial.printf("RS485: UART1 TX=GPIO%d RX=GPIO%d baud=%d\n",
                RS485_TX, RS485_RX, rs485Baud);
}

// Poll drain sensor via Modbus RTU FC03 (stub — sensor model TBD).
// Silently skips on timeout or CRC error; never crashes or reboots.
void pollDrainSensor() {
  // Modbus request: addr=0x01, FC=0x03, reg=0x0000, count=0x0001
  uint8_t req[6];
  req[0] = 0x01;  // slave address
  req[1] = 0x03;  // function code: Read Holding Registers
  req[2] = 0x00;  // register high
  req[3] = 0x00;  // register low
  req[4] = 0x00;  // count high
  req[5] = 0x01;  // count low
  uint16_t crc = modbusCalcCRC(req, 6);
  uint8_t frame[8];
  memcpy(frame, req, 6);
  frame[6] = crc & 0xFF;
  frame[7] = (crc >> 8) & 0xFF;

  // Flush RX buffer before sending
  while (Serial2.available()) Serial2.read();
  Serial2.write(frame, 8);
  Serial2.flush();

  // Wait for response (timeout 500ms)
  // Expected: addr(1) + FC(1) + byte_count(1) + data(2) + CRC(2) = 7 bytes
  const int    RESP_LEN    = 7;
  const unsigned long TIMEOUT_MS = 500UL;
  uint8_t      resp[RESP_LEN];
  int          received    = 0;
  unsigned long deadline   = millis() + TIMEOUT_MS;

  while (received < RESP_LEN && millis() < deadline) {
    if (Serial2.available()) {
      resp[received++] = (uint8_t)Serial2.read();
    }
  }

  // No response → sensor not connected, skip silently
  if (received < RESP_LEN) return;

  // CRC check
  uint16_t rxCRC = (uint16_t)resp[5] | ((uint16_t)resp[6] << 8);
  if (modbusCalcCRC(resp, 5) != rxCRC) return;

  // Parse 16-bit value (big-endian in Modbus payload)
  uint16_t raw = ((uint16_t)resp[3] << 8) | resp[4];

  // Publish to MQTT
  if (!mqttClient.connected()) return;

  char buffer[96];
  snprintf(buffer, sizeof(buffer),
           "{\"raw\":%u,\"ts\":%lu}", raw, getCurrentEpoch());
  String topic = String("agriha/") + houseId + "/sensor/drain/state";
  mqttClient.publish(topic.c_str(), buffer);
  Serial.printf("Drain sensor raw=%u\n", raw);
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
        mqttClientId  = String("rp2350-") + nodeId;
        rs485Baud     = doc["rs485_baud"] | RS485_DEFAULT_BAUD;
        mdns_enabled  = doc["mdns_enabled"] | DEFAULT_MDNS_ENABLED;

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

void publishDIPulse() {
  noInterrupts();
  uint32_t c1 = diPulseCount[0]; diPulseCount[0] = 0;
  uint32_t c2 = diPulseCount[1]; diPulseCount[1] = 0;
  interrupts();

  JsonDocument doc;
  doc["count_di1"]    = c1;
  doc["count_di2"]    = c2;
  doc["interval_sec"] = SENSOR_INTERVAL;
  doc["ts"]           = getCurrentEpoch();

  char buffer[128];
  serializeJson(doc, buffer);

  String topic = String("agriha/") + houseId + "/di/pulse";
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
    doc["name"]     = RO_NAMES[ch - 1];
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
    doc["name"]    = DI_NAMES[di - 1];
    doc["stat_t"]  = diTopic;
    doc["val_tpl"] = String("{{ value_json.di") + di + " }}";
    doc["pl_on"]   = 1;
    doc["pl_off"]  = 0;
    // DI3-6: 窓リミットスイッチ → dev_cla="door"
    // DI1-2: 流量パルス / DI7-8: 予備 → dev_cla="power"
    doc["dev_cla"] = (di >= 3 && di <= 6) ? "door" : "power";
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

  // --- RS485 drain sensor (stub — always published for HA entity pre-registration) ---
  {
    String drainTopic = String("agriha/") + houseId + "/sensor/drain/state";
    String uid        = String(nodeId.c_str()) + "_drain";
    String topic      = String(PREFIX) + "/sensor/" + uid + "/config";

    JsonDocument doc;
    doc["name"]         = "排水センサー";
    doc["stat_t"]       = drainTopic;
    doc["val_tpl"]      = "{{ value_json.raw }}";
    doc["unit_of_meas"] = "";
    doc["ic"]           = "mdi:water-pump";
    doc["uniq_id"]      = uid;
    doc["dev"]          = deviceDoc;

    char buffer[512];
    serializeJson(doc, buffer);
    mqttClient.publish(topic.c_str(), buffer, true);
    Serial.printf("HA sensor/%s\n", uid.c_str());
    delay(100);
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
// WebUI — HTML ダッシュボード
// ============================================================
static const char HTML_PAGE[] = R"RELAY_HTML(
<!DOCTYPE html>
<html><head>
<meta charset=UTF-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>RP2350 Relay</title>
<style>
body{font-family:sans-serif;margin:16px;background:#1a1a2e;color:#e0e0e0}
h2{color:#4fc3f7;margin:0 0 10px}h3{color:#90caf9;margin:6px 0}
table{border-collapse:collapse;width:100%;margin:6px 0}
th,td{border:1px solid #37474f;padding:5px 8px}
th{background:#162447;color:#90caf9}
.on{color:#66bb6a;font-weight:bold}.off{color:#ef5350}
.bon{background:#43a047;color:#fff;border:none;padding:4px 8px;border-radius:3px;cursor:pointer}
.bof{background:#e53935;color:#fff;border:none;padding:4px 8px;border-radius:3px;cursor:pointer}
.sec{background:#162447;border-radius:6px;padding:12px;margin:8px 0}
input[type=number]{width:55px;padding:3px;background:#263238;color:#eee;border:1px solid #546e7a;border-radius:3px}
a{color:#90caf9}
</style>
</head><body>
<h2>RP2350 Relay Node</h2>
<div class=sec id=sys></div>
<div class=sec id=net></div>
<div class=sec id=devstat></div>
<div class=sec>
<h3>Relay Control / リレー制御</h3>
<table><tr><th>CH</th><th>Name</th><th>State</th><th>Control</th></tr>
<tbody id=rtbl></tbody></table>
</div>
<div class=sec>
<h3>Digital Input / デジタル入力</h3>
<table><tr><th>CH</th><th>Name</th><th>State</th></tr>
<tbody id=dtbl></tbody></table>
</div>
<div class=sec id=sens></div>
<script>
var RO=['側窓A開','側窓A閉','側窓B開','側窓B閉','電磁弁','循環扇1','循環扇2','予備'];
var DI=['灌水パルス1','灌水パルス2','側窓A開端','側窓A閉端','側窓B開端','側窓B閉端','予備DI7','予備DI8'];
function relay(ch,v,dur){
  var b={value:v};if(dur>0)b.duration_sec=dur;
  fetch('/api/relay/'+ch,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)}).then(load);
}
function load(){
  fetch('/api/state').then(function(r){return r.json();}).then(function(d){
    document.getElementById('sys').innerHTML=
      '<b>Node:</b> '+d.node_id+' | <b>House:</b> '+d.house_id+
      ' | <b>FW:</b> '+d.version+
      ' | <b>MQTT:</b> <span class="'+(d.mqtt_ok?'on':'off')+'">'+(d.mqtt_ok?'OK':'FAIL')+'</span>'+
      ' | <b>Uptime:</b> '+d.uptime+'s'+
      ' | <a href="/config">Config</a>';
    var mdnsHost=d.mdns_hostname?(' | <b>mDNS:</b> '+d.mdns_hostname):'';
    document.getElementById('net').innerHTML=
      '<h3>Network</h3>'+
      '<b>IP:</b> '+d.ip+
      ' | <b>Subnet:</b> '+d.subnet+
      ' | <b>GW:</b> '+d.gateway+
      ' | <b>DNS:</b> '+d.dns+
      mdnsHost+
      '<br><b>MAC:</b> '+d.mac+
      ' | <b>MQTT Broker:</b> '+d.mqtt_broker+':'+d.mqtt_port+
      ' | <b>IP Mode:</b> '+(d.static_ip?'Static':'DHCP');
    document.getElementById('devstat').innerHTML=
      '<h3>Device Status</h3>'+
      '<b>I2C Sensor:</b> '+(d.sht40_ok?'<span class=on>SHT40: detected</span>':'<span class=off>No sensors</span>')+
      ' | <b>RS485:</b> <span style="color:#ffa726">RS485: not configured</span>';
    var rt='';
    for(var i=1;i<=8;i++){
      var s=d.relay['ch'+i];
      rt+='<tr><td>'+i+'</td><td>'+RO[i-1]+'</td>'+
        '<td class="'+(s?'on':'off')+'">'+(s?'ON':'OFF')+'</td>'+
        '<td><button class=bon onclick="relay('+i+',1,0)">ON</button> '+
        '<button class=bof onclick="relay('+i+',0,0)">OFF</button> '+
        '<input id="d'+i+'" type=number value=30 min=1 max=3600>s '+
        '<button class=bon onclick="relay('+i+',1,+document.getElementById(\'d'+i+'\').value)">ON+T</button></td></tr>';
    }
    document.getElementById('rtbl').innerHTML=rt;
    var dt='';
    for(var i=1;i<=8;i++){
      var s=d.di['di'+i];
      dt+='<tr><td>'+i+'</td><td>'+DI[i-1]+'</td><td class="'+(s?'on':'off')+'">'+(s?'ON':'OFF')+'</td></tr>';
    }
    document.getElementById('dtbl').innerHTML=dt;
    var sv='<h3>Sensors / センサー</h3>';
    if(d.sensor.temp!==null)sv+='<b>温度:</b> '+d.sensor.temp.toFixed(1)+'°C &nbsp;';
    if(d.sensor.hum!==null)sv+='<b>湿度:</b> '+d.sensor.hum.toFixed(1)+'%';
    if(!d.sensor.temp&&!d.sensor.hum)sv+='<span class=off>センサーなし</span>';
    document.getElementById('sens').innerHTML=sv;
  }).catch(function(){
    document.getElementById('sys').innerHTML='<span class=off>通信エラー</span>';
  });
}
load();setInterval(load,5000);
</script>
</body></html>
)RELAY_HTML";

void sendDashboard(WiFiClient& client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=UTF-8");
  client.println("Connection: close");
  client.println();
  client.print(HTML_PAGE);
}

void sendAPIState(WiFiClient& client) {
  JsonDocument doc;

  JsonObject relay = doc["relay"].to<JsonObject>();
  for (int i = 1; i <= 8; i++) {
    relay[String("ch") + i] = (relayState >> (i - 1)) & 1;
  }

  JsonObject di = doc["di"].to<JsonObject>();
  for (int i = 0; i < 8; i++) {
    di[String("di") + (i + 1)] = diState[i] ? 1 : 0;
  }

  JsonObject sensor = doc["sensor"].to<JsonObject>();
  if (!isnan(g_sht40_temp)) sensor["temp"] = round(g_sht40_temp * 10) / 10.0;
  else                       sensor["temp"] = nullptr;
  if (!isnan(g_sht40_hum))  sensor["hum"]  = round(g_sht40_hum * 10) / 10.0;
  else                       sensor["hum"]  = nullptr;

  doc["node_id"]       = nodeId;
  doc["house_id"]      = houseId;
  doc["version"]       = FW_VERSION;
  doc["mqtt_ok"]       = mqttClient.connected();
  doc["uptime"]        = millis() / 1000;
  doc["ip"]            = eth.localIP().toString();
  doc["subnet"]        = eth.subnetMask().toString();
  doc["gateway"]       = eth.gatewayIP().toString();
  doc["dns"]           = eth.dnsIP().toString();
  doc["ts"]            = getCurrentEpoch();
  doc["sht40_ok"]      = sht40_detected;
  doc["mqtt_broker"]   = mqttBroker;
  doc["mqtt_port"]     = mqttPort;
  // static_ip: true if ip field was set in config (non-empty)
  {
    bool hasStatic = false;
    if (LittleFS.exists("/config.json")) {
      File f = LittleFS.open("/config.json", "r");
      if (f) {
        JsonDocument cfgDoc;
        if (!deserializeJson(cfgDoc, f)) {
          const char* ipField = cfgDoc["ip"] | "";
          hasStatic = (ipField[0] != '\0');
        }
        f.close();
      }
    }
    doc["static_ip"] = hasStatic;
  }
  // MAC address
  {
    uint8_t mac[6];
    eth.macAddress(mac);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    doc["mac"] = macStr;
  }
  // mDNS hostname
  if (mdns_enabled) {
    doc["mdns_hostname"] = nodeId + ".local";
  } else {
    doc["mdns_hostname"] = nullptr;
  }

  char buffer[1024];
  serializeJson(doc, buffer);

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Access-Control-Allow-Origin: *");
  client.println("Connection: close");
  client.println();
  client.print(buffer);
}

void handleRelayPost(WiFiClient& client, int ch, const String& body) {
  if (ch < 1 || ch > 8) {
    client.println("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    client.println("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n");
    return;
  }

  int value = doc["value"] | -1;
  int dur   = doc["duration_sec"] | 0;

  if (value == 1) {
    setRelay(ch, true);
    relayDurationEnd[ch - 1] = (dur > 0) ? millis() + (unsigned long)dur * 1000UL : 0;
  } else if (value == 0) {
    setRelay(ch, false);
    relayDurationEnd[ch - 1] = 0;
  }
  publishRelayState();

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Connection: close");
  client.println();
  client.printf("{\"ok\":true,\"ch\":%d}\n", ch);
}

// ============================================================
// GET /api/config — return current config as JSON
// ============================================================
void sendAPIConfig(WiFiClient& client) {
  JsonDocument doc;
  doc["house_id"]     = houseId;
  doc["node_id"]      = nodeId;
  doc["mqtt_broker"]  = mqttBroker;
  doc["mqtt_port"]    = mqttPort;
  doc["mdns_enabled"] = mdns_enabled;

  // Read IP fields from config.json if present
  if (LittleFS.exists("/config.json")) {
    File f = LittleFS.open("/config.json", "r");
    if (f) {
      JsonDocument cfgDoc;
      if (!deserializeJson(cfgDoc, f)) {
        doc["ip"]      = (const char*)(cfgDoc["ip"]      | "");
        doc["subnet"]  = (const char*)(cfgDoc["subnet"]  | DEFAULT_SUBNET);
        doc["gateway"] = (const char*)(cfgDoc["gateway"] | DEFAULT_GATEWAY);
        doc["dns"]     = (const char*)(cfgDoc["dns"]     | DEFAULT_DNS);
      }
      f.close();
    }
  } else {
    doc["ip"]      = "";
    doc["subnet"]  = DEFAULT_SUBNET;
    doc["gateway"] = DEFAULT_GATEWAY;
    doc["dns"]     = DEFAULT_DNS;
  }

  char buffer[512];
  serializeJson(doc, buffer);

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Access-Control-Allow-Origin: *");
  client.println("Connection: close");
  client.println();
  client.print(buffer);
}

// ============================================================
// GET /config — HTML config page
// ============================================================
void sendConfigPage(WiFiClient& client) {
  // Read current values to pre-fill the form
  String curIp, curSubnet, curGateway, curDns, curHouseId, curNodeId, curBroker;
  int    curPort = mqttPort;
  bool   curMdns = mdns_enabled;
  curHouseId = houseId;
  curNodeId  = nodeId;
  curBroker  = mqttBroker;
  curSubnet  = DEFAULT_SUBNET;
  curGateway = DEFAULT_GATEWAY;
  curDns     = DEFAULT_DNS;
  if (LittleFS.exists("/config.json")) {
    File f = LittleFS.open("/config.json", "r");
    if (f) {
      JsonDocument cfgDoc;
      if (!deserializeJson(cfgDoc, f)) {
        curIp      = (const char*)(cfgDoc["ip"]      | "");
        curSubnet  = (const char*)(cfgDoc["subnet"]  | DEFAULT_SUBNET);
        curGateway = (const char*)(cfgDoc["gateway"] | DEFAULT_GATEWAY);
        curDns     = (const char*)(cfgDoc["dns"]     | DEFAULT_DNS);
      }
      f.close();
    }
  }

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=UTF-8");
  client.println("Connection: close");
  client.println();
  client.println("<!DOCTYPE html><html><head>");
  client.println("<meta charset=UTF-8><meta name=viewport content='width=device-width,initial-scale=1'>");
  client.println("<title>Config - RP2350 Relay</title>");
  client.println("<style>body{font-family:sans-serif;margin:16px;background:#1a1a2e;color:#e0e0e0}");
  client.println("h2{color:#4fc3f7}h3{color:#90caf9}");
  client.println(".sec{background:#162447;border-radius:6px;padding:12px;margin:8px 0}");
  client.println("label{display:block;margin:6px 0 2px}");
  client.println("input[type=text],input[type=number]{width:220px;padding:4px;background:#263238;color:#eee;border:1px solid #546e7a;border-radius:3px}");
  client.println("input[type=submit]{background:#1976d2;color:#fff;border:none;padding:8px 20px;border-radius:4px;cursor:pointer;margin-top:10px}");
  client.println("a{color:#90caf9}.note{color:#90a4ae;font-size:0.85em}</style></head><body>");
  client.println("<h2>Network &amp; MQTT Configuration</h2>");
  client.printf("<p><a href='/'>&#8592; Dashboard</a> &nbsp; | &nbsp; Node: <b>%s</b></p>\n", nodeId.c_str());
  client.println("<form method=POST action=/api/config>");
  client.println("<div class=sec><h3>Identity</h3>");
  client.printf("<label>house_id<input type=text name=house_id value='%s'></label>\n", curHouseId.c_str());
  client.printf("<label>node_id<input type=text name=node_id value='%s'></label>\n", curNodeId.c_str());
  client.println("</div>");
  client.println("<div class=sec><h3>MQTT</h3>");
  client.printf("<label>broker IP<input type=text name=mqtt_broker value='%s'></label>\n", curBroker.c_str());
  client.printf("<label>port<input type=number name=mqtt_port min=1 max=65535 value='%d'></label>\n", curPort);
  client.println("</div>");
  client.println("<div class=sec><h3>IP Address</h3>");
  client.println("<p class=note>Leave IP blank for DHCP. Fill IP to use static address.</p>");
  client.printf("<label>IP (blank=DHCP)<input type=text name=ip value='%s' placeholder='e.g. 192.168.15.50'></label>\n", curIp.c_str());
  client.printf("<label>Subnet<input type=text name=subnet value='%s'></label>\n", curSubnet.c_str());
  client.printf("<label>Gateway<input type=text name=gateway value='%s'></label>\n", curGateway.c_str());
  client.printf("<label>DNS<input type=text name=dns value='%s'></label>\n", curDns.c_str());
  client.println("</div>");
  client.println("<div class=sec><h3>mDNS</h3>");
  client.printf("<label><input type=checkbox name=mdns_enabled value=1%s> Enable mDNS ({node_id}.local)</label>\n",
                curMdns ? " checked" : "");
  client.println("</div>");
  client.println("<input type=submit value='Save &amp; Reboot'>");
  client.println("</form>");
  client.println("<p class=note>Device will reboot after saving.</p>");
  client.println("</body></html>");
}

// ============================================================
// POST /api/config — save config.json and reboot
// Form-encoded body: house_id=...&node_id=...&mqtt_broker=...&mqtt_port=...
//                    &ip=...&subnet=...&gateway=...&dns=...&mdns_enabled=1
// ============================================================
void handleConfigPost(WiFiClient& client, const String& body) {
  // Simple URL-decode and form-field parse (no library needed for ASCII fields)
  auto getField = [&](const String& key) -> String {
    String search = key + "=";
    int idx = body.indexOf(search);
    if (idx < 0) return "";
    idx += search.length();
    int end = body.indexOf('&', idx);
    if (end < 0) end = body.length();
    String val = body.substring(idx, end);
    // URL-decode '+' → space and %XX
    val.replace('+', ' ');
    String decoded;
    for (int i = 0; i < (int)val.length(); i++) {
      if (val[i] == '%' && i + 2 < (int)val.length()) {
        char hex[3] = { val[i+1], val[i+2], '\0' };
        decoded += (char)strtol(hex, nullptr, 16);
        i += 2;
      } else {
        decoded += val[i];
      }
    }
    return decoded;
  };

  String newHouseId    = getField("house_id");
  String newNodeId     = getField("node_id");
  String newBroker     = getField("mqtt_broker");
  String newPortStr    = getField("mqtt_port");
  String newIp         = getField("ip");
  String newSubnet     = getField("subnet");
  String newGateway    = getField("gateway");
  String newDns        = getField("dns");
  String newMdnsStr    = getField("mdns_enabled");

  // Validate mandatory fields
  if (newHouseId.length() == 0) newHouseId = houseId;
  if (newNodeId.length() == 0)  newNodeId  = nodeId;
  if (newBroker.length() == 0)  newBroker  = mqttBroker;
  int newPort = (newPortStr.length() > 0) ? newPortStr.toInt() : mqttPort;
  if (newPort < 1 || newPort > 65535) newPort = mqttPort;
  if (newSubnet.length() == 0)  newSubnet  = DEFAULT_SUBNET;
  if (newGateway.length() == 0) newGateway = DEFAULT_GATEWAY;
  if (newDns.length() == 0)     newDns     = DEFAULT_DNS;
  bool newMdns = (newMdnsStr == "1");

  // Build and write config.json
  JsonDocument doc;
  doc["house_id"]     = newHouseId;
  doc["node_id"]      = newNodeId;
  doc["mqtt_broker"]  = newBroker;
  doc["mqtt_port"]    = newPort;
  doc["mdns_enabled"] = newMdns;
  if (newIp.length() > 0) {
    doc["ip"]      = newIp;
    doc["subnet"]  = newSubnet;
    doc["gateway"] = newGateway;
    doc["dns"]     = newDns;
  }
  // Preserve rs485_baud if already set
  if (LittleFS.exists("/config.json")) {
    File rf = LittleFS.open("/config.json", "r");
    if (rf) {
      JsonDocument old;
      if (!deserializeJson(old, rf) && !old["rs485_baud"].isNull()) {
        doc["rs485_baud"] = old["rs485_baud"];
      }
      rf.close();
    }
  }

  File f = LittleFS.open("/config.json", "w");
  if (!f) {
    client.println("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n");
    return;
  }
  serializeJson(doc, f);
  f.close();
  Serial.println("Config saved via WebUI");

  // Respond with redirect to dashboard (device will reboot shortly)
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=UTF-8");
  client.println("Connection: close");
  client.println();
  client.println("<!DOCTYPE html><html><head><meta charset=UTF-8></head><body>");
  client.println("<p>Config saved. Rebooting...</p>");
  client.println("</body></html>");
  client.flush();
  delay(500);
  rebootWithReason("config_saved_via_webui");
}

void handleWebClient() {
  WiFiClient client = webServer.accept();
  if (!client) return;

  client.setTimeout(500);
  unsigned long t = millis();

  // リクエスト行読み取り
  while (!client.available() && (millis() - t) < 500) delay(1);
  if (!client.available()) { client.stop(); return; }

  String reqLine = client.readStringUntil('\n');
  reqLine.trim();

  // メソッド + パス解析
  int sp1 = reqLine.indexOf(' ');
  int sp2 = (sp1 >= 0) ? reqLine.indexOf(' ', sp1 + 1) : -1;
  if (sp1 < 0 || sp2 < 0) { client.stop(); return; }

  String method = reqLine.substring(0, sp1);
  String path   = reqLine.substring(sp1 + 1, sp2);

  // ヘッダー読み取り (Content-Length を取得)
  int contentLength = 0;
  while ((millis() - t) < 2000) {
    String hdr = client.readStringUntil('\n');
    hdr.trim();
    if (hdr.length() == 0) break;
    if (hdr.startsWith("Content-Length:")) {
      contentLength = hdr.substring(15).toInt();
    }
  }

  // ボディ読み取り
  String body;
  if (contentLength > 0) {
    unsigned long bt = millis();
    while ((int)body.length() < contentLength && (millis() - bt) < 1000) {
      if (client.available()) body += (char)client.read();
    }
  }

  // ルーティング
  if (method == "GET" && (path == "/" || path == "/index.html")) {
    sendDashboard(client);
  } else if (method == "GET" && path == "/api/state") {
    sendAPIState(client);
  } else if (method == "GET" && path == "/api/config") {
    sendAPIConfig(client);
  } else if (method == "GET" && path == "/config") {
    sendConfigPage(client);
  } else if (method == "POST" && path == "/api/config") {
    handleConfigPost(client, body);
  } else if (method == "POST" && path.startsWith("/api/relay/")) {
    int ch = path.substring(11).toInt();
    handleRelayPost(client, ch, body);
  } else {
    client.println("HTTP/1.1 404 Not Found\r\nConnection: close\r\n");
  }

  delay(1);
  client.stop();
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
  }
  // DI1-2: FALLING edge パルスカウント (流量計)
  attachInterrupt(digitalPinToInterrupt(DI_PINS[0]), diPulseISR1, FALLING);
  attachInterrupt(digitalPinToInterrupt(DI_PINS[1]), diPulseISR2, FALLING);
  // DI3-8: CHANGE (リミットスイッチ・状態監視)
  for (int i = 2; i < 8; i++) {
    attachInterrupt(digitalPinToInterrupt(DI_PINS[i]), diISR, CHANGE);
  }
  Serial.println("DI: GPIO9-10 pulse(FALLING), GPIO11-16 state(CHANGE)");

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

  // --- RS485 UART1 初期化 ---
  initRS485();

  // --- MQTT ---
  mqttClient.setServer(mqttBroker.c_str(), mqttPort);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(60);
  mqttClient.setBufferSize(1024);
  connectMQTT();

  // --- mDNS ({node_id}.local) ---
  if (mdns_enabled) {
    if (MDNS.begin(nodeId.c_str())) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("mDNS: %s.local\n", nodeId.c_str());
    } else {
      Serial.println("mDNS: begin failed (continuing without mDNS)");
    }
  } else {
    Serial.println("mDNS: disabled by config");
  }

  // --- HTTP WebUI (port 80) ---
  webServer.begin();
  Serial.println("WebUI: http://");
  Serial.println(eth.localIP().toString());

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

  // mDNS update
  if (mdns_enabled) MDNS.update();

  // HTTP WebUI クライアント処理
  handleWebClient();

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
    pollDrainSensor();

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
    publishDIPulse();
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
