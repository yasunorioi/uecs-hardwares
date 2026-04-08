// standalone_rp2350_relay.ino - Waveshare 8ch Relay + DI Node (RP2350B) — Standalone Mode
// Board: RP2350-POE-ETH-8DI-8RO
// Relay: GPIO17-24 直接制御
// DI:    GPIO9-16, フォトカプラ絶縁, アクティブLOW, 割り込み検知
// RTC:   PCF85063 (I2C0: SDA=GPIO6, SCL=GPIO7)
// Comm:  W5500 SPI1 (CS=GPIO33, RST=GPIO25, SCK=GPIO34, MOSI=GPIO35, MISO=GPIO36)
// RS485: UART1 TX=GPIO4, RX=GPIO5 (DFRobot SEN0575 rain sensor)
//
// No MQTT. Rule engine + scheduler + event log + WebUI.
//
// Libraries: arduino-pico 4.5.2+, ArduinoJson 7.x, NTPClient 3.2.1,
//            W5500lwIP (arduino-pico内蔵), SensirionI2cSht4x, LEAmDNS

#include <SPI.h>
#include <W5500lwIP.h>
#include <ArduinoJson.h>        // v7.x
#include <LittleFS.h>
#include <Wire.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <SensirionI2cSht4x.h>
#include <LEAmDNS.h>
#include <math.h>

#include "sw_watchdog.h"
#include "sensor_registry.h"
#include "rule_engine.h"
#include "scheduler.h"
#include "event_log.h"
#include "web_ui.h"

// ========== Firmware Version ==========
const char* FW_VERSION = "1.0.0";
const char* FW_NAME    = "standalone_rp2350_relay";

// ========== Default Configuration ==========
const char* DEFAULT_NODE_ID      = "relay_01";
const char* DEFAULT_NODE_NAME    = "\xe6\xb8\xa9\xe5\xae\xa4\xe5\x88\xb6\xe5\xbe\xa1\xe3\x83\x9c\xe3\x83\x83\xe3\x82\xaf\xe3\x82\xb9"; // "温室制御ボックス" UTF-8
const char* DEFAULT_IP           = "";
const char* DEFAULT_SUBNET       = "255.255.255.0";
const char* DEFAULT_GATEWAY      = "192.168.1.1";
const char* DEFAULT_DNS          = "8.8.8.8";
const bool  DEFAULT_MDNS_ENABLED   = true;
const char* DEFAULT_MDNS_HOSTNAME = "uecs-relay-01";
const int   DEFAULT_MANUAL_TIMEOUT_MIN = 60;
const char* DEFAULT_NTP_SERVER    = "pool.ntp.org";

// ========== W5500 SPI1 Pins ==========
const int W5500_CS   = 33;
const int W5500_RST  = 25;
const int W5500_SCK  = 34;
const int W5500_MOSI = 35;
const int W5500_MISO = 36;
const int W5500_INT  = -1;

// ========== RS485 UART1 Pins ==========
const int RS485_TX = 4;
const int RS485_RX = 5;
const int RS485_DEFAULT_BAUD = 9600;

// ========== I2C0 Pins ==========
const int I2C_SDA = 6;
const int I2C_SCL = 7;

// ========== PCF85063 RTC ==========
const uint8_t PCF85063_ADDR = 0x51;

// ========== Relay GPIO Pins (GPIO17-24) ==========
const int RELAY_PINS[8] = {17, 18, 19, 20, 21, 22, 23, 24};

// ========== DI GPIO Pins (GPIO9-16, アクティブLOW) ==========
const int DI_PINS[8] = {9, 10, 11, 12, 13, 14, 15, 16};

// ========== Channel Names (generic defaults) ==========
const char* RO_NAMES[8] = {
  "\xe3\x83\xaa\xe3\x83\xac\xe3\x83\xbc1",  // "リレー1"
  "\xe3\x83\xaa\xe3\x83\xac\xe3\x83\xbc2",
  "\xe3\x83\xaa\xe3\x83\xac\xe3\x83\xbc3",
  "\xe3\x83\xaa\xe3\x83\xac\xe3\x83\xbc4",
  "\xe3\x83\xaa\xe3\x83\xac\xe3\x83\xbc5",
  "\xe3\x83\xaa\xe3\x83\xac\xe3\x83\xbc6",
  "\xe3\x83\xaa\xe3\x83\xac\xe3\x83\xbc7",
  "\xe3\x83\xaa\xe3\x83\xac\xe3\x83\xbc8"
};
const char* DI_NAMES[8] = {
  "\xe5\x85\xa5\xe5\x8a\x981",  // "入力1"
  "\xe5\x85\xa5\xe5\x8a\x982",
  "\xe5\x85\xa5\xe5\x8a\x983",
  "\xe5\x85\xa5\xe5\x8a\x984",
  "\xe5\x85\xa5\xe5\x8a\x985",
  "\xe5\x85\xa5\xe5\x8a\x986",
  "\xe5\x85\xa5\xe5\x8a\x987",
  "\xe5\x85\xa5\xe5\x8a\x988"
};

// ========== Timing ==========
const unsigned long SENSOR_INTERVAL     = 10000UL;    // 10 seconds
const unsigned long REBOOT_INTERVAL     = 86400000UL; // 24 hours
const unsigned long NTP_SYNC_INTERVAL   = 3600000UL;  // 1 hour
const unsigned long LOG_FLUSH_INTERVAL  = 300000UL;   // 5 minutes
const int           ETH_CONNECT_TIMEOUT = 15;          // seconds

// ========== HW WDT ==========
const int HW_WDT_TIMEOUT_MS = 8000;

// ========== Manual Override ==========
// Default 1 hour; overridden by config manual_timeout_min
unsigned long MANUAL_TIMEOUT = 3600000UL;

