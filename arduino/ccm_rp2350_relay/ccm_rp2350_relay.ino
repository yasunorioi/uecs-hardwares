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

// ========== Greenhouse Local Control ==========
// 温度ベース比例制御。CCM受信優先 → 途絶時ローカルフォールバック
struct GreenhouseCtrl {
  bool   enabled;        // ローカル制御有効
  int    ch;             // 制御対象リレーch (0-7, -1=none)
  float  temp_open;      // 開始温度 (この温度でデューティ>0%)
  float  temp_full;      // 全開温度 (この温度でデューティ100%)
  int    cycle_sec;      // 制御周期 秒 (e.g. 60 = 30sON+30sOFF at 50%)
  int    sensor_src;     // 0=SHT40, 1=DS18B20
};

const int GH_CTRL_SLOTS = 4;  // 最大4ルール
GreenhouseCtrl ghCtrl[GH_CTRL_SLOTS];

// ランタイム状態
struct GhRuntime {
  unsigned long cycleStart;  // 現在サイクルの開始時刻
  float         lastTemp;    // 最後に使った温度
  float         duty;        // 現在のデューティ比 (0.0-1.0)
  bool          active;      // 現在リレーON中か
};
GhRuntime ghRun[GH_CTRL_SLOTS];

// ========== Solar Irrigation Control ==========
// 積算日射量ベース灌水。PVSS-03 (0-1V=0-1000W/m²) → ADS1110 I2C ADC
struct IrrigationCtrl {
  bool   enabled;
  int    relay_ch;        // 灌水リレーch (0-7, -1=none)
  float  threshold_mj;    // 積算日射量閾値 (MJ/m²) — 到達で灌水開始
  int    duration_sec;    // 灌水時間 (秒)
  float  min_wm2;         // この日射量未満は積算しない (夜間ノイズ除外)
};

const int IRRI_SLOTS = 2;  // 最大2ルール (e.g. 点滴+ミスト)
IrrigationCtrl irriCtrl[IRRI_SLOTS];

// ランタイム状態
struct IrriRuntime {
  float         accum_mj;       // 現在の積算日射量 (MJ/m²)
  bool          irrigating;     // 灌水中フラグ
  unsigned long irri_start;     // 灌水開始時刻 (millis)
  unsigned long last_sample;    // 最終サンプル時刻 (millis)
  int           today_count;    // 本日の灌水回数
};
IrriRuntime irriRun[IRRI_SLOTS];

// ========== Dew Prevention (結露対策) ==========
// 日の出前に循環扇/暖房を起動して結露を防止
struct DewPreventionCtrl {
  bool   enabled;
  float  latitude;         // 緯度 (度、北半球+)
  float  longitude;        // 経度 (度、東経+)
  int    timezone_h;       // UTC offset (JST=9)
  int    before_sunrise_min; // 日の出の何分前に開始 (default 30)
  int    after_sunrise_min;  // 日の出の何分後に停止 (default 60)
  int    fan_relay_ch;     // 循環扇リレーch (0-7, -1=無効)
  int    heater_relay_ch;  // 暖房リレーch (0-7, -1=無効)
};

DewPreventionCtrl dewCtrl;

// ランタイム
struct DewRuntime {
  int   sunrise_min;       // 今日の日の出時刻 (ローカル時、0時からの分)
  int   last_calc_day;     // 最後に計算した日 (day_of_year)
  bool  active;            // 結露対策稼働中
};
DewRuntime dewRun = {0, -1, false};

// ========== Temp Rate Guard (温度急変対策) ==========
// 温度変化率が閾値を超えたらリレー制御
struct TempRateGuard {
  bool   enabled;
  float  rate_threshold;   // ℃/分 の閾値 (e.g. 2.0 = 2℃/分以上で発動)
  int    fan_relay_ch;     // 換気扇リレーch (0-7, -1=無効)
  int    sensor_src;       // 0=SHT40, 1=DS18B20
  int    hold_sec;         // 発動後の最小保持時間 (秒)
};

TempRateGuard rateGuard;

// ランタイム
struct RateRuntime {
  float  prev_temp;        // 前回温度
  unsigned long prev_time; // 前回時刻 (millis)
  float  current_rate;     // 現在の変化率 (℃/分)
  bool   active;           // ガード発動中
  unsigned long active_since; // 発動開始時刻
};
RateRuntime rateRun = {NAN, 0, 0.0, false, 0};

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

// ========== ADS1110 ADC (M5Stack ADC Unit V1.1 / PVSS Solar Sensor) ==========
const uint8_t ADS1110_ADDR     = 0x48;  // default (ADDR=GND)
const uint8_t ADS1110_CFG_CONT_16BIT = 0x0C;  // continuous, 8SPS, gain=1, 16bit
bool  ads1110_detected  = false;
float g_solar_wm2       = NAN;  // instantaneous solar radiation (W/m²)

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
void loadGreenhouseConfig();
void saveGreenhouseConfig();
void greenhouseControl(unsigned long now);
void sendGreenhousePage(WiFiClient& client);
void handleGreenhousePost(WiFiClient& client, const String& body);
void loadIrrigationConfig();
void saveIrrigationConfig();
void irrigationControl(unsigned long now);
void sendIrrigationPage(WiFiClient& client);
void handleIrrigationPost(WiFiClient& client, const String& body);
float readADS1110();
int calcSunriseMinLocal(int dayOfYear, float lat, float lon, int tz_h);
void loadDewConfig();
void saveDewConfig();
void dewPreventionControl(unsigned long now);
void loadRateGuardConfig();
void saveRateGuardConfig();
void tempRateGuardControl(unsigned long now);
void sendProtectionPage(WiFiClient& client);
void handleProtectionPost(WiFiClient& client, const String& body);

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
          if (SENSOR_REGISTRY[i].type == SENSOR_ADS1110) ads1110_detected = true;
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
  if (ads1110_detected) {
    // Configure: continuous conversion, 8 SPS, PGA gain=1
    Wire1.beginTransmission(ADS1110_ADDR);
    Wire1.write(ADS1110_CFG_CONT_16BIT);
    Wire1.endTransmission();
    Serial.println("ADS1110 initialized (solar ADC)");
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

  // ADS1110 → Solar radiation (PVSS-03: 0-1V = 0-1000 W/m²)
  if (ads1110_detected) {
    float v = readADS1110();
    if (!isnan(v)) {
      // PVSS-03: 0-1V linear → 0-1000 W/m²
      float wm2 = v * 1000.0;
      if (wm2 < 0.0) wm2 = 0.0;
      if (wm2 > 2000.0) wm2 = NAN;  // sanity check
      g_solar_wm2 = wm2;
    }
  }
}

