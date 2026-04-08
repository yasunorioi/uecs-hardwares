#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

// Event log — circular buffer, persisted to /log.json on LittleFS
// Max 100 entries. Ring buffer with wrap.
// Uses ArduinoJson v7
// Flush strategy: write to LittleFS every 10 events or on explicit request.

enum EventType : uint8_t {
  EVT_RELAY    = 0,  // relay toggled
  EVT_SENSOR,        // sensor reading
  EVT_SYSTEM,        // boot, NTP sync, config change
  EVT_RULE,          // rule triggered/deactivated
  EVT_SCHEDULE       // schedule started/ended
};

struct LogEntry {
  unsigned long ts;       // epoch (UTC)
  EventType     type;
  uint8_t       relayCh;  // 0 if not relay event
  uint8_t       value;    // 0/1 for relay, 0 otherwise
  uint8_t       sourceId; // rule.id or schedule.id
  char          msg[48];  // human-readable (Japanese OK, UTF-8)
};

#define LOG_MAX_ENTRIES 100
#define LOG_PATH        "/log.json"
#define LOG_FLUSH_EVERY 10  // flush after this many new entries

struct EventLog {
  LogEntry entries[LOG_MAX_ENTRIES];
  int      head;        // next write position (0..LOG_MAX_ENTRIES-1)
  int      count;       // total valid entries (max LOG_MAX_ENTRIES)
  bool     dirty;       // needs flush to LittleFS
  int      dirtyCount;  // entries added since last flush
};

// Zero everything.
static inline void logInit(EventLog& log) {
  memset(&log, 0, sizeof(log));
}

// Add one entry to the ring buffer. Sets dirty=true.
static inline void logAdd(EventLog& log,
                          unsigned long epoch,
                          EventType     type,
                          uint8_t       ch,
                          uint8_t       val,
                          uint8_t       srcId,
                          const char*   msg) {
  LogEntry& e = log.entries[log.head];
  e.ts       = epoch;
  e.type     = type;
  e.relayCh  = ch;
  e.value    = val;
  e.sourceId = srcId;
  strncpy(e.msg, msg, sizeof(e.msg) - 1);
  e.msg[sizeof(e.msg) - 1] = '\0';

  log.head = (log.head + 1) % LOG_MAX_ENTRIES;
  if (log.count < LOG_MAX_ENTRIES) log.count++;

  log.dirty = true;
  log.dirtyCount++;
}

// Load log from /log.json. Entries in file are newest-first;
// we store them oldest-first internally (chronological order).
static inline void logLoad(EventLog& log) {
  logInit(log);

  File f = LittleFS.open(LOG_PATH, "r");
  if (!f) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return;

  JsonArray arr = doc["log"].as<JsonArray>();
  if (!arr) return;

  // Count how many valid entries are in the file (up to LOG_MAX_ENTRIES)
  int total = 0;
  for (JsonObject obj : arr) {
    (void)obj;
    if (++total >= LOG_MAX_ENTRIES) break;
  }
  if (total == 0) return;

  // File is newest-first; load in reverse so index 0 = oldest.
  // Build a temporary index array to iterate in reverse without STL.
  // Since total <= LOG_MAX_ENTRIES (<=100) we can re-scan.
  int idx = 0;
  for (JsonObject obj : arr) {
    if (idx >= LOG_MAX_ENTRIES) break;
    // We want to store entry[total-1-idx] = obj (reverse order)
    int slot = total - 1 - idx;
    LogEntry& e = log.entries[slot];
    e.ts       = obj["ts"]  | (unsigned long)0;
    e.type     = (EventType)(obj["ev"] | 0);
    e.relayCh  = obj["ch"]  | (uint8_t)0;
    e.value    = obj["val"] | (uint8_t)0;
    e.sourceId = obj["sid"] | (uint8_t)0;
    const char* m = obj["msg"] | "";
    strncpy(e.msg, m, sizeof(e.msg) - 1);
    e.msg[sizeof(e.msg) - 1] = '\0';
    idx++;
  }

  log.count      = total;
  log.head       = total % LOG_MAX_ENTRIES;  // next write slot
  log.dirty      = false;
  log.dirtyCount = 0;
}

// Save log to /log.json if dirty. Writes entries newest-first.
// Resets dirty flag on success.
static inline void logFlush(EventLog& log) {
  if (!log.dirty) return;

  JsonDocument doc;
  JsonArray arr = doc["log"].to<JsonArray>();

  // Iterate newest-first: start from (head-1) and go backwards.
  int total = log.count;
  for (int i = 0; i < total; i++) {
    int slot = ((log.head - 1 - i) + LOG_MAX_ENTRIES) % LOG_MAX_ENTRIES;
    LogEntry& e = log.entries[slot];
    JsonObject obj = arr.add<JsonObject>();
    obj["ts"]  = e.ts;
    obj["ev"]  = (uint8_t)e.type;
    obj["ch"]  = e.relayCh;
    obj["val"] = e.value;
    obj["sid"] = e.sourceId;
    obj["msg"] = e.msg;
  }

  File f = LittleFS.open(LOG_PATH, "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();

  log.dirty      = false;
  log.dirtyCount = 0;
}

// Return true if the log should be flushed now:
// dirty AND (dirtyCount >= LOG_FLUSH_EVERY).
// For time-based triggering, call logFlush() explicitly from loop().
static inline bool logNeedsFlush(EventLog& log) {
  return log.dirty && (log.dirtyCount >= LOG_FLUSH_EVERY);
}

// Copy up to maxOut entries to out[] in reverse chronological order
// (newest first). Returns the number of entries copied.
static inline int logGetEntries(EventLog& log, LogEntry* out, int maxOut) {
  int total = log.count;
  if (total > maxOut) total = maxOut;

  for (int i = 0; i < total; i++) {
    int slot = ((log.head - 1 - i) + LOG_MAX_ENTRIES) % LOG_MAX_ENTRIES;
    out[i] = log.entries[slot];
  }
  return total;
}
