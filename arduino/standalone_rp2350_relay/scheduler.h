#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

// Timer scheduler for standalone relay control
// Max 24 schedules, persisted to /schedule.json on LittleFS
// Uses ArduinoJson v7

struct Schedule {
  uint8_t  id;
  bool     enabled;
  char     name[25];     // UTF-8
  uint8_t  relayCh;      // 1-8
  uint8_t  hour;         // 0-23
  uint8_t  minute;       // 0-59
  uint16_t durationSec;  // max 65535 (~18h)
  uint8_t  dowMask;      // bitmask: bit0=Sun, bit1=Mon, ..., bit6=Sat. 127=everyday
  // runtime
  bool     running;
};

#define MAX_SCHEDULES 24
#define JST_OFFSET    32400   // 9 * 3600
#define SCHEDULE_PATH "/schedule.json"

// Load schedules from /schedule.json on LittleFS.
// Returns number of schedules loaded, or 0 on error.
static inline int loadSchedules(Schedule scheds[]) {
  File f = LittleFS.open(SCHEDULE_PATH, "r");
  if (!f) return 0;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return 0;

  JsonArray arr = doc["schedules"].as<JsonArray>();
  if (!arr) return 0;

  int count = 0;
  for (JsonObject obj : arr) {
    if (count >= MAX_SCHEDULES) break;
    Schedule &s = scheds[count];
    s.id          = obj["id"]          | (uint8_t)count;
    s.enabled     = obj["enabled"]     | false;
    s.relayCh     = obj["relayCh"]     | (uint8_t)1;
    s.hour        = obj["hour"]        | (uint8_t)0;
    s.minute      = obj["minute"]      | (uint8_t)0;
    s.durationSec = obj["durationSec"] | (uint16_t)0;
    s.dowMask     = obj["dowMask"]     | (uint8_t)127;
    s.running     = false;  // runtime state always resets on load

    const char *nm = obj["name"] | "";
    strncpy(s.name, nm, sizeof(s.name) - 1);
    s.name[sizeof(s.name) - 1] = '\0';

    count++;
  }
  return count;
}

// Save schedules[] to /schedule.json on LittleFS.
// Returns true on success.
static inline bool saveSchedules(Schedule scheds[], int count) {
  JsonDocument doc;
  JsonArray arr = doc["schedules"].to<JsonArray>();

  for (int i = 0; i < count; i++) {
    Schedule &s = scheds[i];
    JsonObject obj = arr.add<JsonObject>();
    obj["id"]          = s.id;
    obj["enabled"]     = s.enabled;
    obj["name"]        = s.name;
    obj["relayCh"]     = s.relayCh;
    obj["hour"]        = s.hour;
    obj["minute"]      = s.minute;
    obj["durationSec"] = s.durationSec;
    obj["dowMask"]     = s.dowMask;
    // running is runtime state, not persisted
  }

  File f = LittleFS.open(SCHEDULE_PATH, "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}

// Evaluate all enabled schedules against current UTC epoch and populate
// relayRequest[] / relaySource[].
//   relayRequest[ch]  0xFF=no opinion, 1=on, 0=off  (ch is 0-indexed, maps to relayCh 1-8)
//   relaySource[ch]   100 + schedule.id that last wrote this channel
//
// Time handling:
//   - utcEpoch is converted to JST by adding JST_OFFSET
//   - dowMask bit0=Sun, bit1=Mon, ..., bit6=Sat
//   - Midnight crossing: e.g. start=23:50 + 30min spans into next calendar day.
//     The window is expressed entirely in seconds-from-midnight for comparison.
//     When end wraps past 86400s, two checks are done: cur >= start OR cur < end-86400.
//   - Transition logging: Serial.println on started/ended edges
//   - Last-write-wins when multiple schedules target the same channel
static inline void evaluateSchedules(Schedule scheds[], int count,
                                     unsigned long utcEpoch,
                                     uint8_t relayRequest[8],
                                     uint8_t relaySource[8]) {
  // Initialize: no opinion
  for (int ch = 0; ch < 8; ch++) {
    relayRequest[ch] = 0xFF;
    relaySource[ch]  = 0xFF;
  }

  // Convert UTC epoch to JST
  unsigned long jstEpoch = utcEpoch + (unsigned long)JST_OFFSET;

  // Break into time components (no mktime/gmtime on bare metal; manual decomposition)
  // seconds within current day
  uint32_t daySeconds = (uint32_t)(jstEpoch % 86400UL);
  // days since epoch (1970-01-01 = Thursday)
  uint32_t days = (uint32_t)(jstEpoch / 86400UL);

  // hour and minute of current JST time
  uint8_t curHour   = (uint8_t)(daySeconds / 3600);
  uint8_t curMinute = (uint8_t)((daySeconds % 3600) / 60);

  // Day of week: 1970-01-01 was Thursday (wday=4). bit0=Sun(0)...bit6=Sat(6)
  // wday: 0=Sun ... 6=Sat
  uint8_t wday = (uint8_t)((days + 4) % 7);  // +4 because epoch day0 = Thursday

  uint32_t curSec = daySeconds;  // seconds since midnight JST

  for (int i = 0; i < count; i++) {
    Schedule &s = scheds[i];
    if (!s.enabled) continue;

    int ch = (int)s.relayCh - 1;  // convert 1-8 to 0-7
    if (ch < 0 || ch > 7) continue;

    // Check day-of-week match
    // For midnight-crossing schedules the window may have started yesterday,
    // so we also check yesterday's dowMask when the end wraps into today.
    uint32_t startSec = (uint32_t)s.hour * 3600UL + (uint32_t)s.minute * 60UL;
    uint32_t endSec   = startSec + (uint32_t)s.durationSec;

    bool active = false;

    if (endSec <= 86400UL) {
      // No midnight crossing: check today's dowMask
      if ((s.dowMask >> wday) & 0x01) {
        active = (curSec >= startSec && curSec < endSec);
      }
    } else {
      // Midnight crossing: window spans two calendar days.
      // Part A (today,  startSec ..< 86400): today's dowMask
      // Part B (next,   0 ..< endSec-86400): yesterday's dowMask (the start day)
      uint32_t wrapEnd = endSec - 86400UL;

      // Part A: today after start time — today must be in dowMask
      if ((s.dowMask >> wday) & 0x01) {
        if (curSec >= startSec) {
          active = true;
        }
      }
      // Part B: early morning before wrap end — previous day must be in dowMask
      if (!active && curSec < wrapEnd) {
        uint8_t prevWday = (wday == 0) ? 6 : wday - 1;
        if ((s.dowMask >> prevWday) & 0x01) {
          active = true;
        }
      }
    }

    // Track running state transitions
    if (active && !s.running) {
      s.running = true;
      Serial.print("[scheduler] ch");
      Serial.print(s.relayCh);
      Serial.print(" id=");
      Serial.print(s.id);
      Serial.print(" '");
      Serial.print(s.name);
      Serial.println("' started");
    } else if (!active && s.running) {
      s.running = false;
      Serial.print("[scheduler] ch");
      Serial.print(s.relayCh);
      Serial.print(" id=");
      Serial.print(s.id);
      Serial.print(" '");
      Serial.print(s.name);
      Serial.println("' ended");
    }

    // Write to relay arrays (last-write-wins for same channel)
    relayRequest[ch] = active ? 1 : 0;
    relaySource[ch]  = (uint8_t)(100 + s.id);
  }
}
