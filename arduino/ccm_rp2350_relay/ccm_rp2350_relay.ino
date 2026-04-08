// ccm_rp2350_relay.ino - Waveshare 8ch Relay + DI Node (RP2350B) — UECS-CCM版
// Board: RP2350-ETH-8DI-8RO / RP2350-POE-ETH-8DI-8RO
// Protocol: UECS-CCM (UDP multicast 224.0.0.1:16520)
// Relay: GPIO17-24 直接制御
// DI:    GPIO9-16, フォトカプラ絶縁, アクティブLOW, 割り込み検知
// RTC:   PCF85063 (I2C1: SDA=GPIO6, SCL=GPIO7) — GPIO6/7 is I2C1 on RP2350B pinmux
// Comm:  W5500 SPI0 (CS=GPIO33, RST=GPIO25, SCK=GPIO34, MOSI=GPIO35, MISO=GPIO36)
// Framework: arduino-pico (Earle Philhower)
//
// Libraries: arduino-pico 4.5.2+, ArduinoJson 7.x, NTPClient 3.2.1,
//            W5500lwIP (arduino-pico内蔵), SensirionI2cSht4x (optional),
//            LEAmDNS (arduino-pico内蔵)
// Note: PubSubClient 不要 (CCM = UDP multicast)

#include <SPI.h>
#include <W5500lwIP.h>
#include <ArduinoJson.h>        // v7.x
#include <LittleFS.h>
#include <Wire.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <SensirionI2cSht4x.h>
#include <LEAmDNS.h>
#include <Updater.h>
#include <Adafruit_NeoPixel.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#include "sw_watchdog.h"
#include "sensor_registry.h"

// ========== Firmware Version ==========
const char* FW_VERSION = "1.0.0";
const char* FW_NAME    = "ccm_rp2350_relay";

// ========== UECS-CCM Protocol ==========
const IPAddress CCM_MULTICAST(224, 0, 0, 1);
const int       CCM_PORT       = 16520;
const char*     UECS_VERSION   = "1.00-E10";
const int       CCM_SEND_INTERVAL = 10;  // seconds

// ========== Default Configuration ==========
const char* DEFAULT_NODE_NAME     = "UECS-Pi Relay";
const char* DEFAULT_IP            = "";          // 空=DHCP
const char* DEFAULT_SUBNET        = "255.255.255.0";
const char* DEFAULT_GATEWAY       = "192.168.1.1";
const char* DEFAULT_DNS           = "192.168.1.1";
const bool  DEFAULT_MDNS_ENABLED   = true;
const char* DEFAULT_MDNS_HOSTNAME  = "uecs-ccm-01";
const char* DEFAULT_NODE_ID        = "ccm_relay_01";

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

// ========== I2C0 Pins (RTC PCF85063) ==========
const int I2C_SDA = 6;  // I2C1 SDA (not I2C0 — GPIO6 is I2C1 per RP2350 pinmux)
const int I2C_SCL = 7;  // I2C1 SCL

// ========== WS2812 RGB LED ==========
const int WS2812_PIN = 2;   // GPIO2 (onboard)
const int WS2812_NUM = 1;   // 1 LED

// ========== DS18B20 OneWire ==========
const int ONEWIRE_PIN = 3;  // GPIO3 (Grove基板から引出し)

// ========== PCF85063 RTC ==========
const uint8_t PCF85063_ADDR = 0x51;

// ========== Relay GPIO Pins (GPIO17-24 直接制御) ==========
const int RELAY_PINS[8] = {17, 18, 19, 20, 21, 22, 23, 24};

// ========== DI GPIO Pins (GPIO9-16, アクティブLOW) ==========
const int DI_PINS[8] = {9, 10, 11, 12, 13, 14, 15, 16};

// ========== CCM Channel Mapping ==========
// 各リレーchに対するCCM属性 (WebUIから設定)
struct CcmMapping {
  char     ccmType[32];   // CcmInfoName: "Irri", "VenFan", etc. 空=未割当
  int      room;
  int      region;
  int      order;
  int      priority;
  char     suffix[8];     // ".cMC", ".mC", ".MC" — default ".cMC"
  int      watchdog_sec;  // 無通信タイマー秒 (0=無効, >0: 最終CCM受信からN秒でOFF)
  int      di_link;      // DI連動 (-1=なし, 0-7=DI番号, ON→リレーON)
  bool     di_invert;    // DI反転 (true: DI ON→リレーOFF, フロートスイッチ等)
};

// ArSprout標準のアクチュエータタイプ
const char* CCM_ACTUATOR_TYPES[] = {
  "", "Irri", "VenFan", "CirHoriFan", "AirHeatBurn", "AirHeatHP",
  "CO2Burn", "VenRfWin", "VenSdWin", "ThCrtn", "LsCrtn",
  "AirCoolHP", "AirHumFog", "Relay"
};
const int CCM_ACTUATOR_TYPES_COUNT = sizeof(CCM_ACTUATOR_TYPES) / sizeof(CCM_ACTUATOR_TYPES[0]);

CcmMapping ccmMap[8];

// ========== Timing ==========
const int           SENSOR_INTERVAL      = 10;
const int           ETH_CONNECT_TIMEOUT  = 15;
const unsigned long REBOOT_INTERVAL      = 600000UL;  // 10分
const unsigned long NTP_SYNC_INTERVAL    = 3600000UL;

// ========== HW WDT ==========
const int HW_WDT_TIMEOUT_MS = 8000;

// ========== Global State ==========
uint8_t      relayState          = 0x00;
unsigned long relayDurationEnd[8] = {0};
unsigned long lastCcmRx[8]       = {0};  // 最終CCM受信時刻 (millis)

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

bool  ds18b20_detected  = false;
float g_ds18b20_temp    = NAN;

bool mdns_enabled = DEFAULT_MDNS_ENABLED;

unsigned long ntpEpoch  = 0;
unsigned long ntpMillis = 0;

int loopCount = 0;
unsigned long last_status = 0;  // [STATUS] 30秒タイマー

// ========== Objects ==========
// Note: eth must be constructed after SPI pin setup in initEthernet()
// GPIO33-36 are SPI0 pins on RP2350B (not SPI1)
Wiznet5500lwIP* ethPtr = nullptr;
#define eth (*ethPtr)
WiFiUDP        ccmUDP;      // CCM multicast send/receive
WiFiUDP        ntpUDP;
NTPClient      timeClient(ntpUDP, "pool.ntp.org", 0);
SensirionI2cSht4x sht4x;
Adafruit_NeoPixel rgbLED(WS2812_NUM, WS2812_PIN, NEO_GRB + NEO_KHZ800);
OneWire           oneWire(ONEWIRE_PIN);
DallasTemperature ds18b20(&oneWire);
WiFiServer        webServer(80);

String nodeId;
String nodeName;
String mdnsHostname;
int    rs485Baud = RS485_DEFAULT_BAUD;

// ========== RS485 / SEN0575 ==========
const uint8_t  SEN0575_ADDR          = 0xC0;
const uint16_t SEN0575_REG_CUMRAIN_H = 0x0007;
const uint16_t SEN0575_REG_RAWDATA_H = 0x0009;
const uint16_t SEN0575_REG_SYSTIME   = 0x000B;
const uint16_t SEN0575_REG_PID_H     = 0x0000;

bool     sen0575_detected    = false;
uint32_t sen0575_cumRainRaw  = 0;
uint32_t sen0575_rawTips     = 0;
uint16_t sen0575_workTimeMins = 0;