// ============================================================
// ADS1110 (M5Stack ADC Unit V1.1) — 16bit signed, Vref=2.048V
// ============================================================
float readADS1110() {
  if (Wire1.requestFrom(ADS1110_ADDR, (uint8_t)3) != 3) return NAN;
  uint8_t hi  = Wire1.read();
  uint8_t lo  = Wire1.read();
  uint8_t cfg = Wire1.read();
  (void)cfg;
  int16_t raw = ((int16_t)hi << 8) | lo;
  // Vref=2.048V, gain=1, 16bit signed (max=+32767)
  float voltage = (float)raw / 32767.0 * 2.048;
  if (voltage < -0.01) return NAN;
  return voltage;
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

  // Solar radiation as InRadiation (ADS1110 + PVSS-03)
  if (ads1110_detected && !isnan(g_solar_wm2)) {
    int room = (ccmMap[0].ccmType[0] != '\0') ? ccmMap[0].room : 2;
    xml += "<DATA type=\"InRadiation.cMC\" room=\"";
    xml += room;
    xml += "\" region=\"11\" order=\"1\" priority=\"29\" lv=\"S\" cast=\"uni\">";
    char solBuf[8];
    dtostrf(g_solar_wm2, 1, 1, solBuf);
    xml += solBuf;
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

    // CCM sensor reception: InRadiation → use as solar input for irrigation
    if (strcmp(baseType, "InRadiation") == 0 && !ads1110_detected) {
      float wm2 = atof(valBuf);
      if (wm2 >= 0.0 && wm2 <= 2000.0) {
        g_solar_wm2 = wm2;
        Serial.printf("CCM RX: InRadiation=%.1f W/m2 (room=%d)\n", wm2, room);
      }
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
    ccmMap[i].order    = i + 1;  // CH1=1, CH2=2, ..., CH8=8
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
// Greenhouse Local Control — Config & Logic
// ============================================================
void loadGreenhouseConfig() {
  for (int i = 0; i < GH_CTRL_SLOTS; i++) {
    ghCtrl[i].enabled   = false;
    ghCtrl[i].ch        = -1;
    ghCtrl[i].temp_open = 25.0;
    ghCtrl[i].temp_full = 30.0;
    ghCtrl[i].cycle_sec = 60;
    ghCtrl[i].sensor_src = 0;
    ghRun[i] = {0, NAN, 0.0, false};
  }
  if (!LittleFS.exists("/gh_ctrl.json")) return;
  File f = LittleFS.open("/gh_ctrl.json", "r");
  if (!f) return;
  JsonDocument doc;
  if (deserializeJson(doc, f)) { f.close(); return; }
  f.close();
  JsonArray arr = doc["rules"].as<JsonArray>();
  int idx = 0;
  for (JsonObject r : arr) {
    if (idx >= GH_CTRL_SLOTS) break;
    ghCtrl[idx].enabled    = r["enabled"]    | false;
    ghCtrl[idx].ch         = r["ch"]         | -1;
    ghCtrl[idx].temp_open  = r["temp_open"]  | 25.0;
    ghCtrl[idx].temp_full  = r["temp_full"]  | 30.0;
    ghCtrl[idx].cycle_sec  = r["cycle_sec"]  | 60;
    ghCtrl[idx].sensor_src = r["sensor_src"] | 0;
    idx++;
  }
  Serial.printf("Greenhouse: %d rules loaded\n", idx);
}

void saveGreenhouseConfig() {
  JsonDocument doc;
  JsonArray arr = doc["rules"].to<JsonArray>();
  for (int i = 0; i < GH_CTRL_SLOTS; i++) {
    JsonObject r = arr.add<JsonObject>();
    r["enabled"]    = ghCtrl[i].enabled;
    r["ch"]         = ghCtrl[i].ch;
    r["temp_open"]  = ghCtrl[i].temp_open;
    r["temp_full"]  = ghCtrl[i].temp_full;
    r["cycle_sec"]  = ghCtrl[i].cycle_sec;
    r["sensor_src"] = ghCtrl[i].sensor_src;
  }
  File f = LittleFS.open("/gh_ctrl.json", "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
  Serial.println("Greenhouse config saved");
}

void greenhouseControl(unsigned long now) {
  for (int i = 0; i < GH_CTRL_SLOTS; i++) {
    if (!ghCtrl[i].enabled || ghCtrl[i].ch < 0 || ghCtrl[i].ch > 7) continue;
    int ch = ghCtrl[i].ch;

    // CCM受信があるchはスキップ（CCM優先）
    if (lastCcmRx[ch] > 0 &&
        (now - lastCcmRx[ch]) < (unsigned long)ccmMap[ch].watchdog_sec * 1000UL) {
      continue;
    }

    // 温度取得
    float temp = NAN;
    if (ghCtrl[i].sensor_src == 0 && !isnan(g_sht40_temp)) {
      temp = g_sht40_temp;
    } else if (ghCtrl[i].sensor_src == 1 && !isnan(g_ds18b20_temp)) {
      temp = g_ds18b20_temp;
    }
    if (isnan(temp)) continue;  // センサー無し→制御しない

    ghRun[i].lastTemp = temp;

    // デューティ比計算 (比例制御)
    float duty = 0.0;
    if (temp >= ghCtrl[i].temp_full) {
      duty = 1.0;
    } else if (temp > ghCtrl[i].temp_open) {
      duty = (temp - ghCtrl[i].temp_open) / (ghCtrl[i].temp_full - ghCtrl[i].temp_open);
    }
    ghRun[i].duty = duty;

    // サイクル制御 (デューティ比でON/OFF切替)
    unsigned long cycleDur = (unsigned long)ghCtrl[i].cycle_sec * 1000UL;
    if (cycleDur == 0) cycleDur = 60000;
    if (ghRun[i].cycleStart == 0) ghRun[i].cycleStart = now;
    unsigned long elapsed = now - ghRun[i].cycleStart;
    if (elapsed >= cycleDur) {
      ghRun[i].cycleStart = now;
      elapsed = 0;
    }

    unsigned long onDur = (unsigned long)(duty * cycleDur);
    bool shouldBeOn = (duty > 0.01) && (elapsed < onDur);

    if (shouldBeOn != ghRun[i].active) {
      setRelay(ch + 1, shouldBeOn);
      ghRun[i].active = shouldBeOn;
      Serial.printf("[GH] rule%d CH%d %s (%.1fC duty=%.0f%%)\n",
                    i + 1, ch + 1, shouldBeOn ? "ON" : "OFF", temp, duty * 100);
    }
  }
}

// ============================================================
// Solar Irrigation Control — Config & Logic
// ============================================================
void loadIrrigationConfig() {
  for (int i = 0; i < IRRI_SLOTS; i++) {
    irriCtrl[i].enabled      = false;
    irriCtrl[i].relay_ch     = -1;
    irriCtrl[i].threshold_mj = 0.5;   // 0.5 MJ/m² default
    irriCtrl[i].duration_sec = 120;    // 2分 default
    irriCtrl[i].min_wm2      = 50.0;  // 50 W/m² 未満は積算しない
    irriRun[i] = {0.0, false, 0, 0, 0};
  }
  if (!LittleFS.exists("/irri_ctrl.json")) return;
  File f = LittleFS.open("/irri_ctrl.json", "r");
  if (!f) return;
  JsonDocument doc;
  if (deserializeJson(doc, f)) { f.close(); return; }
  f.close();
  JsonArray arr = doc["rules"].as<JsonArray>();
  int idx = 0;
  for (JsonObject r : arr) {
    if (idx >= IRRI_SLOTS) break;
    irriCtrl[idx].enabled      = r["enabled"]      | false;
    irriCtrl[idx].relay_ch     = r["relay_ch"]      | -1;
    irriCtrl[idx].threshold_mj = r["threshold_mj"]  | 0.5;
    irriCtrl[idx].duration_sec = r["duration_sec"]  | 120;
    irriCtrl[idx].min_wm2      = r["min_wm2"]       | 50.0;
    idx++;
  }
  Serial.printf("Irrigation: %d rules loaded\n", idx);
}

void saveIrrigationConfig() {
  JsonDocument doc;
  JsonArray arr = doc["rules"].to<JsonArray>();
  for (int i = 0; i < IRRI_SLOTS; i++) {
    JsonObject r = arr.add<JsonObject>();
    r["enabled"]      = irriCtrl[i].enabled;
    r["relay_ch"]     = irriCtrl[i].relay_ch;
    r["threshold_mj"] = irriCtrl[i].threshold_mj;
    r["duration_sec"] = irriCtrl[i].duration_sec;
    r["min_wm2"]      = irriCtrl[i].min_wm2;
  }
  File f = LittleFS.open("/irri_ctrl.json", "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
  Serial.println("Irrigation config saved");
}

void irrigationControl(unsigned long now) {
  if (!ads1110_detected || isnan(g_solar_wm2)) return;

  for (int i = 0; i < IRRI_SLOTS; i++) {
    if (!irriCtrl[i].enabled || irriCtrl[i].relay_ch < 0) continue;
    int ch = irriCtrl[i].relay_ch;

    // 灌水中 → 時間経過で停止
    if (irriRun[i].irrigating) {
      if ((now - irriRun[i].irri_start) >= (unsigned long)irriCtrl[i].duration_sec * 1000UL) {
        setRelay(ch + 1, false);
        irriRun[i].irrigating = false;
        Serial.printf("[IRRI] rule%d CH%d OFF (done, accum=%.3f MJ, count=%d)\n",
                      i + 1, ch + 1, irriRun[i].accum_mj, irriRun[i].today_count);
        irriRun[i].accum_mj = 0.0;  // リセット、再積算開始
      }
      continue;  // 灌水中は積算しない
    }

    // 積算 (SENSOR_INTERVAL秒ごとにreadSensorsが呼ばれる前提)
    if (irriRun[i].last_sample == 0) {
      irriRun[i].last_sample = now;
      continue;
    }
    unsigned long dt_ms = now - irriRun[i].last_sample;
    if (dt_ms < 5000) continue;  // 最低5秒間隔
    irriRun[i].last_sample = now;

    if (g_solar_wm2 >= irriCtrl[i].min_wm2) {
      // W/m² × 秒 → J/m² → MJ/m²
      float dt_sec = dt_ms / 1000.0;
      irriRun[i].accum_mj += (g_solar_wm2 * dt_sec) / 1000000.0;
    }

    // 閾値到達 → 灌水開始
    if (irriRun[i].accum_mj >= irriCtrl[i].threshold_mj) {
      setRelay(ch + 1, true);
      irriRun[i].irrigating = true;
      irriRun[i].irri_start = now;
      irriRun[i].today_count++;
      Serial.printf("[IRRI] rule%d CH%d ON (accum=%.3f MJ >= %.3f, #%d)\n",
                    i + 1, ch + 1, irriRun[i].accum_mj,
                    irriCtrl[i].threshold_mj, irriRun[i].today_count);
    }
  }
}

// ============================================================
// NOAA Sunrise Calculation (solareqns.PDF)
// ============================================================
int calcSunriseMinLocal(int dayOfYear, float lat, float lon, int tz_h) {
  // Fractional year (γ) in radians
  float gamma = 2.0 * PI / 365.0 * (dayOfYear - 1);

  // Equation of time (minutes)
  float eqtime = 229.18 * (0.000075 + 0.001868 * cos(gamma) - 0.032077 * sin(gamma)
                 - 0.014615 * cos(2 * gamma) - 0.040849 * sin(2 * gamma));

  // Solar declination (radians)
  float decl = 0.006918 - 0.399912 * cos(gamma) + 0.070257 * sin(gamma)
               - 0.006758 * cos(2 * gamma) + 0.000907 * sin(2 * gamma)
               - 0.002697 * cos(3 * gamma) + 0.00148 * sin(3 * gamma);

  // Hour angle for sunrise (degrees)
  float latRad = lat * PI / 180.0;
  float zenith = 90.833 * PI / 180.0;  // atmospheric refraction correction
  float cosHA = (cos(zenith) / (cos(latRad) * cos(decl))) - tan(latRad) * tan(decl);

  if (cosHA > 1.0) return -1;   // no sunrise (polar night)
  if (cosHA < -1.0) return -1;  // no sunset (midnight sun)

  float ha = acos(cosHA) * 180.0 / PI;  // degrees, positive = sunrise

  // Sunrise UTC in minutes
  float sunriseUTC = 720.0 - 4.0 * (lon + ha) - eqtime;

  // Convert to local time
  int sunriseLocal = (int)(sunriseUTC + tz_h * 60.0);
  if (sunriseLocal < 0) sunriseLocal += 1440;
  if (sunriseLocal >= 1440) sunriseLocal -= 1440;
  return sunriseLocal;
}

// ============================================================
// Dew Prevention — Config & Logic
// ============================================================
void loadDewConfig() {
  dewCtrl.enabled           = false;
  dewCtrl.latitude          = 43.0;   // 北海道デフォルト
  dewCtrl.longitude         = 141.3;
  dewCtrl.timezone_h        = 9;      // JST
  dewCtrl.before_sunrise_min = 30;
  dewCtrl.after_sunrise_min  = 60;
  dewCtrl.fan_relay_ch      = -1;
  dewCtrl.heater_relay_ch   = -1;

  if (!LittleFS.exists("/dew_ctrl.json")) return;
  File f = LittleFS.open("/dew_ctrl.json", "r");
  if (!f) return;
  JsonDocument doc;
  if (deserializeJson(doc, f)) { f.close(); return; }
  f.close();
  dewCtrl.enabled           = doc["enabled"]    | false;
  dewCtrl.latitude          = doc["lat"]        | 43.0;
  dewCtrl.longitude         = doc["lon"]        | 141.3;
  dewCtrl.timezone_h        = doc["tz"]         | 9;
  dewCtrl.before_sunrise_min = doc["before_min"] | 30;
  dewCtrl.after_sunrise_min  = doc["after_min"]  | 60;
  dewCtrl.fan_relay_ch      = doc["fan_ch"]     | -1;
  dewCtrl.heater_relay_ch   = doc["heater_ch"]  | -1;
  Serial.printf("Dew: lat=%.2f lon=%.2f tz=%d before=%d after=%d\n",
                dewCtrl.latitude, dewCtrl.longitude, dewCtrl.timezone_h,
                dewCtrl.before_sunrise_min, dewCtrl.after_sunrise_min);
}

void saveDewConfig() {
  JsonDocument doc;
  doc["enabled"]    = dewCtrl.enabled;
  doc["lat"]        = dewCtrl.latitude;
  doc["lon"]        = dewCtrl.longitude;
  doc["tz"]         = dewCtrl.timezone_h;
  doc["before_min"] = dewCtrl.before_sunrise_min;
  doc["after_min"]  = dewCtrl.after_sunrise_min;
  doc["fan_ch"]     = dewCtrl.fan_relay_ch;
  doc["heater_ch"]  = dewCtrl.heater_relay_ch;
  File f = LittleFS.open("/dew_ctrl.json", "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
  Serial.println("Dew config saved");
}

void dewPreventionControl(unsigned long now) {
  if (!dewCtrl.enabled) return;
  unsigned long epoch = getCurrentEpoch();
  if (epoch == 0) return;  // NTP未同期

  // ローカル時刻 (分)
  int localSec = (epoch + dewCtrl.timezone_h * 3600L) % 86400L;
  int localMin = localSec / 60;

  // 日の出時刻を1日1回再計算
  int dayOfYear = ((epoch + dewCtrl.timezone_h * 3600L) / 86400L) % 365 + 1;
  if (dayOfYear != dewRun.last_calc_day) {
    dewRun.sunrise_min = calcSunriseMinLocal(dayOfYear, dewCtrl.latitude,
                                              dewCtrl.longitude, dewCtrl.timezone_h);
    dewRun.last_calc_day = dayOfYear;
    Serial.printf("[DEW] day=%d sunrise=%d:%02d local\n",
                  dayOfYear, dewRun.sunrise_min / 60, dewRun.sunrise_min % 60);
  }

  if (dewRun.sunrise_min < 0) return;  // 極夜

  int startMin = dewRun.sunrise_min - dewCtrl.before_sunrise_min;
  int endMin   = dewRun.sunrise_min + dewCtrl.after_sunrise_min;
  if (startMin < 0) startMin += 1440;

  // 時間帯内か判定
  bool inWindow;
  if (startMin < endMin) {
    inWindow = (localMin >= startMin && localMin < endMin);
  } else {
    // 日をまたぐ場合 (e.g. 23:30-05:30)
    inWindow = (localMin >= startMin || localMin < endMin);
  }

  if (inWindow && !dewRun.active) {
    dewRun.active = true;
    if (dewCtrl.fan_relay_ch >= 0 && dewCtrl.fan_relay_ch <= 7)
      setRelay(dewCtrl.fan_relay_ch + 1, true);
    if (dewCtrl.heater_relay_ch >= 0 && dewCtrl.heater_relay_ch <= 7)
      setRelay(dewCtrl.heater_relay_ch + 1, true);
    Serial.printf("[DEW] ON — sunrise=%d:%02d now=%d:%02d\n",
                  dewRun.sunrise_min / 60, dewRun.sunrise_min % 60,
                  localMin / 60, localMin % 60);
  } else if (!inWindow && dewRun.active) {
    dewRun.active = false;
    if (dewCtrl.fan_relay_ch >= 0 && dewCtrl.fan_relay_ch <= 7)
      setRelay(dewCtrl.fan_relay_ch + 1, false);
    if (dewCtrl.heater_relay_ch >= 0 && dewCtrl.heater_relay_ch <= 7)
      setRelay(dewCtrl.heater_relay_ch + 1, false);
    Serial.printf("[DEW] OFF — window ended\n");
  }
}

// ============================================================
// Temp Rate Guard — Config & Logic
// ============================================================
void loadRateGuardConfig() {
  rateGuard.enabled        = false;
  rateGuard.rate_threshold = 2.0;   // 2℃/分
  rateGuard.fan_relay_ch   = -1;
  rateGuard.sensor_src     = 0;     // SHT40
  rateGuard.hold_sec       = 120;   // 2分保持

  if (!LittleFS.exists("/rate_ctrl.json")) return;
  File f = LittleFS.open("/rate_ctrl.json", "r");
  if (!f) return;
  JsonDocument doc;
  if (deserializeJson(doc, f)) { f.close(); return; }
  f.close();
  rateGuard.enabled        = doc["enabled"]   | false;
  rateGuard.rate_threshold = doc["threshold"]  | 2.0;
  rateGuard.fan_relay_ch   = doc["fan_ch"]     | -1;
  rateGuard.sensor_src     = doc["sensor_src"] | 0;
  rateGuard.hold_sec       = doc["hold_sec"]   | 120;
  Serial.printf("RateGuard: threshold=%.1f℃/min fan=CH%d hold=%ds\n",
                rateGuard.rate_threshold, rateGuard.fan_relay_ch + 1, rateGuard.hold_sec);
}

void saveRateGuardConfig() {
  JsonDocument doc;
  doc["enabled"]    = rateGuard.enabled;
  doc["threshold"]  = rateGuard.rate_threshold;
  doc["fan_ch"]     = rateGuard.fan_relay_ch;
  doc["sensor_src"] = rateGuard.sensor_src;
  doc["hold_sec"]   = rateGuard.hold_sec;
  File f = LittleFS.open("/rate_ctrl.json", "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
  Serial.println("RateGuard config saved");
}

void tempRateGuardControl(unsigned long now) {
  if (!rateGuard.enabled || rateGuard.fan_relay_ch < 0) return;

  float temp = (rateGuard.sensor_src == 0) ? g_sht40_temp : g_ds18b20_temp;
  if (isnan(temp)) return;

  // 初回
  if (isnan(rateRun.prev_temp) || rateRun.prev_time == 0) {
    rateRun.prev_temp = temp;
    rateRun.prev_time = now;
    return;
  }

  // 10秒以上経過で変化率計算
  unsigned long dt_ms = now - rateRun.prev_time;
  if (dt_ms < 10000) return;

  float dt_min = dt_ms / 60000.0;
  rateRun.current_rate = (temp - rateRun.prev_temp) / dt_min;
  rateRun.prev_temp = temp;
  rateRun.prev_time = now;

  // 急上昇検知 → 換気扇ON
  if (!rateRun.active && rateRun.current_rate >= rateGuard.rate_threshold) {
    rateRun.active = true;
    rateRun.active_since = now;
    setRelay(rateGuard.fan_relay_ch + 1, true);
    Serial.printf("[RATE] ON — %.2f℃/min >= %.1f threshold\n",
                  rateRun.current_rate, rateGuard.rate_threshold);
  }

  // 保持時間経過 かつ 変化率が閾値未満 → OFF
  if (rateRun.active) {
    bool holdDone = (now - rateRun.active_since) >= (unsigned long)rateGuard.hold_sec * 1000UL;
    bool rateLow  = rateRun.current_rate < rateGuard.rate_threshold;
    if (holdDone && rateLow) {
      rateRun.active = false;
      setRelay(rateGuard.fan_relay_ch + 1, false);
      Serial.printf("[RATE] OFF — rate=%.2f℃/min, hold done\n", rateRun.current_rate);
    }
  }
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
<div class=sec id=gh></div>
<div class=sec id=irri></div>
<div class=sec id=prot></div>
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
      ' | <a href="/ccm">CCM</a> | <a href="/greenhouse">Greenhouse</a> | <a href="/irrigation">Irrigation</a> | <a href="/protection">Protection</a> | <a href="/config">Network</a> | <a href="/ota">FW</a>';
    var mdnsHost=d.mdns_hostname?(' | <b>mDNS:</b> '+d.mdns_hostname):'';
    document.getElementById('net').innerHTML=
      '<h3>Network</h3><b>IP:</b> '+d.ip+
      ' | <b>GW:</b> '+d.gateway+mdnsHost+
      '<br><b>MAC:</b> '+d.mac+
      ' | <b>CCM:</b> 224.0.0.1:16520';
    document.getElementById('devstat').innerHTML=
      '<h3>Device Status</h3>'+
      '<b>I2C:</b> '+(d.sht40_ok?'<span class=on>SHT40</span>':'<span class=off>none</span>')+
      ' | <b>ADC:</b> '+(d.ads1110_ok?'<span class=on>ADS1110</span>':'<span class=off>none</span>')+
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
    if(d.sensor&&d.sensor.solar_wm2!==null)sv+='<b>Solar:</b> '+d.sensor.solar_wm2.toFixed(1)+' W/m&sup2; ';
    if(d.sensor&&d.sensor.temp===null&&d.sensor.hum===null&&d.sensor.ds18b20_temp===null&&d.sensor.solar_wm2===null)sv+='<span class=off>No sensors</span>';
    document.getElementById('sens').innerHTML=sv;
    var gh=d.greenhouse||[];
    var anyGh=false;
    for(var i=0;i<gh.length;i++){if(gh[i].enabled)anyGh=true;}
    if(anyGh){
      var gv='<h3>Greenhouse Control</h3><table><tr><th>Rule</th><th>CH</th><th>Sensor</th><th>Temp</th><th>Range</th><th>Duty</th><th>State</th></tr>';
      for(var i=0;i<gh.length;i++){
        var g=gh[i];
        if(!g.enabled)continue;
        gv+='<tr><td>'+(i+1)+'</td><td>CH'+(g.ch+1)+'</td><td>'+g.sensor+'</td>';
        gv+='<td>'+(g.temp!==null?g.temp.toFixed(1)+'C':'-')+'</td>';
        gv+='<td>'+g.temp_open+'-'+g.temp_full+'C</td>';
        gv+='<td>'+g.duty+'%</td>';
        gv+='<td class='+(g.active?'on':'off')+'>'+(g.active?'ON':'OFF')+'</td></tr>';
      }
      gv+='</table><p><a href="/greenhouse">Settings</a></p>';
      document.getElementById('gh').innerHTML=gv;
    } else {
      document.getElementById('gh').innerHTML='<h3>Greenhouse Control</h3><span class=off>No rules active</span> — <a href="/greenhouse">Configure</a>';
    }
    var ir=d.irrigation||[];
    var anyIr=false;
    for(var i=0;i<ir.length;i++){if(ir[i].enabled)anyIr=true;}
    if(anyIr){
      var iv='<h3>Solar Irrigation</h3>';
      if(d.sensor&&d.sensor.solar_wm2!==null)iv+='<b>Solar:</b> '+d.sensor.solar_wm2.toFixed(1)+' W/m&sup2; | ';
      iv+='<table><tr><th>Rule</th><th>CH</th><th>Accum</th><th>Threshold</th><th>State</th><th>Today</th></tr>';
      for(var i=0;i<ir.length;i++){var r=ir[i];if(!r.enabled)continue;
        iv+='<tr><td>'+(i+1)+'</td><td>CH'+(r.relay_ch+1)+'</td>';
        iv+='<td>'+r.accum_mj.toFixed(3)+' MJ</td><td>'+r.threshold_mj+' MJ</td>';
        iv+='<td class='+(r.irrigating?'on':'off')+'>'+(r.irrigating?'WATERING':'Accum.')+'</td>';
        iv+='<td><b>'+r.today_count+'</b></td></tr>';}
      iv+='</table><p><a href="/irrigation">Settings</a></p>';
      document.getElementById('irri').innerHTML=iv;
    } else {
      document.getElementById('irri').innerHTML='<h3>Solar Irrigation</h3><span class=off>Not configured</span> — <a href="/irrigation">Configure</a>';
    }
    var p=d.protection||{};
    var pv='<h3>Protection</h3>';
    var anyP=false;
    if(p.dew&&p.dew.enabled){anyP=true;
      pv+='<b>Dew:</b> '+(p.dew.active?'<span class=on>ACTIVE</span>':'Standby');
      if(p.dew.sunrise)pv+=' (sunrise '+p.dew.sunrise+') ';
      pv+=' | ';}
    if(p.rate&&p.rate.enabled){anyP=true;
      pv+='<b>Rate Guard:</b> '+(p.rate.active?'<span class=on>ACTIVE</span>':'Normal');
      if(p.rate.current_rate!==null)pv+=' ('+p.rate.current_rate.toFixed(1)+'C/min)';}
    if(anyP){pv+=' — <a href="/protection">Settings</a>';
      document.getElementById('prot').innerHTML=pv;
    }else{
      document.getElementById('prot').innerHTML='<h3>Protection</h3><span class=off>Not configured</span> — <a href="/protection">Configure</a>';
    }
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
  if (!isnan(g_solar_wm2)) sensor["solar_wm2"] = round(g_solar_wm2 * 10) / 10.0;
  else                      sensor["solar_wm2"] = nullptr;

  // Network
  doc["ip"]      = eth.localIP().toString();
  doc["subnet"]  = eth.subnetMask().toString();
  doc["gateway"] = eth.gatewayIP().toString();
  doc["dns"]     = eth.dnsIP().toString();
  doc["sht40_ok"]    = sht40_detected;
  doc["ds18b20_ok"]  = ds18b20_detected;
  doc["sen0575_ok"]  = sen0575_detected;
  doc["ads1110_ok"]  = ads1110_detected;

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

  // Greenhouse control status
  JsonArray ghArr = doc["greenhouse"].to<JsonArray>();
  for (int i = 0; i < GH_CTRL_SLOTS; i++) {
    JsonObject g = ghArr.add<JsonObject>();
    g["enabled"]  = ghCtrl[i].enabled;
    g["ch"]       = ghCtrl[i].ch;
    g["temp_open"] = ghCtrl[i].temp_open;
    g["temp_full"] = ghCtrl[i].temp_full;
    g["cycle_sec"] = ghCtrl[i].cycle_sec;
    g["sensor"]   = ghCtrl[i].sensor_src == 0 ? "SHT40" : "DS18B20";
    if (!isnan(ghRun[i].lastTemp)) g["temp"] = round(ghRun[i].lastTemp * 10) / 10.0;
    else g["temp"] = nullptr;
    g["duty"]     = round(ghRun[i].duty * 100);
    g["active"]   = ghRun[i].active;
  }

  // Irrigation control status
  JsonArray irriArr = doc["irrigation"].to<JsonArray>();
  for (int i = 0; i < IRRI_SLOTS; i++) {
    JsonObject ir = irriArr.add<JsonObject>();
    ir["enabled"]      = irriCtrl[i].enabled;
    ir["relay_ch"]     = irriCtrl[i].relay_ch;
    ir["threshold_mj"] = irriCtrl[i].threshold_mj;
    ir["duration_sec"] = irriCtrl[i].duration_sec;
    ir["min_wm2"]      = irriCtrl[i].min_wm2;
    ir["accum_mj"]     = round(irriRun[i].accum_mj * 1000) / 1000.0;
    ir["irrigating"]   = irriRun[i].irrigating;
    ir["today_count"]  = irriRun[i].today_count;
    if (irriRun[i].irrigating) {
      ir["remaining_sec"] = irriCtrl[i].duration_sec -
        (int)((millis() - irriRun[i].irri_start) / 1000);
    }
  }

  // Protection status (Dew + Rate Guard)
  JsonObject prot = doc["protection"].to<JsonObject>();
  {
    JsonObject dew = prot["dew"].to<JsonObject>();
    dew["enabled"] = dewCtrl.enabled;
    dew["active"]  = dewRun.active;
    if (dewRun.sunrise_min >= 0) {
      char sr[6];
      snprintf(sr, sizeof(sr), "%d:%02d", dewRun.sunrise_min / 60, dewRun.sunrise_min % 60);
      dew["sunrise"] = sr;
    }
    JsonObject rate = prot["rate"].to<JsonObject>();
    rate["enabled"] = rateGuard.enabled;
    rate["active"]  = rateRun.active;
    if (!isnan(rateRun.prev_temp)) rate["current_rate"] = round(rateRun.current_rate * 100) / 100.0;
    else rate["current_rate"] = nullptr;
  }

  char buffer[4096];
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
  client.printf("<p><a href='/'>Dashboard</a> | <a href='/ccm'>CCM</a> | <a href='/greenhouse'>Greenhouse</a> | <a href='/irrigation'>Irrigation</a> | <a href='/protection'>Protection</a> | <a href='/config'>Network</a> | <a href='/ota'>FW</a></p>\n");
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
  client.printf("<p><a href='/'>Dashboard</a> | <a href='/ccm'>CCM</a> | <a href='/greenhouse'>Greenhouse</a> | <a href='/irrigation'>Irrigation</a> | <a href='/protection'>Protection</a> | <a href='/config'>Network</a> | <a href='/ota'>FW</a></p>\n");
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
// Greenhouse Control Page (GET /greenhouse)
// ============================================================
void sendGreenhousePage(WiFiClient& client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");
  client.println();
  client.println("<!DOCTYPE html><html><head>");
  client.println("<meta charset=UTF-8><meta name=viewport content='width=device-width,initial-scale=1'>");
  client.println("<title>Greenhouse Control</title>");
  client.println("<style>body{font-family:sans-serif;margin:16px;background:#0f1011;color:#f7f8f8}");
  client.println("h2{color:#5e6ad2}h3{color:#d0d6e0}.sec{background:#191a1b;border-radius:6px;padding:12px;margin:8px 0}");
  client.println("table{border-collapse:collapse;width:100%}th,td{border:1px solid #2e2e2e;padding:4px 6px}");
  client.println("th{background:#191a1b;color:#d0d6e0}");
  client.println("select,input[type=number]{padding:3px;background:#1a1a1f;color:#eee;border:1px solid #3e3e44;border-radius:3px}");
  client.println("input[type=number]{width:60px}select{width:80px}");
  client.println("input[type=submit]{background:#1976d2;color:#fff;border:none;padding:8px 20px;border-radius:4px;cursor:pointer;margin-top:10px}");
  client.println("a{color:#d0d6e0}.note{color:#8a8f98;font-size:0.85em}");
  client.println(".on{color:#66bb6a}.off{color:#ef5350}</style></head><body>");
  client.println("<h2>Greenhouse Control</h2>");
  client.printf("<p><a href='/'>Dashboard</a> | <a href='/ccm'>CCM</a> | <a href='/greenhouse'>Greenhouse</a> | <a href='/irrigation'>Irrigation</a> | <a href='/protection'>Protection</a> | <a href='/config'>Network</a> | <a href='/ota'>FW</a></p>\n");
  client.println("<p class=note>Temperature-based proportional relay control. CCM commands take priority when active.</p>");
  client.println("<div class=sec id=ghstat>Loading...</div>");
  client.println("<div class=sec id=ghrun>Loading...</div>");

  // Config form
  client.println("<form method=POST action=/api/greenhouse>");
  client.println("<table><tr><th>Rule</th><th>Enable</th><th>CH</th><th>Sensor</th><th>Open(C)</th><th>Full(C)</th><th>Cycle(s)</th></tr>");
  for (int i = 0; i < GH_CTRL_SLOTS; i++) {
    client.printf("<tr><td>%d</td>", i + 1);
    client.printf("<td><input type=checkbox name=en%d value=1%s></td>", i, ghCtrl[i].enabled ? " checked" : "");
    // CH select
    client.printf("<td><select name=ch%d>", i);
    client.printf("<option value=-1%s>-</option>", ghCtrl[i].ch < 0 ? " selected" : "");
    for (int c = 0; c < 8; c++) {
      client.printf("<option value=%d%s>CH%d</option>", c, ghCtrl[i].ch == c ? " selected" : "", c + 1);
    }
    client.printf("</select></td>");
    // Sensor select
    client.printf("<td><select name=ss%d>", i);
    client.printf("<option value=0%s>SHT40</option>", ghCtrl[i].sensor_src == 0 ? " selected" : "");
    client.printf("<option value=1%s>DS18B20</option>", ghCtrl[i].sensor_src == 1 ? " selected" : "");
    client.printf("</select></td>");
    client.printf("<td><input type=number name=to%d value=%.1f min=-10 max=60 step=0.5></td>", i, ghCtrl[i].temp_open);
    client.printf("<td><input type=number name=tf%d value=%.1f min=-10 max=60 step=0.5></td>", i, ghCtrl[i].temp_full);
    client.printf("<td><input type=number name=cy%d value=%d min=10 max=600></td></tr>", i, ghCtrl[i].cycle_sec);
  }
  client.println("</table>");
  client.println("<input type=submit value='Save'>");
  client.println("</form>");
  client.println("<p class=note>Open: relay starts at this temp. Full: 100% duty at this temp. Cycle: ON+OFF period in seconds.</p>");
  // Auto-refresh status
  client.println("<script>");
  client.println("function ghLoad(){");
  client.println("fetch('/api/state').then(function(r){return r.json();}).then(function(d){");
  // Sensor status
  client.println("var s='<h3>Sensors</h3>';");
  client.println("if(d.sht40_ok)s+='<b>SHT40:</b> <span class=on>'+(d.sensor.temp!==null?d.sensor.temp.toFixed(1)+'C':'OK')+'</span> ';");
  client.println("else s+='<b>SHT40:</b> <span class=off>none</span> ';");
  client.println("if(d.sensor.hum!==null)s+='<b>Hum:</b> '+d.sensor.hum.toFixed(1)+'% ';");
  client.println("if(d.ds18b20_ok)s+='| <b>DS18B20:</b> <span class=on>'+(d.sensor.ds18b20_temp!==null?d.sensor.ds18b20_temp.toFixed(1)+'C':'OK')+'</span>';");
  client.println("else s+='| <b>DS18B20:</b> <span class=off>none</span>';");
  client.println("document.getElementById('ghstat').innerHTML=s;");
  // Runtime table
  client.println("var gh=d.greenhouse||[];var h='<h3>Runtime</h3>';");
  client.println("var any=false;for(var i=0;i<gh.length;i++)if(gh[i].enabled)any=true;");
  client.println("if(any){");
  client.println("h+='<table><tr><th>Rule</th><th>CH</th><th>Sensor</th><th>Temp</th><th>Range</th><th>Duty</th><th>State</th></tr>';");
  client.println("for(var i=0;i<gh.length;i++){var g=gh[i];if(!g.enabled)continue;");
  client.println("h+='<tr><td>'+(i+1)+'</td><td>CH'+(g.ch+1)+'</td><td>'+g.sensor+'</td>';");
  client.println("h+='<td style=\"font-size:1.3em;font-weight:bold\">'+(g.temp!==null?g.temp.toFixed(1)+'C':'-')+'</td>';");
  client.println("h+='<td>'+g.temp_open+' - '+g.temp_full+'C</td>';");
  // Duty bar visualization
  client.println("h+='<td><div style=\"background:#2e2e2e;border-radius:3px;height:18px;width:80px;display:inline-block;vertical-align:middle\">';");
  client.println("h+='<div style=\"background:'+(g.duty>70?'#e53935':g.duty>30?'#ffa726':'#43a047')+';height:100%;width:'+g.duty+'%;border-radius:3px\"></div></div> '+g.duty+'%</td>';");
  client.println("h+='<td class='+(g.active?'on':'off')+'><b>'+(g.active?'ON':'OFF')+'</b></td></tr>';}");
  client.println("h+='</table>';}else{h+='<span class=off>No rules active</span>';}");
  client.println("document.getElementById('ghrun').innerHTML=h;");
  client.println("});}");
  client.println("ghLoad();setInterval(ghLoad,3000);");
  client.println("</script>");
  client.println("</body></html>");
}

// ============================================================
// POST /api/greenhouse
// ============================================================
void handleGreenhousePost(WiFiClient& client, const String& body) {
  auto getField = [&](const String& key) -> String {
    String search = key + "=";
    int idx = body.indexOf(search);
    if (idx < 0) return "";
    idx += search.length();
    int end = body.indexOf('&', idx);
    if (end < 0) end = body.length();
    return body.substring(idx, end);
  };

  for (int i = 0; i < GH_CTRL_SLOTS; i++) {
    String en = getField(String("en") + i);
    ghCtrl[i].enabled = (en == "1");

    String ch = getField(String("ch") + i);
    if (ch.length() > 0) ghCtrl[i].ch = ch.toInt();

    String ss = getField(String("ss") + i);
    if (ss.length() > 0) ghCtrl[i].sensor_src = ss.toInt();

    String to = getField(String("to") + i);
    if (to.length() > 0) ghCtrl[i].temp_open = to.toFloat();

    String tf = getField(String("tf") + i);
    if (tf.length() > 0) ghCtrl[i].temp_full = tf.toFloat();

    String cy = getField(String("cy") + i);
    if (cy.length() > 0) ghCtrl[i].cycle_sec = cy.toInt();

    // Reset runtime on config change
    ghRun[i].cycleStart = 0;
  }

  saveGreenhouseConfig();

  client.println("HTTP/1.1 303 See Other");
  client.println("Location: /greenhouse");
  client.println("Connection: close");
  client.println();
}

// ============================================================
// Solar Irrigation Page (GET /irrigation)
// ============================================================
void sendIrrigationPage(WiFiClient& client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");
  client.println();
  client.println("<!DOCTYPE html><html><head>");
  client.println("<meta charset=UTF-8><meta name=viewport content='width=device-width,initial-scale=1'>");
  client.println("<title>Solar Irrigation</title>");
  client.println("<style>body{font-family:sans-serif;margin:16px;background:#0f1011;color:#f7f8f8}");
  client.println("h2{color:#5e6ad2}h3{color:#d0d6e0}.sec{background:#191a1b;border-radius:6px;padding:12px;margin:8px 0}");
  client.println("table{border-collapse:collapse;width:100%}th,td{border:1px solid #2e2e2e;padding:4px 6px}");
  client.println("th{background:#191a1b;color:#d0d6e0}");
  client.println("select,input[type=number]{padding:3px;background:#1a1a1f;color:#eee;border:1px solid #3e3e44;border-radius:3px}");
  client.println("input[type=number]{width:70px}select{width:80px}");
  client.println("input[type=submit]{background:#1976d2;color:#fff;border:none;padding:8px 20px;border-radius:4px;cursor:pointer;margin-top:10px}");
  client.println("a{color:#d0d6e0}.note{color:#8a8f98;font-size:0.85em}");
  client.println(".on{color:#66bb6a}.off{color:#ef5350}");
  client.println(".bar{background:#2e2e2e;border-radius:3px;height:18px;width:120px;display:inline-block;vertical-align:middle}");
  client.println(".fill{height:100%;border-radius:3px}</style></head><body>");
  client.println("<h2>Solar Irrigation</h2>");
  client.printf("<p><a href='/'>Dashboard</a> | <a href='/ccm'>CCM</a> | <a href='/greenhouse'>Greenhouse</a> | <a href='/irrigation'>Irrigation</a> | <a href='/protection'>Protection</a> | <a href='/config'>Network</a> | <a href='/ota'>FW</a></p>\n");
  client.println("<p class=note>Accumulated solar radiation triggers irrigation. Requires ADS1110 + PVSS-03 on I2C Grove.</p>");
  client.println("<div class=sec id=solstat>Loading...</div>");
  client.println("<div class=sec id=irrirun>Loading...</div>");

  // Config form
  client.println("<h3>Settings</h3>");
  client.println("<form method=POST action=/api/irrigation>");
  client.println("<table><tr><th>Rule</th><th>Enable</th><th>Relay CH</th><th>Threshold(MJ/m&sup2;)</th><th>Duration(s)</th><th>Min W/m&sup2;</th></tr>");
  for (int i = 0; i < IRRI_SLOTS; i++) {
    client.printf("<tr><td>%d</td>", i + 1);
    client.printf("<td><input type=checkbox name=en%d value=1%s></td>", i, irriCtrl[i].enabled ? " checked" : "");
    client.printf("<td><select name=rc%d>", i);
    client.printf("<option value=-1%s>-</option>", irriCtrl[i].relay_ch < 0 ? " selected" : "");
    for (int c = 0; c < 8; c++) {
      client.printf("<option value=%d%s>CH%d</option>", c, irriCtrl[i].relay_ch == c ? " selected" : "", c + 1);
    }
    client.printf("</select></td>");
    client.printf("<td><input type=number name=th%d value=%.3f min=0.01 max=10 step=0.01></td>", i, irriCtrl[i].threshold_mj);
    client.printf("<td><input type=number name=du%d value=%d min=10 max=3600></td>", i, irriCtrl[i].duration_sec);
    client.printf("<td><input type=number name=mw%d value=%.0f min=0 max=500 step=10></td></tr>", i, irriCtrl[i].min_wm2);
  }
  client.println("</table>");
  client.println("<input type=submit value='Save'>");
  client.println("</form>");
  client.println("<p class=note>Threshold: accumulated solar energy to trigger irrigation (typical: 0.3-1.0 MJ/m&sup2;).<br>");
  client.println("Duration: how long irrigation runs per trigger.<br>");
  client.println("Min W/m&sup2;: ignore solar readings below this (nighttime noise filter, default 50).</p>");

  // Auto-refresh JS
  client.println("<script>");
  client.println("function irLoad(){");
  client.println("fetch('/api/state').then(function(r){return r.json();}).then(function(d){");
  // Solar sensor status
  client.println("var s='<h3>Solar Sensor</h3>';");
  client.println("if(d.ads1110_ok){");
  client.println("  var wm2=d.sensor.solar_wm2;");
  client.println("  s+='<b>ADS1110:</b> <span class=on>OK</span> | ';");
  client.println("  if(wm2!==null){");
  client.println("    s+='<b>Solar:</b> <span style=\"font-size:1.4em;font-weight:bold\">'+wm2.toFixed(1)+'</span> W/m&sup2;';");
  client.println("    var pct=Math.min(100,wm2/10);");
  client.println("    var col=wm2>600?'#ffa726':wm2>200?'#66bb6a':'#5e6ad2';");
  client.println("    s+=' <div class=bar><div class=fill style=\"background:'+col+';width:'+pct+'%\"></div></div>';");
  client.println("  } else s+='<span class=off>no reading</span>';");
  client.println("} else s+='<b>ADS1110:</b> <span class=off>not detected</span> — connect M5Stack ADC Unit to Grove I2C';");
  client.println("document.getElementById('solstat').innerHTML=s;");
  // Irrigation runtime
  client.println("var ir=d.irrigation||[];var h='<h3>Irrigation Status</h3>';");
  client.println("var any=false;for(var i=0;i<ir.length;i++)if(ir[i].enabled)any=true;");
  client.println("if(any){");
  client.println("h+='<table><tr><th>Rule</th><th>Relay</th><th>Accumulated</th><th>Threshold</th><th>Progress</th><th>State</th><th>Today</th></tr>';");
  client.println("for(var i=0;i<ir.length;i++){var r=ir[i];if(!r.enabled)continue;");
  client.println("var pct=Math.min(100,(r.accum_mj/r.threshold_mj)*100);");
  client.println("h+='<tr><td>'+(i+1)+'</td><td>CH'+(r.relay_ch+1)+'</td>';");
  client.println("h+='<td>'+r.accum_mj.toFixed(3)+' MJ/m&sup2;</td>';");
  client.println("h+='<td>'+r.threshold_mj+' MJ/m&sup2;</td>';");
  client.println("h+='<td><div class=bar><div class=fill style=\"background:'+(r.irrigating?'#42a5f5':'#43a047')+';width:'+pct+'%\"></div></div> '+pct.toFixed(0)+'%</td>';");
  client.println("h+='<td class='+(r.irrigating?'on':'off')+'><b>'+(r.irrigating?'WATERING'+(r.remaining_sec?' ('+r.remaining_sec+'s)':''):'Accumulating')+'</b></td>';");
  client.println("h+='<td><b>'+r.today_count+'</b></td></tr>';}");
  client.println("h+='</table>';}else{h+='<span class=off>No rules configured</span>';}");
  client.println("document.getElementById('irrirun').innerHTML=h;");
  client.println("});}");
  client.println("irLoad();setInterval(irLoad,3000);");
  client.println("</script>");
  client.println("</body></html>");
}

// ============================================================
// POST /api/irrigation
// ============================================================
void handleIrrigationPost(WiFiClient& client, const String& body) {
  auto getField = [&](const String& key) -> String {
    String search = key + "=";
    int idx = body.indexOf(search);
    if (idx < 0) return "";
    idx += search.length();
    int end = body.indexOf('&', idx);
    if (end < 0) end = body.length();
    return body.substring(idx, end);
  };

  for (int i = 0; i < IRRI_SLOTS; i++) {
    String en = getField(String("en") + i);
    irriCtrl[i].enabled = (en == "1");

    String rc = getField(String("rc") + i);
    if (rc.length() > 0) irriCtrl[i].relay_ch = rc.toInt();

    String th = getField(String("th") + i);
    if (th.length() > 0) irriCtrl[i].threshold_mj = th.toFloat();

    String du = getField(String("du") + i);
    if (du.length() > 0) irriCtrl[i].duration_sec = du.toInt();

    String mw = getField(String("mw") + i);
    if (mw.length() > 0) irriCtrl[i].min_wm2 = mw.toFloat();

    // Reset runtime on config change
    irriRun[i].accum_mj = 0.0;
    irriRun[i].last_sample = 0;
  }

  saveIrrigationConfig();

  client.println("HTTP/1.1 303 See Other");
  client.println("Location: /irrigation");
  client.println("Connection: close");
  client.println();
}

// ============================================================
// Protection Page (GET /protection) — Dew + Rate Guard
// ============================================================
void sendProtectionPage(WiFiClient& client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");
  client.println();
  client.println("<!DOCTYPE html><html><head>");
  client.println("<meta charset=UTF-8><meta name=viewport content='width=device-width,initial-scale=1'>");
  client.println("<title>Protection</title>");
  client.println("<style>body{font-family:sans-serif;margin:16px;background:#0f1011;color:#f7f8f8}");
  client.println("h2{color:#5e6ad2}h3{color:#d0d6e0}.sec{background:#191a1b;border-radius:6px;padding:12px;margin:8px 0}");
  client.println("table{border-collapse:collapse;width:100%}th,td{border:1px solid #2e2e2e;padding:4px 6px}");
  client.println("th{background:#191a1b;color:#d0d6e0}");
  client.println("select,input[type=number]{padding:3px;background:#1a1a1f;color:#eee;border:1px solid #3e3e44;border-radius:3px}");
  client.println("input[type=number]{width:70px}select{width:80px}");
  client.println("input[type=submit]{background:#1976d2;color:#fff;border:none;padding:8px 20px;border-radius:4px;cursor:pointer;margin-top:10px}");
  client.println("a{color:#d0d6e0}.note{color:#8a8f98;font-size:0.85em}");
  client.println(".on{color:#66bb6a}.off{color:#ef5350}fieldset{border:1px solid #3e3e44;border-radius:6px;padding:12px;margin:12px 0}");
  client.println("legend{color:#5e6ad2;font-weight:bold}</style></head><body>");
  client.println("<h2>Protection</h2>");
  client.printf("<p><a href='/'>Dashboard</a> | <a href='/ccm'>CCM</a> | <a href='/greenhouse'>Greenhouse</a> | <a href='/irrigation'>Irrigation</a> | <a href='/protection'>Protection</a> | <a href='/config'>Network</a> | <a href='/ota'>FW</a></p>\n");
  client.println("<div class=sec id=pstat>Loading...</div>");

  // Dew Prevention form
  client.println("<form method=POST action=/api/protection>");
  client.println("<fieldset><legend>Dew Prevention (結露対策)</legend>");
  client.println("<p class=note>Runs circulation fan / heater before sunrise to prevent condensation.</p>");
  client.println("<table>");
  client.printf("<tr><th>Enable</th><td><input type=checkbox name=dew_en value=1%s></td></tr>\n", dewCtrl.enabled ? " checked" : "");
  client.printf("<tr><th>Latitude</th><td><input type=number name=lat value=%.4f min=-90 max=90 step=0.01></td></tr>\n", dewCtrl.latitude);
  client.printf("<tr><th>Longitude</th><td><input type=number name=lon value=%.4f min=-180 max=180 step=0.01></td></tr>\n", dewCtrl.longitude);
  client.printf("<tr><th>Timezone (UTC+)</th><td><input type=number name=tz value=%d min=-12 max=14></td></tr>\n", dewCtrl.timezone_h);
  client.printf("<tr><th>Before sunrise (min)</th><td><input type=number name=bmin value=%d min=0 max=180></td></tr>\n", dewCtrl.before_sunrise_min);
  client.printf("<tr><th>After sunrise (min)</th><td><input type=number name=amin value=%d min=0 max=180></td></tr>\n", dewCtrl.after_sunrise_min);

  // Fan relay select
  client.printf("<tr><th>Fan relay</th><td><select name=dfan>");
  client.printf("<option value=-1%s>-</option>", dewCtrl.fan_relay_ch < 0 ? " selected" : "");
  for (int c = 0; c < 8; c++)
    client.printf("<option value=%d%s>CH%d</option>", c, dewCtrl.fan_relay_ch == c ? " selected" : "", c + 1);
  client.printf("</select></td></tr>\n");

  // Heater relay select
  client.printf("<tr><th>Heater relay</th><td><select name=dheat>");
  client.printf("<option value=-1%s>-</option>", dewCtrl.heater_relay_ch < 0 ? " selected" : "");
  for (int c = 0; c < 8; c++)
    client.printf("<option value=%d%s>CH%d</option>", c, dewCtrl.heater_relay_ch == c ? " selected" : "", c + 1);
  client.printf("</select></td></tr>\n");
  client.println("</table></fieldset>");

  // Temp Rate Guard form
  client.println("<fieldset><legend>Temp Rate Guard (温度急変対策)</legend>");
  client.println("<p class=note>Activates fan when temperature rises too fast (e.g. sudden sun exposure).</p>");
  client.println("<table>");
  client.printf("<tr><th>Enable</th><td><input type=checkbox name=rate_en value=1%s></td></tr>\n", rateGuard.enabled ? " checked" : "");
  client.printf("<tr><th>Threshold (C/min)</th><td><input type=number name=rthr value=%.1f min=0.5 max=10 step=0.1></td></tr>\n", rateGuard.rate_threshold);

  // Sensor select
  client.printf("<tr><th>Sensor</th><td><select name=rsrc>");
  client.printf("<option value=0%s>SHT40</option>", rateGuard.sensor_src == 0 ? " selected" : "");
  client.printf("<option value=1%s>DS18B20</option>", rateGuard.sensor_src == 1 ? " selected" : "");
  client.printf("</select></td></tr>\n");

  // Fan relay
  client.printf("<tr><th>Fan relay</th><td><select name=rfan>");
  client.printf("<option value=-1%s>-</option>", rateGuard.fan_relay_ch < 0 ? " selected" : "");
  for (int c = 0; c < 8; c++)
    client.printf("<option value=%d%s>CH%d</option>", c, rateGuard.fan_relay_ch == c ? " selected" : "", c + 1);
  client.printf("</select></td></tr>\n");

  client.printf("<tr><th>Hold time (s)</th><td><input type=number name=rhld value=%d min=30 max=600></td></tr>\n", rateGuard.hold_sec);
  client.println("</table></fieldset>");

  client.println("<input type=submit value='Save All'>");
  client.println("</form>");

  // Auto-refresh status
  client.println("<script>");
  client.println("function pLoad(){");
  client.println("fetch('/api/state').then(function(r){return r.json();}).then(function(d){");
  client.println("var p=d.protection||{};var s='<h3>Status</h3>';");
  client.println("if(p.dew){var dw=p.dew;");
  client.println("  s+='<b>Dew:</b> '+(dw.enabled?'<span class=on>Enabled</span>':'<span class=off>Disabled</span>');");
  client.println("  if(dw.sunrise)s+=' | <b>Sunrise:</b> '+dw.sunrise;");
  client.println("  s+=' | <b>State:</b> '+(dw.active?'<span class=on>ACTIVE</span>':'<span class=off>Standby</span>');");
  client.println("  s+='<br>';}");
  client.println("if(p.rate){var rt=p.rate;");
  client.println("  s+='<b>Rate Guard:</b> '+(rt.enabled?'<span class=on>Enabled</span>':'<span class=off>Disabled</span>');");
  client.println("  if(rt.current_rate!==null)s+=' | <b>Rate:</b> '+rt.current_rate.toFixed(2)+' C/min';");
  client.println("  s+=' | <b>State:</b> '+(rt.active?'<span class=on>ACTIVE</span>':'<span class=off>Normal</span>');}");
  client.println("document.getElementById('pstat').innerHTML=s;");
  client.println("});}");
  client.println("pLoad();setInterval(pLoad,3000);");
  client.println("</script>");
  client.println("</body></html>");
}

// ============================================================
// POST /api/protection
// ============================================================
void handleProtectionPost(WiFiClient& client, const String& body) {
  auto getField = [&](const String& key) -> String {
    String search = key + "=";
    int idx = body.indexOf(search);
    if (idx < 0) return "";
    idx += search.length();
    int end = body.indexOf('&', idx);
    if (end < 0) end = body.length();
    return body.substring(idx, end);
  };

  // Dew
  dewCtrl.enabled = (getField("dew_en") == "1");
  String lat = getField("lat");  if (lat.length() > 0) dewCtrl.latitude = lat.toFloat();
  String lon = getField("lon");  if (lon.length() > 0) dewCtrl.longitude = lon.toFloat();
  String tz  = getField("tz");   if (tz.length() > 0)  dewCtrl.timezone_h = tz.toInt();
  String bm  = getField("bmin"); if (bm.length() > 0)  dewCtrl.before_sunrise_min = bm.toInt();
  String am  = getField("amin"); if (am.length() > 0)  dewCtrl.after_sunrise_min = am.toInt();
  String df  = getField("dfan"); if (df.length() > 0)  dewCtrl.fan_relay_ch = df.toInt();
  String dh  = getField("dheat");if (dh.length() > 0)  dewCtrl.heater_relay_ch = dh.toInt();

  // Rate Guard
  rateGuard.enabled = (getField("rate_en") == "1");
  String rt = getField("rthr"); if (rt.length() > 0) rateGuard.rate_threshold = rt.toFloat();
  String rs = getField("rsrc"); if (rs.length() > 0) rateGuard.sensor_src = rs.toInt();
  String rf = getField("rfan"); if (rf.length() > 0) rateGuard.fan_relay_ch = rf.toInt();
  String rh = getField("rhld"); if (rh.length() > 0) rateGuard.hold_sec = rh.toInt();

  // Reset runtimes
  dewRun.last_calc_day = -1;  // force recalc
  dewRun.active = false;
  rateRun = {NAN, 0, 0.0, false, 0};

  saveDewConfig();
  saveRateGuardConfig();

  client.println("HTTP/1.1 303 See Other");
  client.println("Location: /protection");
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
  client.printf("<p><a href='/'>Dashboard</a> | <a href='/ccm'>CCM</a> | <a href='/greenhouse'>Greenhouse</a> | <a href='/irrigation'>Irrigation</a> | <a href='/protection'>Protection</a> | <a href='/config'>Network</a> | <a href='/ota'>FW</a></p>\n");
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
  } else if (method == "GET" && path == "/greenhouse") {
    sendGreenhousePage(client);
  } else if (method == "POST" && path == "/api/greenhouse") {
    handleGreenhousePost(client, body);
  } else if (method == "GET" && path == "/irrigation") {
    sendIrrigationPage(client);
  } else if (method == "POST" && path == "/api/irrigation") {
    handleIrrigationPost(client, body);
  } else if (method == "GET" && path == "/protection") {
    sendProtectionPage(client);
  } else if (method == "POST" && path == "/api/protection") {
    handleProtectionPost(client, body);
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
  loadGreenhouseConfig();
  loadIrrigationConfig();
  loadDewConfig();
  loadRateGuardConfig();

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

  // Greenhouse local control (every loop for duty cycle switching)
  greenhouseControl(now);

  // Solar irrigation control (accumulation + trigger)
  irrigationControl(now);

  // Dew prevention (sunrise-based)
  dewPreventionControl(now);

  // Temperature rate guard
  tempRateGuardControl(now);

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