// ========== Control Source ==========
enum ControlSource : uint8_t {
  SRC_NONE     = 0,
  SRC_MANUAL   = 1,
  SRC_RULE     = 2,
  SRC_SCHEDULE = 3
};

struct ChannelCtrl {
  ControlSource source;
  uint8_t       sourceId;
  unsigned long manualExpiry;  // millis() when manual override expires, 0=none
};

// ========== Global State ==========
uint8_t       relayState           = 0x00;
unsigned long relayDurationEnd[8]  = {0};
ChannelCtrl   g_chCtrl[8]          = {};

bool diState[8]     = {false};
bool diPrevState[8] = {false};
volatile bool     diInterruptFlag  = false;
volatile uint32_t diPulseCount[2]  = {0, 0};
unsigned long diLastDebounce  = 0;
const unsigned long DI_DEBOUNCE_MS = 50;

bool  sht40_detected    = false;
int   sht40_error_count = 0;
float g_sht40_temp      = NAN;
float g_sht40_hum       = NAN;
float g_vpd             = NAN;

bool mdns_enabled = DEFAULT_MDNS_ENABLED;

unsigned long ntpEpoch  = 0;
unsigned long ntpMillis = 0;

int loopCount = 0;
unsigned long last_status = 0;  // [STATUS] 30秒タイマー

// ========== Rule Engine / Scheduler / Log ==========
Rule     g_rules[MAX_RULES];
int      g_ruleCount = 0;
Schedule g_schedules[MAX_SCHEDULES];
int      g_schedCount = 0;
EventLog g_log;

// ========== RS485 SEN0575 ==========
const uint8_t  SEN0575_ADDR          = 0xC0;
const uint16_t SEN0575_REG_CUMRAIN_H = 0x0007;
const uint16_t SEN0575_REG_RAWDATA_H = 0x0009;
const uint16_t SEN0575_REG_SYSTIME   = 0x000B;
const uint16_t SEN0575_REG_PID_H     = 0x0000;

bool     sen0575_detected     = false;
uint32_t sen0575_cumRainRaw   = 0;
uint32_t sen0575_rawTips      = 0;
uint16_t sen0575_workTimeMins = 0;

// ========== Objects ==========
Wiznet5500lwIP    eth(W5500_CS, SPI1, W5500_INT);
WiFiUDP           ntpUDP;
NTPClient         timeClient(ntpUDP, "pool.ntp.org", 0);
SensirionI2cSht4x sht4x;
WiFiServer        webServer(80);

String nodeId;
String nodeName;
String ntpServer;
String mdnsHostname;
int    rs485Baud = RS485_DEFAULT_BAUD;

// ========== Function Declarations ==========
void loadConfig();
void initEthernet();
void setRelay(uint8_t ch, bool on, ControlSource src = SRC_MANUAL, uint8_t srcId = 0);
void initRelaysOff();
bool readDI();
void scanI2CSensors();
void readSensors();
void calculateVPD();
void syncNTP();
bool rtcGetTime(struct tm* t);
bool rtcSetTime(struct tm* t);
unsigned long getCurrentEpoch();
void rebootWithReason(const char* reason);
void initRS485();
void pollDrainSensor();
uint16_t modbusCalcCRC(const uint8_t* data, size_t len);
bool modbusReadInput(uint8_t addr, uint16_t reg, uint16_t count,
                     uint16_t* out1, uint16_t* out2);
void resolveControl(uint8_t ruleReq[8], uint8_t ruleSrc[8],
                    uint8_t schedReq[8], uint8_t schedSrc[8]);
void checkManualExpiry();
void handleWebClient();
void sendConfigPageWrapper(WiFiClient& client);
void sendAPIStateWrapper(WiFiClient& client);
void sendAPIRulesWrapper(WiFiClient& client);
void sendAPISchedulesWrapper(WiFiClient& client);
void sendAPIConfig(WiFiClient& client);
void sendAPILogWrapper(WiFiClient& client);
void handleRelayPost(WiFiClient& client, int ch, const String& body);
void handleRulesPost(WiFiClient& client, const String& body);
void handleRulesDeletePost(WiFiClient& client, const String& body);
void handleSchedulesPost(WiFiClient& client, const String& body);
void handleSchedulesDeletePost(WiFiClient& client, const String& body);
void handleConfigPost(WiFiClient& client, const String& body);

// ============================================================
// DI Interrupt
// ============================================================
void diPulseISR1() { diPulseCount[0]++; }
void diPulseISR2() { diPulseCount[1]++; }
void diISR()       { diInterruptFlag = true; }

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
  t->tm_sec  = bcd2dec(Wire.read() & 0x7F);
  t->tm_min  = bcd2dec(Wire.read() & 0x7F);
  t->tm_hour = bcd2dec(Wire.read() & 0x3F);
  t->tm_mday = bcd2dec(Wire.read() & 0x3F);
  Wire.read();  // weekday skip
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

unsigned long getCurrentEpoch() {
  if (ntpEpoch == 0) return 0;
  return ntpEpoch + (millis() - ntpMillis) / 1000;
}

// ============================================================
// NTP Sync → ntpEpoch + PCF85063 RTC更新
// ============================================================
void syncNTP() {
  timeClient.setPoolServerName(ntpServer.c_str());
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
  logAdd(g_log, ntpEpoch, EVT_SYSTEM, 0, 0, 0, "NTP sync OK");

  // Write time to PCF85063 (hms only — full date write requires more decomposition)
  unsigned long e = ntpEpoch;
  struct tm t;
  t.tm_sec  = e % 60; e /= 60;
  t.tm_min  = e % 60; e /= 60;
  t.tm_hour = e % 24;
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
  memset(g_chCtrl, 0, sizeof(g_chCtrl));
  Serial.println("All relays OFF (safe startup state)");
}