// ========== Function Declarations ==========
void loadConfig();
void loadCcmMapping();
void saveCcmMapping();
void initEthernet();
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
void handleWebClient();
void sendDashboard(WiFiClient& client);
void sendAPIState(WiFiClient& client);
void sendAPIConfig(WiFiClient& client);
void sendConfigPage(WiFiClient& client);
void sendCcmConfigPage(WiFiClient& client);
void handleRelayPost(WiFiClient& client, int ch, const String& body);
void handleConfigPost(WiFiClient& client, const String& body);
void handleCcmConfigPost(WiFiClient& client, const String& body);
void sendOTAPage(WiFiClient& client);
void handleOTAUpload(WiFiClient& client, int contentLength);
void initRS485();
void pollDrainSensor();
uint16_t modbusCalcCRC(const uint8_t* data, size_t len);
bool modbusReadInput(uint8_t addr, uint16_t reg, uint16_t count,
                     uint16_t* out1, uint16_t* out2);
void ccmSendStates();
void ccmReceive();

// ============================================================
// DI Interrupt
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
  Wire1.beginTransmission(PCF85063_ADDR);
  Wire1.write(0x04);
  if (Wire1.endTransmission(false) != 0) return false;
  if (Wire1.requestFrom((uint8_t)PCF85063_ADDR, (uint8_t)7) != 7) return false;
  t->tm_sec  = bcd2dec(Wire1.read() & 0x7F);
  t->tm_min  = bcd2dec(Wire1.read() & 0x7F);
  t->tm_hour = bcd2dec(Wire1.read() & 0x3F);
  t->tm_mday = bcd2dec(Wire1.read() & 0x3F);
  Wire1.read();
  t->tm_mon  = bcd2dec(Wire1.read() & 0x1F) - 1;
  t->tm_year = bcd2dec(Wire1.read()) + 100;
  t->tm_isdst = 0;
  return true;
}

bool rtcSetTime(struct tm* t) {
  Wire1.beginTransmission(PCF85063_ADDR);
  Wire1.write(0x04);
  Wire1.write(dec2bcd(t->tm_sec));
  Wire1.write(dec2bcd(t->tm_min));
  Wire1.write(dec2bcd(t->tm_hour));
  Wire1.write(dec2bcd(t->tm_mday));
  Wire1.write(0);
  Wire1.write(dec2bcd(t->tm_mon + 1));
  Wire1.write(dec2bcd(t->tm_year - 100));
  return Wire1.endTransmission() == 0;
}

unsigned long getCurrentEpoch() {
  if (ntpEpoch == 0) return 0;
  return ntpEpoch + (millis() - ntpMillis) / 1000;
}

// ============================================================
// NTP Sync
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

  unsigned long e = ntpEpoch;
  struct tm t;
  t.tm_sec  = e % 60; e /= 60;
  t.tm_min  = e % 60; e /= 60;
  t.tm_hour = e % 24; e /= 24;
  if (rtcSetTime(&t)) {
    Serial.println("RTC: time written (hms only)");
  }
}

// ============================================================
// Relay Control
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
    diState[i] = !digitalRead(DI_PINS[i]);
    if (diState[i] != diPrevState[i]) {
      changed = true;
      Serial.printf("DI%d: %s\n", i + 1, diState[i] ? "ON" : "OFF");
    }
  }
  memcpy(diPrevState, diState, sizeof(diState));
  return changed;
}

// ============================================================
// I2C Sensor
// ============================================================
void scanI2CSensors() {
  Wire1.setTimeout(10);
  Serial.println("I2C scan:");
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    if (addr == PCF85063_ADDR) continue;
    Wire1.beginTransmission(addr);
    uint8_t rc = Wire1.endTransmission();
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
  if (found == 0) Serial.println("  no devices (continuing)");
  if (sht40_detected) {
    sht4x.begin(Wire1, 0x44);
    Serial.println("SHT40 initialized");
  }
}

void readSensors() {
  // SHT40 (I2C)
  if (sht40_detected) {
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
        Serial.println("SHT40: disabled after 3 errors");
      }
    } else {
      sht40_error_count = 0;
      g_sht40_temp = temp;
      g_sht40_hum  = hum;
    }
  }

  // DS18B20 (OneWire)
  if (ds18b20_detected) {
    ds18b20.requestTemperatures();
    float t = ds18b20.getTempCByIndex(0);
    if (t != DEVICE_DISCONNECTED_C && t > -55.0 && t < 125.0) {
      g_ds18b20_temp = t;
    } else {
      g_ds18b20_temp = NAN;
    }
  }
}

// ============================================================
// RS485 / Modbus RTU — DFRobot SEN0575
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

void initRS485() {
  Serial2.setTX(RS485_TX);
  Serial2.setRX(RS485_RX);
  Serial2.begin(rs485Baud);
  Serial.printf("RS485: UART1 TX=GPIO%d RX=GPIO%d baud=%d\n", RS485_TX, RS485_RX, rs485Baud);

  delay(100);
  uint16_t pidH = 0, pidL = 0;
  if (modbusReadInput(SEN0575_ADDR, SEN0575_REG_PID_H, 2, &pidH, &pidL)) {
    uint32_t pid = ((uint32_t)pidH << 16) | pidL;
    sen0575_detected = (pid == 0x000100C0);
    Serial.printf("SEN0575: PID=0x%08lX %s\n", pid,
                  sen0575_detected ? "DETECTED" : "PID mismatch");
  } else {
    Serial.println("SEN0575: not found");
  }
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

  float rainfall_mm = sen0575_cumRainRaw / 10000.0;
  float workHours   = sen0575_workTimeMins / 60.0;
  Serial.printf("SEN0575: rain=%.2fmm tips=%lu work=%.1fh\n",
                rainfall_mm, (unsigned long)sen0575_rawTips, workHours);
}

// ============================================================
// UECS-CCM: Send relay/DI/sensor states as CCM XML
// UDP multicast 224.0.0.1:16520
// ============================================================
void ccmSendStates() {
  // Build XML packet with all mapped channels
  String xml = "<UECS ver=\"";
  xml += UECS_VERSION;
  xml += "\">";

  // Relay states (mapped channels only)
  for (int i = 0; i < 8; i++) {
    if (ccmMap[i].ccmType[0] == '\0') continue;  // unmapped
    int val = (relayState >> i) & 1;
    xml += "<DATA type=\"";
    xml += ccmMap[i].ccmType;
    xml += ccmMap[i].suffix;
    xml += "\" room=\"";
    xml += ccmMap[i].room;
    xml += "\" region=\"";
    xml += ccmMap[i].region;
    xml += "\" order=\"";
    xml += ccmMap[i].order;
    xml += "\" priority=\"";
    xml += ccmMap[i].priority;
    xml += "\" lv=\"S\" cast=\"uni\">";
    xml += val;
    xml += "</DATA>";
  }

  // I2C sensor: InAirTemp, InAirHumid (room=1, region=11 = internal sensor)
  if (sht40_detected && !isnan(g_sht40_temp)) {
    int room = (ccmMap[0].ccmType[0] != '\0') ? ccmMap[0].room : 1;
    int region = 11;  // UECS internal sensor region

    xml += "<DATA type=\"InAirTemp.cMC\" room=\"";
    xml += room;
    xml += "\" region=\"";
    xml += region;
    xml += "\" order=\"1\" priority=\"29\" lv=\"S\" cast=\"uni\">";
    char tempBuf[8];
    dtostrf(g_sht40_temp, 1, 1, tempBuf);
    xml += tempBuf;
    xml += "</DATA>";

    if (!isnan(g_sht40_hum)) {
      xml += "<DATA type=\"InAirHumid.cMC\" room=\"";
      xml += room;
      xml += "\" region=\"";
      xml += region;
      xml += "\" order=\"1\" priority=\"29\" lv=\"S\" cast=\"uni\">";
      char humBuf[8];
      dtostrf(g_sht40_hum, 1, 1, humBuf);
      xml += humBuf;
      xml += "</DATA>";
    }
  }

  // DS18B20 (OneWire) — 外部温度としてCCM送信
  if (ds18b20_detected && !isnan(g_ds18b20_temp)) {
    int room = (ccmMap[0].ccmType[0] != '\0') ? ccmMap[0].room : 2;
    xml += "<DATA type=\"InAirTemp.cMC\" room=\"";
    xml += room;
    xml += "\" region=\"12\" order=\"2\" priority=\"29\" lv=\"S\" cast=\"uni\">";
    char dsBuf[8];
    dtostrf(g_ds18b20_temp, 1, 1, dsBuf);
    xml += dsBuf;
    xml += "</DATA>";
  }

  // SEN0575 rainfall as WRainfallAmt (weather rainfall amount)
  if (sen0575_detected) {
    float rainfall_mm = sen0575_cumRainRaw / 10000.0;
    xml += "<DATA type=\"WRainfallAmt.cMC\" room=\"1\" region=\"41\" ";
    xml += "order=\"1\" priority=\"29\" lv=\"S\" cast=\"uni\">";
    char rainBuf[12];
    dtostrf(rainfall_mm, 1, 2, rainBuf);
    xml += rainBuf;
    xml += "</DATA>";
  }

  xml += "</UECS>";

  // Send via UDP multicast
  if (ccmUDP.beginPacketMulticast(CCM_MULTICAST, CCM_PORT, eth.localIP())) {
    ccmUDP.write((const uint8_t*)xml.c_str(), xml.length());
    ccmUDP.endPacket();
    Serial.printf("CCM TX: %d bytes\n", xml.length());
  }
}

