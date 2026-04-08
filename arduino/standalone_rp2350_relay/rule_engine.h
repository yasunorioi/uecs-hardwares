#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <math.h>

// Sensor types
enum SensorType : uint8_t {
  SENSOR_TEMP = 0,    // SHT40 temperature
  SENSOR_HUM,         // SHT40 humidity
  SENSOR_VPD,         // calculated from temp+hum
  SENSOR_RAIN,        // RS485 SEN0575
  SENSOR_DI1, SENSOR_DI2, SENSOR_DI3, SENSOR_DI4,
  SENSOR_DI5, SENSOR_DI6, SENSOR_DI7, SENSOR_DI8,
  SENSOR_COUNT
};

enum Condition : uint8_t { COND_GT = 0, COND_LT = 1 };

struct Rule {
  uint8_t    id;
  bool       enabled;
  char       name[25];      // UTF-8, up to 8 kanji
  SensorType sensor;
  Condition  cond;
  float      onValue;       // turn-ON threshold
  float      offValue;      // turn-OFF threshold (hysteresis)
  uint8_t    relayCh;       // 1-8
  bool       inverted;
  bool       active;        // runtime state: currently triggered?
};

#define MAX_RULES 16
#define RULES_PATH "/rules.json"

// Load rules from /rules.json on LittleFS.
// Returns number of rules loaded, or 0 on error.
static inline int loadRules(Rule rules[]) {
  File f = LittleFS.open(RULES_PATH, "r");
  if (!f) return 0;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return 0;

  JsonArray arr = doc["rules"].as<JsonArray>();
  if (!arr) return 0;

  int count = 0;
  for (JsonObject obj : arr) {
    if (count >= MAX_RULES) break;
    Rule &r = rules[count];
    r.id       = obj["id"]       | (uint8_t)count;
    r.enabled  = obj["enabled"]  | false;
    r.sensor   = (SensorType)(obj["sensor"] | 0);
    r.cond     = (Condition)(obj["cond"]     | 0);
    r.onValue  = obj["onValue"]  | 0.0f;
    r.offValue = obj["offValue"] | 0.0f;
    r.relayCh  = obj["relayCh"]  | (uint8_t)1;
    r.inverted = obj["inverted"] | false;
    r.active   = false;  // runtime state always resets on load

    const char *nm = obj["name"] | "";
    strncpy(r.name, nm, sizeof(r.name) - 1);
    r.name[sizeof(r.name) - 1] = '\0';

    count++;
  }
  return count;
}

// Save rules[] to /rules.json on LittleFS.
// Returns true on success.
static inline bool saveRules(Rule rules[], int count) {
  JsonDocument doc;
  JsonArray arr = doc["rules"].to<JsonArray>();

  for (int i = 0; i < count; i++) {
    Rule &r = rules[i];
    JsonObject obj = arr.add<JsonObject>();
    obj["id"]       = r.id;
    obj["enabled"]  = r.enabled;
    obj["name"]     = r.name;
    obj["sensor"]   = (uint8_t)r.sensor;
    obj["cond"]     = (uint8_t)r.cond;
    obj["onValue"]  = r.onValue;
    obj["offValue"] = r.offValue;
    obj["relayCh"]  = r.relayCh;
    obj["inverted"] = r.inverted;
    // active is runtime state, not persisted
  }

  File f = LittleFS.open(RULES_PATH, "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}

// Return current reading for the given sensor type.
// VPD formula: (1 - RH/100) * 0.6108 * exp(17.27 * T / (T + 237.3))  [kPa]
static inline float getSensorValue(SensorType type, float temp, float hum,
                                   float rain, bool di[8]) {
  switch (type) {
    case SENSOR_TEMP: return temp;
    case SENSOR_HUM:  return hum;
    case SENSOR_VPD: {
      float svp = 0.6108f * expf(17.27f * temp / (temp + 237.3f));
      return (1.0f - hum / 100.0f) * svp;
    }
    case SENSOR_RAIN: return rain;
    case SENSOR_DI1:  return di[0] ? 1.0f : 0.0f;
    case SENSOR_DI2:  return di[1] ? 1.0f : 0.0f;
    case SENSOR_DI3:  return di[2] ? 1.0f : 0.0f;
    case SENSOR_DI4:  return di[3] ? 1.0f : 0.0f;
    case SENSOR_DI5:  return di[4] ? 1.0f : 0.0f;
    case SENSOR_DI6:  return di[5] ? 1.0f : 0.0f;
    case SENSOR_DI7:  return di[6] ? 1.0f : 0.0f;
    case SENSOR_DI8:  return di[7] ? 1.0f : 0.0f;
    default:          return 0.0f;
  }
}

// Evaluate all enabled rules and populate relayRequest[] / relaySource[].
//   relayRequest[ch]  0=off, 1=on, 0xFF=no opinion  (ch is 0-indexed, maps to relayCh 1-8)
//   relaySource[ch]   rule ID that last wrote this channel
// Hysteresis:
//   COND_GT: active=true when val >= onValue; active=false when val <= offValue
//   COND_LT: active=true when val <= onValue; active=false when val >= offValue
// If inverted, the relay output is flipped (active->off, inactive->on).
// Last-write-wins when multiple rules target the same channel.
static inline void evaluateRules(Rule rules[], int count,
                                 float temp, float hum, float rain, bool di[8],
                                 uint8_t relayRequest[8], uint8_t relaySource[8]) {
  // Initialize: no opinion
  for (int ch = 0; ch < 8; ch++) {
    relayRequest[ch] = 0xFF;
    relaySource[ch]  = 0xFF;
  }

  for (int i = 0; i < count; i++) {
    Rule &r = rules[i];
    if (!r.enabled) continue;

    int ch = (int)r.relayCh - 1;  // convert 1-8 to 0-7
    if (ch < 0 || ch > 7) continue;

    float val = getSensorValue(r.sensor, temp, hum, rain, di);

    // Update active state with hysteresis
    if (r.cond == COND_GT) {
      if (val >= r.onValue)  r.active = true;
      if (val <= r.offValue) r.active = false;
    } else {  // COND_LT
      if (val <= r.onValue)  r.active = true;
      if (val >= r.offValue) r.active = false;
    }

    bool output = r.inverted ? !r.active : r.active;
    relayRequest[ch] = output ? 1 : 0;
    relaySource[ch]  = r.id;
  }
}