void setRelay(uint8_t ch, bool on, ControlSource src, uint8_t srcId) {
  if (ch < 1 || ch > 8) return;
  uint8_t idx = ch - 1;
  bool prev = (relayState >> idx) & 1;

  digitalWrite(RELAY_PINS[idx], on ? HIGH : LOW);
  if (on) relayState |=  (1 << idx);
  else    relayState &= ~(1 << idx);

  g_chCtrl[idx].source   = src;
  g_chCtrl[idx].sourceId = srcId;

  Serial.printf("Relay CH%d %s src=%d srcId=%d (state=0x%02X)\n",
                ch, on ? "ON" : "OFF", (int)src, (int)srcId, relayState);

  // Log relay change
  if (prev != on) {
    char msg[48];
    snprintf(msg, sizeof(msg), "CH%d %s src=%d", ch, on ? "ON" : "OFF", (int)src);
    logAdd(g_log, getCurrentEpoch(), EVT_RELAY, ch, on ? 1 : 0, srcId, msg);
  }
}

// ============================================================
// Manual Override Expiry Check
// ============================================================
void checkManualExpiry() {
  unsigned long now = millis();
  for (int i = 0; i < 8; i++) {
    if (g_chCtrl[i].source == SRC_MANUAL &&
        g_chCtrl[i].manualExpiry > 0 &&
        now >= g_chCtrl[i].manualExpiry) {
      Serial.printf("Manual override CH%d expired\n", i + 1);
      g_chCtrl[i].source      = SRC_NONE;
      g_chCtrl[i].manualExpiry = 0;
    }
  }
}