// ============================================================
// UECS-CCM: Receive and process incoming CCM packets
// Match type+room+region+order to relay channels
// ============================================================
void ccmReceive() {
  int packetSize = ccmUDP.parsePacket();
  if (packetSize <= 0) return;

  // Read packet (max 2048 bytes)
  char buf[2048];
  int len = ccmUDP.read(buf, sizeof(buf) - 1);
  if (len <= 0) return;
  buf[len] = '\0';

  // Skip packets from ourselves
  IPAddress remote = ccmUDP.remoteIP();
  if (remote == eth.localIP()) return;

  // Simple XML parsing: find each <DATA ...>value</DATA>
  char* pos = buf;
  while ((pos = strstr(pos, "<DATA ")) != nullptr) {
    char* tagEnd = strchr(pos, '>');
    if (!tagEnd) break;

    // Extract attributes
    char type[48] = "";
    int  room = -1, region = -1, order = -1, priority = -1;

    // type="..."
    char* tAttr = strstr(pos, "type=\"");
    if (tAttr && tAttr < tagEnd) {
      tAttr += 6;
      char* tEnd = strchr(tAttr, '"');
      if (tEnd && tEnd < tagEnd) {
        int tLen = tEnd - tAttr;
        if (tLen < (int)sizeof(type)) {
          memcpy(type, tAttr, tLen);
          type[tLen] = '\0';
        }
      }
    }

    // room="..."
    char* rAttr = strstr(pos, "room=\"");
    if (rAttr && rAttr < tagEnd) room = atoi(rAttr + 6);

    // region="..."
    char* rgAttr = strstr(pos, "region=\"");
    if (rgAttr && rgAttr < tagEnd) region = atoi(rgAttr + 8);

    // order="..."
    char* oAttr = strstr(pos, "order=\"");
    if (oAttr && oAttr < tagEnd) order = atoi(oAttr + 7);

    // priority="..."
    char* pAttr = strstr(pos, "priority=\"");
    if (pAttr && pAttr < tagEnd) priority = atoi(pAttr + 10);

    // Value (between > and </DATA>)
    char* valStart = tagEnd + 1;
    char* valEnd   = strstr(valStart, "</DATA>");
    if (!valEnd) break;

    char valBuf[32] = "";
    int valLen = valEnd - valStart;
    if (valLen > 0 && valLen < (int)sizeof(valBuf)) {
      memcpy(valBuf, valStart, valLen);
      valBuf[valLen] = '\0';
    }

    // Strip CCM suffix from type for matching (.cMC, .mC, .MC)
    char baseType[48];
    strncpy(baseType, type, sizeof(baseType));
    baseType[sizeof(baseType) - 1] = '\0';
    char* dot = strrchr(baseType, '.');
    if (dot) {
      // Check if suffix is a known CCM suffix
      if (strcmp(dot, ".cMC") == 0 || strcmp(dot, ".mC") == 0 || strcmp(dot, ".MC") == 0) {
        *dot = '\0';
      }
    }

    // Match against our channel mapping
    for (int ch = 0; ch < 8; ch++) {
      if (ccmMap[ch].ccmType[0] == '\0') continue;
      if (strcmp(ccmMap[ch].ccmType, baseType) != 0) continue;
      if (ccmMap[ch].room   != room)   continue;
      if (ccmMap[ch].region != region) continue;
      if (ccmMap[ch].order  != order)  continue;

      // Priority check: only accept higher priority (lower number)
      if (priority >= 0 && ccmMap[ch].priority > 0 && priority > ccmMap[ch].priority) {
        continue;  // lower priority, ignore
      }

      float fval = atof(valBuf);
      int ival = (int)(fval + 0.5);

      lastCcmRx[ch] = millis();
      if (ival > 0) {
        setRelay(ch + 1, true);
      } else {
        setRelay(ch + 1, false);
      }
      Serial.printf("CCM RX: %s room=%d → CH%d = %d\n", type, room, ch + 1, ival);
      break;
    }

    pos = valEnd + 7;  // skip past </DATA>
  }
}

// ============================================================
// CCM Mapping Config (LittleFS /ccm_map.json)
// ============================================================
void loadCcmMapping() {
  // Initialize defaults
  for (int i = 0; i < 8; i++) {
    strncpy(ccmMap[i].ccmType, "Relay", sizeof(ccmMap[i].ccmType) - 1);
    ccmMap[i].room     = 2;
    ccmMap[i].region   = 61;
    ccmMap[i].order    = 1;
    ccmMap[i].priority = 1;
    ccmMap[i].watchdog_sec = 60;
    ccmMap[i].di_link      = -1;
    ccmMap[i].di_invert    = false;
    strncpy(ccmMap[i].suffix, ".cMC", sizeof(ccmMap[i].suffix));
  }

  if (!LittleFS.exists("/ccm_map.json")) {
    Serial.println("CCM map: no config, using defaults (all Relay, room=2)");
    return;
  }

  File f = LittleFS.open("/ccm_map.json", "r");
  if (!f) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.printf("CCM map parse error: %s\n", err.c_str());
    return;
  }

  JsonArray arr = doc["channels"].as<JsonArray>();
  int idx = 0;
  for (JsonObject ch : arr) {
    if (idx >= 8) break;
    const char* t = ch["type"] | "";
    strncpy(ccmMap[idx].ccmType, t, sizeof(ccmMap[idx].ccmType) - 1);
    ccmMap[idx].ccmType[sizeof(ccmMap[idx].ccmType) - 1] = '\0';
    ccmMap[idx].room     = ch["room"]     | 1;
    ccmMap[idx].region   = ch["region"]   | 61;
    ccmMap[idx].order    = ch["order"]    | 1;
    ccmMap[idx].priority     = ch["priority"]     | 1;
    ccmMap[idx].watchdog_sec = ch["watchdog_sec"] | 60;
    ccmMap[idx].di_link      = ch["di_link"]      | -1;
    ccmMap[idx].di_invert    = ch["di_invert"]    | false;
    const char* sfx = ch["suffix"] | ".cMC";
    strncpy(ccmMap[idx].suffix, sfx, sizeof(ccmMap[idx].suffix) - 1);
    ccmMap[idx].suffix[sizeof(ccmMap[idx].suffix) - 1] = '\0';
    idx++;
  }

  Serial.println("CCM map loaded:");
  for (int i = 0; i < 8; i++) {
    if (ccmMap[i].ccmType[0] != '\0') {
      Serial.printf("  CH%d: %s%s room=%d region=%d order=%d pri=%d\n",
                    i + 1, ccmMap[i].ccmType, ccmMap[i].suffix,
                    ccmMap[i].room, ccmMap[i].region,
                    ccmMap[i].order, ccmMap[i].priority);
    }
  }
}

