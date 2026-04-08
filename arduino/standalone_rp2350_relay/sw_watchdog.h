#pragma once
#include <hardware/watchdog.h>
#include <pico/time.h>

// Configuration
static const unsigned long SWD_CHECK_MS = 5000;
static const int SWD_MISS_THRESHOLD = 3;

// State
static volatile bool swWdtFlag = false;
static volatile int swWdtMissCount = 0;
static volatile bool swWdtEnabled = false;
static struct repeating_timer swWdtTimer;

static inline bool swWdtCallback(struct repeating_timer *t) {
  if (!swWdtEnabled) return true;
  if (swWdtFlag) {
    swWdtFlag = false;
    swWdtMissCount = 0;
  } else {
    swWdtMissCount++;
    if (swWdtMissCount >= SWD_MISS_THRESHOLD) {
      watchdog_reboot(0, 0, 0);
      while (true) { __asm__ volatile("nop"); }
    }
  }
  return true;
}

static inline void swWdtFeed()    { swWdtFlag = true; }
static inline void swWdtDisable() { swWdtEnabled = false; }
static inline void swWdtEnable()  { swWdtFlag = true; swWdtMissCount = 0; swWdtEnabled = true; }
static inline void swWdtStart()   {
  add_repeating_timer_ms(-(long)SWD_CHECK_MS, swWdtCallback, NULL, &swWdtTimer);
  swWdtEnable();
}