// ============================================================
// Control Resolution: rule + schedule → relay
// Priority: MANUAL > RULE > SCHEDULE
// 0xFF = no opinion → relay stays in current state
// ============================================================
void resolveControl(uint8_t ruleReq[8], uint8_t ruleSrc[8],
                    uint8_t schedReq[8], uint8_t schedSrc[8]) {
  for (int i = 0; i < 8; i++) {
    // Manual override wins if active (not expired)
    if (g_chCtrl[i].source == SRC_MANUAL && g_chCtrl[i].manualExpiry == 0) {
      // permanent manual — skip automated control
      continue;
    }
    if (g_chCtrl[i].source == SRC_MANUAL && g_chCtrl[i].manualExpiry > 0) {
      // timed manual — not yet expired (expiry checked in checkManualExpiry)
      continue;
    }

    // Rule next
    if (ruleReq[i] != 0xFF) {
      bool desired = (ruleReq[i] == 1);
      bool current = (relayState >> i) & 1;
      if (desired != current) {
        setRelay(i + 1, desired, SRC_RULE, ruleSrc[i]);
      } else {
        g_chCtrl[i].source   = SRC_RULE;
        g_chCtrl[i].sourceId = ruleSrc[i];
      }
      continue;
    }

    // Schedule last
    if (schedReq[i] != 0xFF) {
      bool desired = (schedReq[i] == 1);
      bool current = (relayState >> i) & 1;
      if (desired != current) {
        setRelay(i + 1, desired, SRC_SCHEDULE, schedSrc[i]);
      } else {
        g_chCtrl[i].source   = SRC_SCHEDULE;
        g_chCtrl[i].sourceId = schedSrc[i];
      }
      continue;
    }
    // No opinion: relay stays as-is
  }
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
  Wire.setTimeout(10);
  Serial.println("I2C scan:");
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    if (addr == PCF85063_ADDR) continue;
    Wire.beginTransmission(addr);
    uint8_t rc = Wire.endTransmission();
    if (rc == 0) {
      found++;
      Serial.printf("  0x%02X -> ", addr);
      bool matched = false;
      for (int i = 0; i < SENSOR_REGISTRY_SIZE; i++) {
        if (SENSOR_REGISTRY[i].addr == addr) {
          Serial.printf("%s\n", SENSOR_REGISTRY[i].name);
          if (SENSOR_REGISTRY[i].type == I2C_SENSOR_SHT40) sht40_detected = true;
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
      g_sht40_temp = NAN;
      g_sht40_hum  = NAN;
      Serial.println("SHT40: disabled after 3 consecutive errors");
    }
  } else {
    sht40_error_count = 0;
    g_sht40_temp = temp;
    g_sht40_hum  = hum;
    calculateVPD();
  }
}

// ============================================================
// VPD Calculation (kPa)
// ============================================================
void calculateVPD() {
  if (isnan(g_sht40_temp) || isnan(g_sht40_hum)) {
    g_vpd = NAN;
    return;
  }
  float svp = 0.6108f * expf(17.27f * g_sht40_temp / (g_sht40_temp + 237.3f));
  g_vpd = (1.0f - g_sht40_hum / 100.0f) * svp;
}

// ============================================================
// RS485 Modbus RTU — SEN0575
// ============================================================
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

bool modbusReadInput(uint8_t addr, uint16_t reg, uint16_t count,
                     uint16_t* out1, uint16_t* out2) {
  uint8_t req[6];
  req[0] = addr;
  req[1] = 0x04;
  req[2] = (reg >> 8) & 0xFF;
  req[3] = reg & 0xFF;
  req[4] = (count >> 8) & 0xFF;
  req[5] = count & 0xFF;
  uint16_t crc = modbusCalcCRC(req, 6);
  uint8_t frame[8];
  memcpy(frame, req, 6);
  frame[6] = crc & 0xFF;
  frame[7] = (crc >> 8) & 0xFF;

  while (Serial2.available()) Serial2.read();
  Serial2.write(frame, 8);
  Serial2.flush();

  const int respLen = 3 + count * 2 + 2;
  uint8_t resp[11];
  if (respLen > (int)sizeof(resp)) return false;

  int received = 0;
  unsigned long deadline = millis() + 1000UL;
  while (received < respLen && millis() < deadline) {
    if (Serial2.available()) resp[received++] = (uint8_t)Serial2.read();
  }
  if (received < respLen) return false;

  uint16_t rxCRC = (uint16_t)resp[respLen - 2] | ((uint16_t)resp[respLen - 1] << 8);
  if (modbusCalcCRC(resp, respLen - 2) != rxCRC) return false;
  if (resp[0] != addr || resp[1] != 0x04 || resp[2] != count * 2) return false;

  *out1 = ((uint16_t)resp[3] << 8) | resp[4];
  if (count >= 2 && out2) *out2 = ((uint16_t)resp[5] << 8) | resp[6];
  return true;
}

void initRS485() {
  Serial2.setTX(RS485_TX);
  Serial2.setRX(RS485_RX);
  Serial2.begin(rs485Baud);
  Serial.printf("RS485: UART1 TX=GPIO%d RX=GPIO%d baud=%d\n",
                RS485_TX, RS485_RX, rs485Baud);
  delay(100);
  uint16_t pidH = 0, pidL = 0;
  if (modbusReadInput(SEN0575_ADDR, SEN0575_REG_PID_H, 2, &pidH, &pidL)) {
    uint32_t pid = ((uint32_t)pidH << 16) | pidL;
    sen0575_detected = (pid == 0x000100C0);
    Serial.printf("SEN0575: PID=0x%08lX %s\n", pid,
                  sen0575_detected ? "DETECTED" : "PID mismatch");
  } else {
    Serial.println("SEN0575: not found (no response on RS485)");
  }
}

void pollDrainSensor() {
  if (!sen0575_detected) return;

  uint16_t cumH = 0, cumL = 0;
  if (modbusReadInput(SEN0575_ADDR, SEN0575_REG_CUMRAIN_H, 2, &cumH, &cumL)) {
    sen0575_cumRainRaw = ((uint32_t)cumH << 16) | cumL;
  }
  delay(50);

  uint16_t rawH = 0, rawL = 0;
  if (modbusReadInput(SEN0575_ADDR, SEN0575_REG_RAWDATA_H, 2, &rawH, &rawL)) {
    sen0575_rawTips = ((uint32_t)rawH << 16) | rawL;
  }
  delay(50);

  uint16_t wt = 0;
  if (modbusReadInput(SEN0575_ADDR, SEN0575_REG_SYSTIME, 1, &wt, nullptr)) {
    sen0575_workTimeMins = wt;
  }

  float rain_mm   = sen0575_cumRainRaw / 10000.0f;
  float workHours = sen0575_workTimeMins / 60.0f;
  Serial.printf("SEN0575: rain=%.2fmm tips=%lu work=%.1fh\n",
                rain_mm, (unsigned long)sen0575_rawTips, workHours);
}

// ============================================================
// Configuration (LittleFS, no MQTT fields)
// ============================================================
void loadConfig() {
  if (LittleFS.exists("/config.json")) {
    File file = LittleFS.open("/config.json", "r");
    if (file) {
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, file);
      file.close();

      if (!err) {
        Serial.println("Config loaded from /config.json");
        nodeId       = (const char*)(doc["node_id"]        | DEFAULT_NODE_ID);
        nodeName     = (const char*)(doc["node_name"]      | DEFAULT_NODE_NAME);
        mdnsHostname = (const char*)(doc["mdns_hostname"]  | DEFAULT_MDNS_HOSTNAME);
        rs485Baud    = doc["rs485_baud"]    | RS485_DEFAULT_BAUD;
        mdns_enabled = doc["mdns_enabled"]  | DEFAULT_MDNS_ENABLED;
        ntpServer    = (const char*)(doc["ntp_server"] | DEFAULT_NTP_SERVER);

        int manualMin = doc["manual_timeout_min"] | DEFAULT_MANUAL_TIMEOUT_MIN;
        MANUAL_TIMEOUT = (unsigned long)manualMin * 60000UL;

        String ipStr = (const char*)(doc["ip"] | "");
        if (ipStr.length() > 0) {
          IPAddress ip, subnet, gw, dns;
          ip.fromString(ipStr);
          subnet.fromString((const char*)(doc["subnet"]  | DEFAULT_SUBNET));
          gw.fromString((const char*)(doc["gateway"]     | DEFAULT_GATEWAY));
          dns.fromString((const char*)(doc["dns"]        | DEFAULT_DNS));
          eth.config(ip, gw, subnet, dns);
          Serial.printf("Static IP: %s\n", ipStr.c_str());
        }
        return;
      }
      Serial.printf("Config parse error: %s\n", err.c_str());
    }
  }
  Serial.println("Using default configuration (DHCP)");
  nodeId       = DEFAULT_NODE_ID;
  nodeName     = DEFAULT_NODE_NAME;
  mdnsHostname = DEFAULT_MDNS_HOSTNAME;
  ntpServer    = DEFAULT_NTP_SERVER;
}

// ============================================================
// Ethernet (W5500 SPI1)
// ============================================================
void initEthernet() {
  pinMode(W5500_RST, OUTPUT);
  digitalWrite(W5500_RST, LOW);
  delay(100);
  digitalWrite(W5500_RST, HIGH);
  delay(500);

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
      Serial.println("[ERR] DHCP timeout, retrying...");
      rebootWithReason("eth_dhcp_timeout");
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  {
    uint8_t mac[6];
    eth.macAddress(mac);
    char mac_str[20];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.printf("[BOOT] mac: %s\n", mac_str);
  }
  Serial.printf("[NET] ip: %s gw: %s mask: %s\n",
                eth.localIP().toString().c_str(),
                eth.gatewayIP().toString().c_str(),
                eth.subnetMask().toString().c_str());
}