void saveCcmMapping() {
  JsonDocument doc;
  JsonArray arr = doc["channels"].to<JsonArray>();
  for (int i = 0; i < 8; i++) {
    JsonObject ch = arr.add<JsonObject>();
    ch["type"]     = ccmMap[i].ccmType;
    ch["room"]     = ccmMap[i].room;
    ch["region"]   = ccmMap[i].region;
    ch["order"]    = ccmMap[i].order;
    ch["priority"]     = ccmMap[i].priority;
    ch["watchdog_sec"] = ccmMap[i].watchdog_sec;
    ch["di_link"]      = ccmMap[i].di_link;
    ch["di_invert"]    = ccmMap[i].di_invert;
    ch["suffix"]       = ccmMap[i].suffix;
  }

  File f = LittleFS.open("/ccm_map.json", "w");
  if (!f) {
    Serial.println("CCM map: save failed");
    return;
  }
  serializeJson(doc, f);
  f.close();
  Serial.println("CCM map saved");
}

// ============================================================
// Configuration (LittleFS /config.json)
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
        nodeId       = (const char*)(doc["node_id"]        | DEFAULT_NODE_ID);
        nodeName     = (const char*)(doc["node_name"]      | DEFAULT_NODE_NAME);
        mdnsHostname = (const char*)(doc["mdns_hostname"]  | DEFAULT_MDNS_HOSTNAME);
        ipStr        = (const char*)(doc["ip"]             | DEFAULT_IP);
        rs485Baud    = doc["rs485_baud"] | RS485_DEFAULT_BAUD;
        mdns_enabled = doc["mdns_enabled"] | DEFAULT_MDNS_ENABLED;

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
  nodeId       = DEFAULT_NODE_ID;
  nodeName     = DEFAULT_NODE_NAME;
  mdnsHostname = DEFAULT_MDNS_HOSTNAME;
}