// ============================================================
// Reboot (Pico SDK watchdog)
// ============================================================
void rebootWithReason(const char* reason) {
  Serial.printf("Rebooting: %s\n", reason);
  logAdd(g_log, getCurrentEpoch(), EVT_SYSTEM, 0, 0, 0, reason);
  logFlush(g_log);

  File file = LittleFS.open("/reboot_reason.txt", "w");
  if (file) { file.print(reason); file.close(); }

  delay(500);
  watchdog_reboot(0, 0, 0);
  while (true) {}
}

// ============================================================
// WebUI wrappers — delegate to web_ui.h with globals
// ============================================================

// sendDashboard, sendRulesPage, sendSchedulePage, sendLogPage
// are static inline in web_ui.h taking only WiFiClient& — used directly.

void sendConfigPageWrapper(WiFiClient& client) {
  String curIp = "", curSubnet = DEFAULT_SUBNET, curGateway = DEFAULT_GATEWAY;
  String curDns = DEFAULT_DNS;
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
  sendConfigPage(client, nodeId.c_str(), nodeName.c_str(),
                 curIp.c_str(), curSubnet.c_str(), curGateway.c_str(), curDns.c_str(),
                 mdns_enabled, (uint32_t)rs485Baud, (uint16_t)(MANUAL_TIMEOUT / 1000UL),
                 ntpServer.c_str(), mdnsHostname.c_str());
}

void sendAPIStateWrapper(WiFiClient& client) {
  uint8_t chCtrl[8];
  uint8_t chSrcId[8];
  const char* chSrcName[8];
  for (int i = 0; i < 8; i++) {
    chCtrl[i]  = (uint8_t)g_chCtrl[i].source;
    chSrcId[i] = g_chCtrl[i].sourceId;
    chSrcName[i] = "";
    if (g_chCtrl[i].source == SRC_RULE) {
      for (int r = 0; r < g_ruleCount; r++) {
        if (g_rules[r].id == g_chCtrl[i].sourceId) { chSrcName[i] = g_rules[r].name; break; }
      }
    } else if (g_chCtrl[i].source == SRC_SCHEDULE) {
      int schedId = g_chCtrl[i].sourceId - 100;
      for (int s = 0; s < g_schedCount; s++) {
        if (g_schedules[s].id == schedId) { chSrcName[i] = g_schedules[s].name; break; }
      }
    }
  }
  sendAPIState(client, relayState, g_sht40_temp, g_sht40_hum, g_vpd,
               (float)(sen0575_cumRainRaw * 0.1f), diState,
               g_rules, g_ruleCount, g_schedules, g_schedCount,
               chCtrl, chSrcId, chSrcName,
               getCurrentEpoch(), ntpEpoch > 0, FW_VERSION, nodeId.c_str(), nodeName.c_str());
}

void sendAPIRulesWrapper(WiFiClient& client) {
  sendAPIRules(client, g_rules, g_ruleCount);
}

void sendAPISchedulesWrapper(WiFiClient& client) {
  sendAPISchedules(client, g_schedules, g_schedCount);
}

void sendAPILogWrapper(WiFiClient& client) {
  sendAPILog(client, g_log);
}

// GET /api/config — not in web_ui.h, stays in .ino
void sendAPIConfig(WiFiClient& client) {
  JsonDocument doc;
  doc["node_id"]            = nodeId;
  doc["node_name"]          = nodeName;
  doc["mdns_hostname"]      = mdnsHostname;
  doc["ntp_server"]         = ntpServer;
  doc["mdns"]               = mdns_enabled;
  doc["rs485_baud"]         = rs485Baud;
  doc["manual_timeout_min"] = (int)(MANUAL_TIMEOUT / 60000UL);

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

  char out[512];
  serializeJson(doc, out);
  sendJsonHeader(client);
  client.print(out);
}

// ============================================================
// POST /api/config — save config.json and reboot
// ============================================================
void handleConfigPost(WiFiClient& client, const String& body) {
  auto getField = [&](const String& key) -> String {
    String search = key + "=";
    int idx = body.indexOf(search);
    if (idx < 0) return "";
    idx += search.length();
    int end = body.indexOf('&', idx);
    if (end < 0) end = body.length();
    String val = body.substring(idx, end);
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

  String newNodeId       = getField("node_id");
  String newNodeName     = getField("node_name");
  String newMdnsHostname = getField("mdns_hostname");
  String newIp           = getField("ip");
  String newSubnet       = getField("subnet");
  String newGateway      = getField("gateway");
  String newDns          = getField("dns");
  String newNtpServer    = getField("ntp_server");
  String newMdnsStr      = getField("mdns_enabled");
  String newManualStr    = getField("manual_timeout_min");

  if (newNodeId.length() == 0)       newNodeId       = nodeId;
  if (newNodeName.length() == 0)     newNodeName     = nodeName;
  if (newMdnsHostname.length() == 0) newMdnsHostname = mdnsHostname;
  if (newNtpServer.length() == 0)    newNtpServer    = ntpServer;
  if (newSubnet.length() == 0)       newSubnet       = DEFAULT_SUBNET;
  if (newGateway.length() == 0)      newGateway      = DEFAULT_GATEWAY;
  if (newDns.length() == 0)          newDns          = DEFAULT_DNS;
  bool newMdns   = (newMdnsStr == "1");
  int  newManual = (newManualStr.length() > 0) ? newManualStr.toInt()
                                               : (int)(MANUAL_TIMEOUT / 60000UL);
  if (newManual < 1 || newManual > 1440) newManual = DEFAULT_MANUAL_TIMEOUT_MIN;

  JsonDocument doc;
  doc["node_id"]            = newNodeId;
  doc["node_name"]          = newNodeName;
  doc["mdns_hostname"]      = newMdnsHostname;
  doc["ntp_server"]         = newNtpServer;
  doc["mdns_enabled"]       = newMdns;
  doc["manual_timeout_min"] = newManual;

  // Preserve rs485_baud
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

  if (newIp.length() > 0) {
    doc["ip"]      = newIp;
    doc["subnet"]  = newSubnet;
    doc["gateway"] = newGateway;
    doc["dns"]     = newDns;
  }

  File f = LittleFS.open("/config.json", "w");
  if (!f) {
    client.println("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n");
    return;
  }
  serializeJson(doc, f);
  f.close();
  Serial.println("Config saved via WebUI");
  logAdd(g_log, getCurrentEpoch(), EVT_SYSTEM, 0, 0, 0, "config saved via WebUI");

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=UTF-8");
  client.println("Connection: close");
  client.println();
  client.println("<!DOCTYPE html><html><head><meta charset=UTF-8></head><body>"
                 "<p>Config saved. Rebooting...</p></body></html>");
  client.flush();
  delay(500);
  rebootWithReason("config_saved_via_webui");
}

// ============================================================
// POST /api/relay/{ch} — manual relay control
// Body: {"value":1,"duration_sec":30}
// ============================================================
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
    setRelay(ch, true, SRC_MANUAL, 0);
    if (dur > 0) {
      g_chCtrl[ch - 1].manualExpiry = millis() + (unsigned long)dur * 1000UL;
    } else {
      g_chCtrl[ch - 1].manualExpiry = millis() + MANUAL_TIMEOUT;
    }
    relayDurationEnd[ch - 1] = g_chCtrl[ch - 1].manualExpiry;
  } else if (value == 0) {
    setRelay(ch, false, SRC_MANUAL, 0);
    g_chCtrl[ch - 1].manualExpiry = 0;
    relayDurationEnd[ch - 1]      = 0;
  } else {
    client.println("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n");
    return;
  }

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Connection: close");
  client.println();
  client.printf("{\"ok\":true,\"ch\":%d,\"value\":%d}\n", ch, value);
}

// ============================================================
// POST /api/rules — upsert a rule (JSON body = Rule fields)
// ============================================================
void handleRulesPost(WiFiClient& client, const String& body) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    client.println("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n");
    return;
  }

  // Find existing rule by id, or add new
  uint8_t rid = doc["id"] | (uint8_t)0xFF;
  int idx = -1;
  for (int i = 0; i < g_ruleCount; i++) {
    if (g_rules[i].id == rid) { idx = i; break; }
  }
  if (idx < 0) {
    if (g_ruleCount >= MAX_RULES) {
      client.println("HTTP/1.1 507 Insufficient Storage\r\nConnection: close\r\n");
      return;
    }
    idx = g_ruleCount++;
    g_rules[idx].id = (rid == 0xFF) ? (uint8_t)idx : rid;
  }

  Rule& r = g_rules[idx];
  r.enabled  = doc["enabled"]  | r.enabled;
  r.sensor   = (SensorType)(doc["sensor"]   | (uint8_t)r.sensor);
  r.cond     = (Condition)(doc["cond"]       | (uint8_t)r.cond);
  r.onValue  = doc["onValue"]  | r.onValue;
  r.offValue = doc["offValue"] | r.offValue;
  r.relayCh  = doc["relayCh"]  | r.relayCh;
  r.inverted = doc["inverted"] | r.inverted;
  const char* nm = doc["name"] | "";
  if (nm[0]) { strncpy(r.name, nm, sizeof(r.name) - 1); r.name[sizeof(r.name)-1] = '\0'; }

  saveRules(g_rules, g_ruleCount);
  logAdd(g_log, getCurrentEpoch(), EVT_RULE, 0, 0, r.id, "rule updated");

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Connection: close");
  client.println();
  client.printf("{\"ok\":true,\"id\":%d}\n", r.id);
}

// ============================================================
// POST /api/rules/delete — delete rule by id
// Body: {"id":N}
// ============================================================
void handleRulesDeletePost(WiFiClient& client, const String& body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    client.println("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n");
    return;
  }
  uint8_t rid = doc["id"] | (uint8_t)0xFF;
  int idx = -1;
  for (int i = 0; i < g_ruleCount; i++) {
    if (g_rules[i].id == rid) { idx = i; break; }
  }
  if (idx < 0) {
    client.println("HTTP/1.1 404 Not Found\r\nConnection: close\r\n");
    return;
  }
  // Shift array
  for (int i = idx; i < g_ruleCount - 1; i++) g_rules[i] = g_rules[i + 1];
  g_ruleCount--;
  saveRules(g_rules, g_ruleCount);
  logAdd(g_log, getCurrentEpoch(), EVT_RULE, 0, 0, rid, "rule deleted");

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Connection: close");
  client.println();
  client.printf("{\"ok\":true,\"id\":%d}\n", rid);
}