// ============================================================
// Ethernet
// ============================================================
void initEthernet() {
  pinMode(W5500_RST, OUTPUT);
  digitalWrite(W5500_RST, LOW);
  delay(100);
  digitalWrite(W5500_RST, HIGH);
  delay(500);

  SPI.setSCK(W5500_SCK);
  SPI.setTX(W5500_MOSI);
  SPI.setRX(W5500_MISO);
  SPI.begin();

  // Construct eth object AFTER SPI pins are configured
  // GPIO33-36 = SPI0 on RP2350B pinmux
  ethPtr = new Wiznet5500lwIP(W5500_CS, SPI, W5500_INT);

  lwipPollingPeriod(5);
  eth.begin();

  Serial.println("ETH: waiting for link...");
  unsigned long start = millis();
  while (!eth.connected()) {
    if (millis() - start > (unsigned long)ETH_CONNECT_TIMEOUT * 1000UL) {
      Serial.println("[ERR] DHCP timeout, retrying...");
      rebootWithReason("eth_timeout");
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

void rebootWithReason(const char* reason) {
  Serial.printf("Rebooting: %s\n", reason);
  File file = LittleFS.open("/reboot_reason.txt", "w");
  if (file) { file.print(reason); file.close(); }
  delay(500);
  watchdog_reboot(0, 0, 0);
  while (true) {}
}

// ============================================================
// WebUI — Dashboard HTML
// ============================================================
static const char HTML_PAGE[] = R"RELAY_HTML(
<!DOCTYPE html>
<html><head>
<meta charset=UTF-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>CCM Relay Node</title>
<style>
body{font-family:sans-serif;margin:16px;background:#0f1011;color:#f7f8f8}
h2{color:#5e6ad2;margin:0 0 10px}h3{color:#d0d6e0;margin:6px 0}
table{border-collapse:collapse;width:100%;margin:6px 0}
th,td{border:1px solid #2e2e2e;padding:5px 8px}
th{background:#191a1b;color:#d0d6e0}
.on{color:#66bb6a;font-weight:bold}.off{color:#ef5350}
.bon{background:#43a047;color:#fff;border:none;padding:4px 8px;border-radius:3px;cursor:pointer}
.bof{background:#e53935;color:#fff;border:none;padding:4px 8px;border-radius:3px;cursor:pointer}
.sec{background:#191a1b;border-radius:6px;padding:12px;margin:8px 0}
input[type=number]{width:55px;padding:3px;background:#1a1a1f;color:#eee;border:1px solid #3e3e44;border-radius:3px}
a{color:#d0d6e0}
.ccm{color:#ffa726;font-size:0.85em}
</style>
</head><body>
<h2>CCM Relay Node (UECS)</h2>
<div class=sec id=sys></div>
<div class=sec id=net></div>
<div class=sec id=devstat></div>
<div class=sec>
<h3>Relay / CCM Mapping</h3>
<table><tr><th>CH</th><th>CCM Type</th><th>Room</th><th>State</th><th>Control</th></tr>
<tbody id=rtbl></tbody></table>
</div>
<div class=sec>
<h3>Digital Input</h3>
<table><tr><th>CH</th><th>State</th></tr>
<tbody id=dtbl></tbody></table>
</div>
<div class=sec id=sens></div>
<script>
function relay(ch,v){
  fetch('/api/relay/'+ch,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({value:v})}).then(load);
}
function load(){
  fetch('/api/state').then(function(r){return r.json();}).then(function(d){
    document.getElementById('sys').innerHTML=
      '<b>Node:</b> '+d.node_id+' | <b>FW:</b> '+d.version+
      ' | <b>Protocol:</b> <span style="color:#ffa726">UECS-CCM</span>'+
      ' | <b>Uptime:</b> '+d.uptime+'s'+
      ' | <a href="/config">Network</a> | <a href="/ccm">CCM Config</a> | <a href="/ota">Firmware</a>';
    var mdnsHost=d.mdns_hostname?(' | <b>mDNS:</b> '+d.mdns_hostname):'';
    document.getElementById('net').innerHTML=
      '<h3>Network</h3><b>IP:</b> '+d.ip+
      ' | <b>GW:</b> '+d.gateway+mdnsHost+
      '<br><b>MAC:</b> '+d.mac+
      ' | <b>CCM:</b> 224.0.0.1:16520';
    document.getElementById('devstat').innerHTML=
      '<h3>Device Status</h3>'+
      '<b>I2C:</b> '+(d.sht40_ok?'<span class=on>SHT40</span>':'<span class=off>none</span>')+
      ' | <b>1-Wire:</b> '+(d.ds18b20_ok?'<span class=on>DS18B20</span>':'<span class=off>none</span>')+
      ' | <b>RS485:</b> '+(d.sen0575_ok?'<span class=on>SEN0575</span>':'<span class=off>none</span>');
    var rt='';
    var ccm=d.ccm_map||[];
    for(var i=0;i<8;i++){
      var s=(d.relay_state>>(i))&1;
      var m=ccm[i]||{};
      var tname=m.type||'(unmapped)';
      rt+='<tr><td>'+(i+1)+'</td><td>'+tname+' <span class=ccm>R'+
        (m.room||'-')+'/Rg'+(m.region||'-')+'/O'+(m.order||'-')+'</span></td>'+
        '<td>'+(m.room||'-')+'</td>'+
        '<td class="'+(s?'on':'off')+'">'+(s?'ON':'OFF')+'</td>'+
        '<td><button class=bon onclick="relay('+(i+1)+',1)">ON</button> '+
        '<button class=bof onclick="relay('+(i+1)+',0)">OFF</button></td></tr>';
    }
    document.getElementById('rtbl').innerHTML=rt;
    var dt='';
    for(var i=0;i<8;i++){
      var s=(d.di_state>>(i))&1;
      dt+='<tr><td>'+(i+1)+'</td><td class="'+(s?'on':'off')+'">'+(s?'ON':'OFF')+'</td></tr>';
    }
    document.getElementById('dtbl').innerHTML=dt;
    var sv='<h3>Sensors</h3>';
    if(d.sensor&&d.sensor.temp!==null)sv+='<b>SHT40 Temp:</b> '+d.sensor.temp.toFixed(1)+'C ';
    if(d.sensor&&d.sensor.hum!==null)sv+='<b>Hum:</b> '+d.sensor.hum.toFixed(1)+'% ';
    if(d.sensor&&d.sensor.ds18b20_temp!==null)sv+='<b>DS18B20:</b> '+d.sensor.ds18b20_temp.toFixed(1)+'C ';
    if(d.sensor&&d.sensor.temp===null&&d.sensor.hum===null&&d.sensor.ds18b20_temp===null)sv+='<span class=off>No sensors</span>';
    document.getElementById('sens').innerHTML=sv;
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

// ============================================================
// API: /api/state
// ============================================================
void sendAPIState(WiFiClient& client) {
  JsonDocument doc;
  doc["node_id"]    = nodeId;
  doc["node_name"]  = nodeName;
  doc["version"]    = FW_VERSION;
  doc["protocol"]   = "UECS-CCM";
  doc["uptime"]     = millis() / 1000;
  doc["ts"]         = getCurrentEpoch();
  doc["relay_state"] = relayState;

  // DI as bitmask
  uint8_t diBits = 0;
  for (int i = 0; i < 8; i++) {
    if (diState[i]) diBits |= (1 << i);
  }
  doc["di_state"]   = diBits;

  // CCM mapping
  JsonArray ccmArr = doc["ccm_map"].to<JsonArray>();
  for (int i = 0; i < 8; i++) {
    JsonObject m = ccmArr.add<JsonObject>();
    m["type"]     = ccmMap[i].ccmType;
    m["room"]     = ccmMap[i].room;
    m["region"]   = ccmMap[i].region;
    m["order"]    = ccmMap[i].order;
    m["priority"]     = ccmMap[i].priority;
    m["watchdog_sec"] = ccmMap[i].watchdog_sec;
    m["last_rx_ago"]  = lastCcmRx[i] > 0 ? (int)((millis() - lastCcmRx[i]) / 1000) : -1;
  }

  // Sensor
  JsonObject sensor = doc["sensor"].to<JsonObject>();
  if (!isnan(g_sht40_temp)) sensor["temp"] = round(g_sht40_temp * 10) / 10.0;
  else                       sensor["temp"] = nullptr;
  if (!isnan(g_sht40_hum))  sensor["hum"]  = round(g_sht40_hum * 10) / 10.0;
  else                       sensor["hum"]  = nullptr;
  if (!isnan(g_ds18b20_temp)) sensor["ds18b20_temp"] = round(g_ds18b20_temp * 10) / 10.0;
  else                         sensor["ds18b20_temp"] = nullptr;

  // Network
  doc["ip"]      = eth.localIP().toString();
  doc["subnet"]  = eth.subnetMask().toString();
  doc["gateway"] = eth.gatewayIP().toString();
  doc["dns"]     = eth.dnsIP().toString();
  doc["sht40_ok"]    = sht40_detected;
  doc["ds18b20_ok"]  = ds18b20_detected;
  doc["sen0575_ok"]  = sen0575_detected;

  // MAC
  {
    uint8_t mac[6];
    eth.macAddress(mac);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    doc["mac"] = macStr;
  }

  if (mdns_enabled) doc["mdns_hostname"] = mdnsHostname + ".local";
  else              doc["mdns_hostname"] = nullptr;

  char buffer[1536];
  serializeJson(doc, buffer);

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Access-Control-Allow-Origin: *");
  client.println("Connection: close");
  client.println();
  client.print(buffer);
}

// ============================================================
// API: /api/config
// ============================================================
void sendAPIConfig(WiFiClient& client) {
  JsonDocument doc;
  doc["node_id"]        = nodeId;
  doc["node_name"]      = nodeName;
  doc["mdns_hostname"]  = mdnsHostname;
  doc["mdns_enabled"]   = mdns_enabled;

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
// Relay POST (WebUI)
// ============================================================
void handleRelayPost(WiFiClient& client, int ch, const String& body) {
  if (ch < 1 || ch > 8) {
    client.println("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n");
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    client.println("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n");
    return;
  }

  int value = doc["value"] | -1;
  if (value == 1) setRelay(ch, true);
  else if (value == 0) setRelay(ch, false);

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Connection: close");
  client.println();
  client.printf("{\"ok\":true,\"ch\":%d}\n", ch);
}

// ============================================================
// Network Config Page (GET /config)
// ============================================================
void sendConfigPage(WiFiClient& client) {
  String curIp, curSubnet = DEFAULT_SUBNET, curGateway = DEFAULT_GATEWAY, curDns = DEFAULT_DNS;
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
  client.println("<title>Config - CCM Relay</title>");
  client.println("<style>body{font-family:sans-serif;margin:16px;background:#0f1011;color:#f7f8f8}");
  client.println("h2{color:#5e6ad2}.sec{background:#191a1b;border-radius:6px;padding:12px;margin:8px 0}");
  client.println("label{display:block;margin:6px 0 2px}");
  client.println("input[type=text],input[type=number]{width:220px;padding:4px;background:#1a1a1f;color:#eee;border:1px solid #3e3e44;border-radius:3px}");
  client.println("input[type=submit]{background:#1976d2;color:#fff;border:none;padding:8px 20px;border-radius:4px;cursor:pointer;margin-top:10px}");
  client.println("a{color:#d0d6e0}.note{color:#8a8f98;font-size:0.85em}</style></head><body>");
  client.println("<h2>Network Configuration</h2>");
  client.printf("<p><a href='/'>Dashboard</a> | <a href='/ccm'>CCM Config</a> | <a href='/ota'>Firmware</a> | Node: <b>%s</b></p>\n", nodeId.c_str());
  client.println("<form method=POST action=/api/config>");
  client.println("<div class=sec><h3>Identity</h3>");
  client.printf("<label>node_id<input type=text name=node_id value='%s'></label>\n", nodeId.c_str());
  client.printf("<label>node_name<input type=text name=node_name value='%s'></label>\n", nodeName.c_str());
  client.println("</div>");
  client.println("<div class=sec><h3>IP Address</h3>");
  client.println("<p class=note>Leave IP blank for DHCP.</p>");
  client.printf("<label>IP<input type=text name=ip value='%s' placeholder='DHCP'></label>\n", curIp.c_str());
  client.printf("<label>Subnet<input type=text name=subnet value='%s'></label>\n", curSubnet.c_str());
  client.printf("<label>Gateway<input type=text name=gateway value='%s'></label>\n", curGateway.c_str());
  client.printf("<label>DNS<input type=text name=dns value='%s'></label>\n", curDns.c_str());
  client.println("</div>");
  client.println("<div class=sec><h3>mDNS</h3>");
  client.printf("<label>Hostname<input type=text name=mdns_hostname value='%s' maxlength=32 placeholder='uecs-ccm-01'></label>\n", mdnsHostname.c_str());
  client.println("<p class=note>Access via <b>&lt;hostname&gt;.local</b></p>");
  client.printf("<label><input type=checkbox name=mdns_enabled value=1%s> Enable mDNS</label>\n",
                mdns_enabled ? " checked" : "");
  client.println("</div>");
  client.println("<input type=submit value='Save &amp; Reboot'>");
  client.println("</form></body></html>");
}

// ============================================================
// CCM Config Page (GET /ccm)
// ============================================================
void sendCcmConfigPage(WiFiClient& client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=UTF-8");
  client.println("Connection: close");
  client.println();
  client.println("<!DOCTYPE html><html><head>");
  client.println("<meta charset=UTF-8><meta name=viewport content='width=device-width,initial-scale=1'>");
  client.println("<title>CCM Config</title>");
  client.println("<style>body{font-family:sans-serif;margin:16px;background:#0f1011;color:#f7f8f8}");
  client.println("h2{color:#5e6ad2}h3{color:#d0d6e0}.sec{background:#191a1b;border-radius:6px;padding:12px;margin:8px 0}");
  client.println("table{border-collapse:collapse;width:100%}th,td{border:1px solid #2e2e2e;padding:4px 6px}");
  client.println("th{background:#191a1b;color:#d0d6e0}");
  client.println("select,input[type=number]{padding:3px;background:#1a1a1f;color:#eee;border:1px solid #3e3e44;border-radius:3px}");
  client.println("input[type=number]{width:55px}select{width:140px}");
  client.println("input[type=submit]{background:#1976d2;color:#fff;border:none;padding:8px 20px;border-radius:4px;cursor:pointer;margin-top:10px}");
  client.println("a{color:#d0d6e0}.note{color:#8a8f98;font-size:0.85em}</style></head><body>");
  client.println("<h2>CCM Channel Mapping</h2>");
  client.printf("<p><a href='/'>Dashboard</a> | <a href='/config'>Network</a> | <a href='/ota'>Firmware</a></p>\n");
  client.println("<p class=note>Map each relay channel to a UECS-CCM actuator type. Blank = unmapped (inactive).</p>");
  // Bulk Room/Region setter
  client.println("<div class=sec><h3>Bulk Set</h3>");
  client.println("<label>Room: <input type=number id=bulkRoom min=1 max=999 style='width:60px'></label>");
  client.println(" <button type=button onclick=\"var v=document.getElementById('bulkRoom').value;if(v)for(var i=0;i<8;i++)document.getElementsByName('room'+i)[0].value=v;\">Apply to All</button>");
  client.println(" &nbsp; <label>Region: <input type=number id=bulkRegion min=1 max=999 style='width:60px'></label>");
  client.println(" <button type=button onclick=\"var v=document.getElementById('bulkRegion').value;if(v)for(var i=0;i<8;i++)document.getElementsByName('region'+i)[0].value=v;\">Apply to All</button>");
  client.println("</div>");
  client.println("<form method=POST action=/api/ccm>");
  client.println("<table><tr><th>CH</th><th>CCM Type</th><th>Room</th><th>Region</th><th>Order</th><th>Priority</th><th>WDT(s)</th><th>DI Link</th></tr>");

  for (int i = 0; i < 8; i++) {
    client.printf("<tr><td>%d</td><td><select name=type%d>", i + 1, i);
    for (int t = 0; t < CCM_ACTUATOR_TYPES_COUNT; t++) {
      bool sel = (strcmp(ccmMap[i].ccmType, CCM_ACTUATOR_TYPES[t]) == 0);
      client.printf("<option value='%s'%s>%s</option>",
                    CCM_ACTUATOR_TYPES[t],
                    sel ? " selected" : "",
                    CCM_ACTUATOR_TYPES[t][0] == '\0' ? "(none)" : CCM_ACTUATOR_TYPES[t]);
    }
    client.printf("</select></td>");
    client.printf("<td><input type=number name=room%d value=%d min=1 max=999></td>", i, ccmMap[i].room);
    client.printf("<td><input type=number name=region%d value=%d min=1 max=999></td>", i, ccmMap[i].region);
    client.printf("<td><input type=number name=order%d value=%d min=1 max=99></td>", i, ccmMap[i].order);
    client.printf("<td><input type=number name=pri%d value=%d min=1 max=99></td>", i, ccmMap[i].priority);
    client.printf("<td><input type=number name=wdt%d value=%d min=0 max=3600></td>", i, ccmMap[i].watchdog_sec);
    // DI Link dropdown: -1=none, 0-7=DI1-8, + invert checkbox
    client.printf("<td><select name=dil%d>", i);
    client.printf("<option value=-1%s>none</option>", ccmMap[i].di_link < 0 ? " selected" : "");
    for (int d = 0; d < 8; d++) {
      client.printf("<option value=%d%s>DI%d</option>", d, ccmMap[i].di_link == d ? " selected" : "", d + 1);
    }
    client.printf("</select> <label><input type=checkbox name=dii%d value=1%s>inv</label></td></tr>",
                  i, ccmMap[i].di_invert ? " checked" : "");
  }

  client.println("</table>");
  client.println("<input type=submit value='Save CCM Mapping'>");
  client.println("</form>");
  client.println("<p class=note>Changes take effect immediately (no reboot required).</p>");
  client.println("</body></html>");
}

// ============================================================
// POST /api/config — save network config and reboot
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
  String newMdnsStr      = getField("mdns_enabled");

  if (newNodeId.length() == 0)       newNodeId       = nodeId;
  if (newNodeName.length() == 0)     newNodeName     = nodeName;
  if (newMdnsHostname.length() == 0) newMdnsHostname = mdnsHostname;
  if (newSubnet.length() == 0)       newSubnet       = DEFAULT_SUBNET;
  if (newGateway.length() == 0)      newGateway      = DEFAULT_GATEWAY;
  if (newDns.length() == 0)          newDns          = DEFAULT_DNS;
  bool newMdns = (newMdnsStr == "1");

  JsonDocument doc;
  doc["node_id"]        = newNodeId;
  doc["node_name"]      = newNodeName;
  doc["mdns_hostname"]  = newMdnsHostname;
  doc["mdns_enabled"]   = newMdns;
  if (newIp.length() > 0) {
    doc["ip"]      = newIp;
    doc["subnet"]  = newSubnet;
    doc["gateway"] = newGateway;
    doc["dns"]     = newDns;
  }

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

  File f = LittleFS.open("/config.json", "w");
  if (!f) {
    client.println("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n");
    return;
  }
  serializeJson(doc, f);
  f.close();

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=UTF-8");
  client.println("Connection: close");
  client.println();
  client.println("<p>Config saved. Rebooting...</p>");
  client.flush();
  delay(500);
  rebootWithReason("config_saved");
}

// ============================================================
// POST /api/ccm — save CCM mapping (no reboot)
// ============================================================
void handleCcmConfigPost(WiFiClient& client, const String& body) {
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

  for (int i = 0; i < 8; i++) {
    String t = getField(String("type") + i);
    strncpy(ccmMap[i].ccmType, t.c_str(), sizeof(ccmMap[i].ccmType) - 1);
    ccmMap[i].ccmType[sizeof(ccmMap[i].ccmType) - 1] = '\0';

    String r = getField(String("room") + i);
    if (r.length() > 0) ccmMap[i].room = r.toInt();

    String rg = getField(String("region") + i);
    if (rg.length() > 0) ccmMap[i].region = rg.toInt();

    String o = getField(String("order") + i);
    if (o.length() > 0) ccmMap[i].order = o.toInt();

    String p = getField(String("pri") + i);
    if (p.length() > 0) ccmMap[i].priority = p.toInt();

    String w = getField(String("wdt") + i);
    if (w.length() > 0) ccmMap[i].watchdog_sec = w.toInt();

    String dl = getField(String("dil") + i);
    if (dl.length() > 0) ccmMap[i].di_link = dl.toInt();

    // Checkbox: present=1, absent=not in form data
    String di = getField(String("dii") + i);
    ccmMap[i].di_invert = (di == "1");
  }

  saveCcmMapping();

  // Redirect back to CCM config page
  client.println("HTTP/1.1 303 See Other");
  client.println("Location: /ccm");
  client.println("Connection: close");
  client.println();
}

// ============================================================
// OTA Firmware Update Page (GET /ota)
// ============================================================
void sendOTAPage(WiFiClient& client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");
  client.println();
  client.println("<!DOCTYPE html><html><head>");
  client.println("<meta charset=UTF-8><meta name=viewport content='width=device-width,initial-scale=1'>");
  client.println("<title>Firmware Update</title>");
  client.println("<style>body{font-family:sans-serif;margin:16px;background:#0f1011;color:#f7f8f8}");
  client.println("h2{color:#5e6ad2}.sec{background:#191a1b;border-radius:6px;padding:16px;margin:8px 0}");
  client.println("a{color:#d0d6e0}.note{color:#8a8f98;font-size:0.85em}");
  client.println("#prog{width:100%;height:24px;background:#2e2e2e;border-radius:4px;margin:10px 0;display:none}");
  client.println("#progBar{height:100%;background:#1976d2;border-radius:4px;width:0%;transition:width 0.3s}");
  client.println("#msg{margin:10px 0;font-weight:bold}");
  client.println("input[type=file]{margin:8px 0}");
  client.println("button{background:#1976d2;color:#fff;border:none;padding:8px 20px;border-radius:4px;cursor:pointer}");
  client.println("button:disabled{background:#555}</style></head><body>");
  client.println("<h2>Firmware Update</h2>");
  client.printf("<p><a href='/'>Dashboard</a> | <a href='/config'>Network</a> | <a href='/ccm'>CCM</a></p>\n");
  client.printf("<div class=sec><p>Current: <b>%s</b> v%s</p>\n", FW_NAME, FW_VERSION);
  client.println("<p class=note>Select a .bin firmware file compiled with arduino-cli.</p>");
  client.println("<input type=file id=fw accept='.bin'><br>");
  client.println("<button id=btn onclick=doOTA()>Upload &amp; Flash</button>");
  client.println("<div id=prog><div id=progBar></div></div>");
  client.println("<div id=msg></div></div>");
  client.println("<script>");
  client.println("function doOTA(){");
  client.println("var f=document.getElementById('fw').files[0];");
  client.println("if(!f){alert('Select a file');return;}");
  client.println("var btn=document.getElementById('btn');btn.disabled=true;");
  client.println("var msg=document.getElementById('msg');");
  client.println("var prog=document.getElementById('prog');prog.style.display='block';");
  client.println("var bar=document.getElementById('progBar');");
  client.println("msg.textContent='Uploading '+f.name+' ('+f.size+' bytes)...';");
  client.println("var xhr=new XMLHttpRequest();");
  client.println("xhr.open('POST','/api/ota',true);");
  client.println("xhr.setRequestHeader('Content-Type','application/octet-stream');");
  client.println("xhr.upload.onprogress=function(e){if(e.lengthComputable)bar.style.width=Math.round(e.loaded/e.total*100)+'%';};");
  client.println("xhr.onload=function(){");
  client.println("if(xhr.status==200){msg.textContent='Success! Rebooting...';bar.style.width='100%';bar.style.background='#4caf50';");
  client.println("setTimeout(function(){window.location='/';},8000);}");
  client.println("else{msg.textContent='Error: '+xhr.responseText;btn.disabled=false;bar.style.background='#f44336';}};");
  client.println("xhr.onerror=function(){msg.textContent='Upload failed (connection lost). Device may be rebooting...';");
  client.println("setTimeout(function(){window.location='/';},8000);};");
  client.println("xhr.send(f);}");
  client.println("</script></body></html>");
}

// ============================================================
// OTA Upload Handler (POST /api/ota)
// Receives raw binary firmware via Content-Type: application/octet-stream
// ============================================================
void handleOTAUpload(WiFiClient& client, int contentLength) {
  if (contentLength <= 0 || contentLength > 8 * 1024 * 1024) {
    client.println("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nInvalid size");
    return;
  }

  Serial.printf("[OTA] Starting update, size=%d bytes\n", contentLength);

  if (!Update.begin(contentLength, U_FLASH)) {
    Serial.printf("[OTA] Update.begin failed: %d\n", Update.getError());
    client.println("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\nUpdate.begin failed");
    return;
  }

  uint8_t buf[4096];
  size_t written = 0;
  unsigned long lastActivity = millis();

  while (written < (size_t)contentLength) {
    watchdog_update();

    int avail = client.available();
    if (avail > 0) {
      int toRead = min(avail, (int)sizeof(buf));
      int rd = client.readBytes(buf, toRead);
      if (rd > 0) {
        size_t wr = Update.write(buf, rd);
        if (wr != (size_t)rd) {
          Serial.printf("[OTA] Write mismatch: rd=%d wr=%zu\n", rd, wr);
          client.println("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\nWrite failed");
          Update.end();
          return;
        }
        written += rd;
        lastActivity = millis();
        if ((written % 65536) < (size_t)rd) {
          Serial.printf("[OTA] %zu / %d bytes\n", written, contentLength);
        }
      }
    } else if (millis() - lastActivity > 30000) {
      Serial.println("[OTA] Timeout waiting for data");
      client.println("HTTP/1.1 408 Request Timeout\r\nConnection: close\r\n\r\nTimeout");
      Update.end();
      return;
    } else {
      delay(1);
    }
  }

  if (Update.end(true)) {
    Serial.printf("[OTA] Success! MD5: %s\n", Update.md5String().c_str());
    client.println("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nOK");
    client.flush();
    delay(500);
    client.stop();
    delay(1000);
    rebootWithReason("ota_update");
  } else {
    Serial.printf("[OTA] end() failed: %d\n", Update.getError());
    client.print("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\nUpdate failed: err=");
    client.println(Update.getError());
  }
}

// ============================================================
// Web Router
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

  // OTA: stream directly, do NOT buffer body into String
  if (method == "POST" && path == "/api/ota") {
    handleOTAUpload(client, contentLength);
    delay(1);
    client.stop();
    return;
  }

  String body;
  if (contentLength > 0) {
    unsigned long bt = millis();
    while ((int)body.length() < contentLength && (millis() - bt) < 1000) {
      if (client.available()) body += (char)client.read();
    }
  }

  if (method == "GET" && (path == "/" || path == "/index.html")) {
    sendDashboard(client);
  } else if (method == "GET" && path == "/api/state") {
    sendAPIState(client);
  } else if (method == "GET" && path == "/api/config") {
    sendAPIConfig(client);
  } else if (method == "GET" && path == "/config") {
    sendConfigPage(client);
  } else if (method == "GET" && path == "/ccm") {
    sendCcmConfigPage(client);
  } else if (method == "GET" && path == "/ota") {
    sendOTAPage(client);
  } else if (method == "POST" && path == "/api/config") {
    handleConfigPost(client, body);
  } else if (method == "POST" && path == "/api/ccm") {
    handleCcmConfigPost(client, body);
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
  { unsigned long t = millis(); while (!Serial && millis() - t < 3000) delay(10); }  // USB-CDC ready (3s timeout)
  Serial.printf("=== %s v%s ===\n", FW_NAME, FW_VERSION);
  Serial.println("Board: RP2350-POE-ETH-8DI-8RO");
  Serial.println("Protocol: UECS-CCM (UDP 224.0.0.1:16520)");

  initRelaysOff();

  for (int i = 0; i < 8; i++) {
    pinMode(DI_PINS[i], INPUT_PULLUP);
  }
  attachInterrupt(digitalPinToInterrupt(DI_PINS[0]), diPulseISR1, FALLING);
  attachInterrupt(digitalPinToInterrupt(DI_PINS[1]), diPulseISR2, FALLING);
  for (int i = 2; i < 8; i++) {
    attachInterrupt(digitalPinToInterrupt(DI_PINS[i]), diISR, CHANGE);
  }

  Wire1.setSDA(I2C_SDA);
  Wire1.setSCL(I2C_SCL);
  Wire1.begin();
  delay(200);

  if (!LittleFS.begin()) {
    Serial.println("LittleFS: formatting...");
    LittleFS.format();
    if (!LittleFS.begin()) {
      Serial.println("WARNING: LittleFS unavailable");
    }
  }

  loadConfig();
  loadCcmMapping();

  Serial.printf("Node=%s\n", nodeId.c_str());
  Serial.printf("[BOOT] hostname: %s.local\n", mdnsHostname.c_str());

  initEthernet();
  syncNTP();
  scanI2CSensors();

  // DS18B20 (OneWire on GPIO3)
  ds18b20.begin();
  if (ds18b20.getDeviceCount() > 0) {
    ds18b20_detected = true;
    ds18b20.setResolution(12);
    Serial.printf("DS18B20: %d device(s) on GPIO%d\n", ds18b20.getDeviceCount(), ONEWIRE_PIN);
  } else {
    Serial.println("DS18B20: not found (continuing)");
  }

  // RGB LED
  rgbLED.begin();
  rgbLED.setBrightness(30);  // 控えめ
  rgbLED.setPixelColor(0, rgbLED.Color(0, 0, 50));  // 起動中=青
  rgbLED.show();

  readSensors();
  initRS485();

  // CCM UDP: join multicast group
  ccmUDP.beginMulticast(CCM_MULTICAST, CCM_PORT);
  Serial.printf("CCM: joined %s:%d\n",
                CCM_MULTICAST.toString().c_str(), CCM_PORT);

  // mDNS
  if (mdns_enabled) {
    if (MDNS.begin(mdnsHostname.c_str())) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("mDNS: %s.local\n", mdnsHostname.c_str());
    }
  }

  webServer.begin();
  Serial.printf("WebUI: http://%s\n", eth.localIP().toString().c_str());

  readDI();

  // Watchdog
  watchdog_enable(HW_WDT_TIMEOUT_MS, true);
  swWdtStart();

  Serial.println("=== Setup complete ===\n");
}

// ============================================================
// Main Loop
// ============================================================
void loop() {
  loopCount++;

  watchdog_update();
  swWdtFeed();

  // [STATUS] 30秒毎デバッグ出力
  if (millis() - last_status >= 30000UL) {
    Serial.printf("[STATUS] ip:%s up:%lus\n",
                  eth.localIP().toString().c_str(),
                  millis() / 1000);
    last_status = millis();
  }

  if (millis() >= REBOOT_INTERVAL) {
    rebootWithReason("periodic_reboot");
  }

  if (!eth.connected()) {
    rebootWithReason("eth_disconnected");
  }

  if (mdns_enabled) MDNS.update();

  handleWebClient();

  // CCM receive (non-blocking)
  ccmReceive();

  // Duration auto-off
  unsigned long now = millis();
  for (int i = 0; i < 8; i++) {
    if (relayDurationEnd[i] > 0 && now >= relayDurationEnd[i]) {
      setRelay(i + 1, false);
      relayDurationEnd[i] = 0;
      Serial.printf("CH%d auto-OFF\n", i + 1);
    }
  }

  // CCM watchdog: 無通信タイマーでリレー強制OFF
  for (int i = 0; i < 8; i++) {
    if (ccmMap[i].watchdog_sec > 0 && lastCcmRx[i] > 0 &&
        (relayState & (1 << i)) &&
        (now - lastCcmRx[i]) >= (unsigned long)ccmMap[i].watchdog_sec * 1000UL) {
      setRelay(i + 1, false);
      lastCcmRx[i] = 0;
      Serial.printf("[WATCHDOG] CH%d OFF — no CCM for %ds\n", i + 1, ccmMap[i].watchdog_sec);
    }
  }

  // DI interrupt
  if (diInterruptFlag && (now - diLastDebounce >= DI_DEBOUNCE_MS)) {
    diInterruptFlag = false;
    diLastDebounce  = now;
    if (readDI()) {
      // DI→リレー連動
      for (int i = 0; i < 8; i++) {
        if (ccmMap[i].di_link < 0 || ccmMap[i].di_link > 7) continue;
        bool di_on = diState[ccmMap[i].di_link];
        bool target = ccmMap[i].di_invert ? !di_on : di_on;
        bool current = (relayState >> i) & 1;
        if (target != current) {
          setRelay(i + 1, target);
          Serial.printf("[DI-LINK] DI%d=%s → CH%d %s\n",
                        ccmMap[i].di_link + 1, di_on ? "ON" : "OFF",
                        i + 1, target ? "ON" : "OFF");
        }
      }
    }
  }

  // Periodic: sensor read + CCM broadcast
  static unsigned long lastBroadcast = 0;
  if (now - lastBroadcast >= (unsigned long)CCM_SEND_INTERVAL * 1000UL) {
    lastBroadcast = now;

    readSensors();
    pollDrainSensor();
    ccmSendStates();

    Serial.printf("[%d] relay=0x%02X epoch=%lu uptime=%lus\n",
                  loopCount, relayState, getCurrentEpoch(), millis() / 1000);
  }

  // NTP re-sync
  static unsigned long lastNtpSync = 0;
  if (now - lastNtpSync >= NTP_SYNC_INTERVAL) {
    lastNtpSync = now;
    syncNTP();
  }

  // Serial command handler — type "status" or "?" to get IP/mDNS/uptime
  static String serialCmd = "";
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      serialCmd.trim();
      if (serialCmd == "status" || serialCmd == "?" || serialCmd == "ip") {
        Serial.printf("[INFO] hostname: %s.local\n", mdnsHostname.c_str());
        Serial.printf("[INFO] ip: %s gw: %s mask: %s\n",
                      eth.localIP().toString().c_str(),
                      eth.gatewayIP().toString().c_str(),
                      eth.subnetMask().toString().c_str());
        byte mac[6]; eth.macAddress(mac);
        Serial.printf("[INFO] mac: %02X:%02X:%02X:%02X:%02X:%02X\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        Serial.printf("[INFO] uptime: %lus relay: 0x%02X\n",
                      millis() / 1000, relayState);
        Serial.printf("[INFO] mdns: %s eth: %s\n",
                      mdns_enabled ? "OK" : "OFF",
                      eth.connected() ? "OK" : "DISC");
      } else if (serialCmd == "help") {
        Serial.println("Commands: status/ip/? help reboot");
      } else if (serialCmd == "reboot") {
        rebootWithReason("serial_cmd");
      } else if (serialCmd.length() > 0) {
        Serial.printf("Unknown: '%s' (type 'help')\n", serialCmd.c_str());
      }
      serialCmd = "";
    } else {
      serialCmd += c;
    }
  }

  // RGB LED status: 緑=正常, 赤=Ethernet断, 黄=リレーON中, 青=起動直後
  static unsigned long lastLedUpdate = 0;
  if (now - lastLedUpdate >= 1000) {
    lastLedUpdate = now;
    if (!eth.connected()) {
      rgbLED.setPixelColor(0, rgbLED.Color(80, 0, 0));    // 赤=Ethernet断
    } else if (relayState > 0) {
      rgbLED.setPixelColor(0, rgbLED.Color(60, 40, 0));   // 黄=リレー稼働中
    } else {
      rgbLED.setPixelColor(0, rgbLED.Color(0, 50, 0));    // 緑=正常待機
    }
    rgbLED.show();
  }

  delay(50);
}