// ============================================================
// POST /api/schedules — upsert a schedule (JSON body)
// ============================================================
void handleSchedulesPost(WiFiClient& client, const String& body) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    client.println("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n");
    return;
  }

  uint8_t sid = doc["id"] | (uint8_t)0xFF;
  int idx = -1;
  for (int i = 0; i < g_schedCount; i++) {
    if (g_schedules[i].id == sid) { idx = i; break; }
  }
  if (idx < 0) {
    if (g_schedCount >= MAX_SCHEDULES) {
      client.println("HTTP/1.1 507 Insufficient Storage\r\nConnection: close\r\n");
      return;
    }
    idx = g_schedCount++;
    g_schedules[idx].id = (sid == 0xFF) ? (uint8_t)idx : sid;
  }

  Schedule& s = g_schedules[idx];
  s.enabled     = doc["enabled"]     | s.enabled;
  s.relayCh     = doc["relayCh"]     | s.relayCh;
  s.hour        = doc["hour"]        | s.hour;
  s.minute      = doc["minute"]      | s.minute;
  s.durationSec = doc["durationSec"] | s.durationSec;
  s.dowMask     = doc["dowMask"]     | s.dowMask;
  const char* nm = doc["name"] | "";
  if (nm[0]) { strncpy(s.name, nm, sizeof(s.name) - 1); s.name[sizeof(s.name)-1] = '\0'; }
  s.running = false;  // reset runtime state on edit

  saveSchedules(g_schedules, g_schedCount);
  logAdd(g_log, getCurrentEpoch(), EVT_SCHEDULE, 0, 0, s.id, "schedule updated");

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Connection: close");
  client.println();
  client.printf("{\"ok\":true,\"id\":%d}\n", s.id);
}

// ============================================================
// POST /api/schedules/delete — delete schedule by id
// Body: {"id":N}
// ============================================================
void handleSchedulesDeletePost(WiFiClient& client, const String& body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    client.println("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n");
    return;
  }
  uint8_t sid = doc["id"] | (uint8_t)0xFF;
  int idx = -1;
  for (int i = 0; i < g_schedCount; i++) {
    if (g_schedules[i].id == sid) { idx = i; break; }
  }
  if (idx < 0) {
    client.println("HTTP/1.1 404 Not Found\r\nConnection: close\r\n");
    return;
  }
  for (int i = idx; i < g_schedCount - 1; i++) g_schedules[i] = g_schedules[i + 1];
  g_schedCount--;
  saveSchedules(g_schedules, g_schedCount);
  logAdd(g_log, getCurrentEpoch(), EVT_SCHEDULE, 0, 0, sid, "schedule deleted");

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Connection: close");
  client.println();
  client.printf("{\"ok\":true,\"id\":%d}\n", sid);
}

// ============================================================
// HTTP Request Router
// ============================================================
void handleWebClient() {
  WiFiClient client = webServer.accept();
  if (!client) return;

  client.setTimeout(500);
  unsigned long t = millis();

  while (!client.available() && (millis() - t) < 500) delay(1);
  if (!client.available()) { client.stop(); return; }

  String reqLine = client.readStringUntil('\n');
  reqLine.trim();

  int sp1 = reqLine.indexOf(' ');
  int sp2 = (sp1 >= 0) ? reqLine.indexOf(' ', sp1 + 1) : -1;
  if (sp1 < 0 || sp2 < 0) { client.stop(); return; }

  String method = reqLine.substring(0, sp1);
  String path   = reqLine.substring(sp1 + 1, sp2);

  int contentLength = 0;
  while ((millis() - t) < 2000) {
    String hdr = client.readStringUntil('\n');
    hdr.trim();
    if (hdr.length() == 0) break;
    if (hdr.startsWith("Content-Length:")) {
      contentLength = hdr.substring(15).toInt();
    }
  }

  String body;
  if (contentLength > 0) {
    unsigned long bt = millis();
    while ((int)body.length() < contentLength && (millis() - bt) < 1000) {
      if (client.available()) body += (char)client.read();
    }
  }

  // GET routes
  if (method == "GET") {
    if (path == "/" || path == "/index.html")     sendDashboard(client);
    else if (path == "/rules")                    sendRulesPage(client);
    else if (path == "/schedule")                 sendSchedulePage(client);
    else if (path == "/config")                   sendConfigPageWrapper(client);
    else if (path == "/log")                      sendLogPage(client);
    else if (path == "/api/state")                sendAPIStateWrapper(client);
    else if (path == "/api/rules")                sendAPIRulesWrapper(client);
    else if (path == "/api/schedules")            sendAPISchedulesWrapper(client);
    else if (path == "/api/config")               sendAPIConfig(client);
    else if (path == "/api/log")                  sendAPILogWrapper(client);
    else send404(client);
  }
  // POST routes
  else if (method == "POST") {
    if      (path == "/api/config")               handleConfigPost(client, body);
    else if (path == "/api/rules")                handleRulesPost(client, body);
    else if (path == "/api/rules/delete")         handleRulesDeletePost(client, body);
    else if (path == "/api/schedules")            handleSchedulesPost(client, body);
    else if (path == "/api/schedules/delete")     handleSchedulesDeletePost(client, body);
    else if (path.startsWith("/api/relay/")) {
      int ch = path.substring(11).toInt();
      handleRelayPost(client, ch, body);
    }
    else send404(client);
  }
  else {
    client.println("HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\n");
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

  // 1. Safe state: all relays OFF
  initRelaysOff();

  // 2. LittleFS
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

  // 3. Config (sets static IP on eth before eth.begin())
  loadConfig();
  Serial.printf("Node=%s\n", nodeId.c_str());
  Serial.printf("[BOOT] hostname: %s.local\n", mdnsHostname.c_str());

  // 4. I2C (RTC + sensors)
  Wire.setSDA(I2C_SDA);
  Wire.setSCL(I2C_SCL);
  Wire.begin();
  delay(200);
  Serial.printf("I2C0: SDA=%d SCL=%d\n", I2C_SDA, I2C_SCL);

  // 5. I2C sensor scan
  scanI2CSensors();

  // 6. Ethernet
  initEthernet();

  // 7. NTP sync
  syncNTP();

  // 8. HTTP server start
  webServer.begin();
  Serial.print("WebUI: http://");
  Serial.println(eth.localIP().toString());

  // 9. mDNS
  if (mdns_enabled) {
    if (MDNS.begin(mdnsHostname.c_str())) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("mDNS: %s.local\n", mdnsHostname.c_str());
    } else {
      Serial.println("mDNS: begin failed (continuing without mDNS)");
    }
  } else {
    Serial.println("mDNS: disabled by config");
  }

  // 10. RS485
  initRS485();

  // 11. Load rules, schedules, log
  g_ruleCount  = loadRules(g_rules);
  g_schedCount = loadSchedules(g_schedules);
  logLoad(g_log);
  Serial.printf("Rules: %d  Schedules: %d  Log entries: %d\n",
                g_ruleCount, g_schedCount, g_log.count);

  // 12. Boot log entry
  logAdd(g_log, getCurrentEpoch(), EVT_SYSTEM, 0, 0, 0, "System boot");

  // 13. DI interrupt setup
  for (int i = 0; i < 8; i++) {
    pinMode(DI_PINS[i], INPUT_PULLUP);
  }
  attachInterrupt(digitalPinToInterrupt(DI_PINS[0]), diPulseISR1, FALLING);
  attachInterrupt(digitalPinToInterrupt(DI_PINS[1]), diPulseISR2, FALLING);
  for (int i = 2; i < 8; i++) {
    attachInterrupt(digitalPinToInterrupt(DI_PINS[i]), diISR, CHANGE);
  }
  Serial.println("DI: GPIO9-10 pulse(FALLING), GPIO11-16 state(CHANGE)");

  // 14. Watchdog (Tier 1 HW + Tier 2 SW)
  watchdog_enable(HW_WDT_TIMEOUT_MS, true);
  Serial.printf("HW WDT: %dms\n", HW_WDT_TIMEOUT_MS);
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

  // Watchdog feed
  watchdog_update();   // Tier 1: HW WDT
  swWdtFeed();         // Tier 2: SW WDT

  // [STATUS] 30秒毎デバッグ出力
  if (millis() - last_status >= 30000UL) {
    Serial.printf("[STATUS] ip:%s up:%lus\n",
                  eth.localIP().toString().c_str(),
                  millis() / 1000);
    last_status = millis();
  }

  // Tier 3: periodic reboot (24h)
  if (millis() >= REBOOT_INTERVAL) {
    rebootWithReason("periodic_24h");
  }

  // Ethernet check
  if (!eth.connected()) {
    Serial.println("ETH: disconnected, rebooting...");
    rebootWithReason("eth_disconnected");
  }

  // mDNS update
  if (mdns_enabled) MDNS.update();

  // HTTP
  handleWebClient();

  // Manual override expiry
  checkManualExpiry();

  // Duration auto-off (relay timed off from manual relay post)
  {
    unsigned long now = millis();
    for (int i = 0; i < 8; i++) {
      if (relayDurationEnd[i] > 0 && now >= relayDurationEnd[i]) {
        relayDurationEnd[i] = 0;
        if ((relayState >> i) & 1) {
          setRelay(i + 1, false, SRC_MANUAL, 0);
          Serial.printf("CH%d auto-OFF (duration expired)\n", i + 1);
        }
        g_chCtrl[i].source      = SRC_NONE;
        g_chCtrl[i].manualExpiry = 0;
      }
    }
  }

  // DI interrupt debounce
  {
    unsigned long now = millis();
    if (diInterruptFlag && (now - diLastDebounce >= DI_DEBOUNCE_MS)) {
      diInterruptFlag = false;
      diLastDebounce  = now;
      readDI();
    }
  }

  // Heartbeat (SENSOR_INTERVAL)
  static unsigned long lastSensor = 0;
  {
    unsigned long now = millis();
    if (now - lastSensor >= SENSOR_INTERVAL) {
      lastSensor = now;

      readSensors();
      calculateVPD();
      pollDrainSensor();

      float rain_mm = sen0575_cumRainRaw / 10000.0f;

      // Rule engine evaluation
      uint8_t ruleReq[8], ruleSrc[8];
      memset(ruleReq, 0xFF, 8);
      memset(ruleSrc, 0, 8);
      evaluateRules(g_rules, g_ruleCount,
                    g_sht40_temp, g_sht40_hum, rain_mm, diState,
                    ruleReq, ruleSrc);

      // Scheduler evaluation
      uint8_t schedReq[8], schedSrc[8];
      memset(schedReq, 0xFF, 8);
      memset(schedSrc, 0, 8);
      evaluateSchedules(g_schedules, g_schedCount, getCurrentEpoch(),
                        schedReq, schedSrc);

      // Resolve and apply
      resolveControl(ruleReq, ruleSrc, schedReq, schedSrc);

      Serial.printf("[%d] relay=0x%02X epoch=%lu uptime=%lus vpd=%.3f\n",
                    loopCount, relayState, getCurrentEpoch(),
                    millis() / 1000, isnan(g_vpd) ? 0.0f : g_vpd);
    }
  }

  // Log flush (count-based or time-based)
  static unsigned long lastLogFlush = 0;
  {
    unsigned long now = millis();
    if (logNeedsFlush(g_log) || (now - lastLogFlush >= LOG_FLUSH_INTERVAL)) {
      lastLogFlush = now;
      logFlush(g_log);
    }
  }

  // NTP resync (1h)
  static unsigned long lastNtpSync = 0;
  {
    unsigned long now = millis();
    if (now - lastNtpSync >= NTP_SYNC_INTERVAL) {
      lastNtpSync = now;
      syncNTP();
    }
  }

  delay(50);
}
