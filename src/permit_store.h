#ifndef PERMIT_STORE_H
#define PERMIT_STORE_H

// The display holds two permits (the current one and the next, already-bought one) so it can
// switch at the handoff on its own, without the phone. All times are LOCAL time stored as
// if it were UTC: the phone sends its local clock, so DST is handled on the phone side.

#include <Arduino.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>

struct PermitData {
  char permitNumber[20];
  char plateNumber[20];
  char validFrom[30];
  char validTo[30];
  char barcodeValue[20];
  char barcodeLabel[20];
};

static const int SLOT_COUNT = 2;

struct PermitStore {
  PermitData slot[SLOT_COUNT];  // slot 0 = what the phone called current, slot 1 = next
  bool displayFlipped;
};

static Preferences prefs;

bool hasPermit(const PermitData &p) { return p.permitNumber[0] != '\0'; }

void timeInit()
{
  setenv("TZ", "UTC0", 1);
  tzset();
}

// Time survives deep sleep (RTC timer) but not a power loss; anything before 2024 means unset
bool timeValid() { return time(nullptr) > 1704067200; }

void setLocalTime(time_t localNow)
{
  struct timeval tv = {localNow, 0};
  settimeofday(&tv, nullptr);
}

// "Nov 22, 2026: 23:59" -> local epoch, or 0 if it can't be parsed
time_t parsePermitTime(const char *s)
{
  static const char *MONTHS = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[4] = {0};
  int day, year, hour = 0, minute = 0;
  if (sscanf(s, "%3s %d, %d: %d:%d", mon, &day, &year, &hour, &minute) < 3) return 0;
  const char *m = strstr(MONTHS, mon);
  if (!m) return 0;
  struct tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon = (m - MONTHS) / 3;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = minute;
  return mktime(&t);  // TZ is UTC0, so this is timegm
}

void savePermits(const PermitStore &store)
{
  prefs.begin("permit", false);
  prefs.putBytes("slots", store.slot, sizeof(store.slot));
  prefs.putBool("flipped", store.displayFlipped);
  prefs.end();
}

bool loadPermits(PermitStore &store)
{
  memset(&store, 0, sizeof(store));
  prefs.begin("permit", true);
  store.displayFlipped = prefs.getBool("flipped", false);
  if (prefs.getBytesLength("slots") == sizeof(store.slot)) {
    prefs.getBytes("slots", store.slot, sizeof(store.slot));
  } else if (prefs.isKey("permitNum")) {
    // Single permit saved by the pre-battery firmware
    PermitData &p = store.slot[0];
    strlcpy(p.permitNumber, prefs.getString("permitNum", "").c_str(), sizeof(p.permitNumber));
    strlcpy(p.plateNumber, prefs.getString("plateNum", "").c_str(), sizeof(p.plateNumber));
    strlcpy(p.validFrom, prefs.getString("validFrom", "").c_str(), sizeof(p.validFrom));
    strlcpy(p.validTo, prefs.getString("validTo", "").c_str(), sizeof(p.validTo));
    strlcpy(p.barcodeValue, prefs.getString("barcode", "").c_str(), sizeof(p.barcodeValue));
    strlcpy(p.barcodeLabel, prefs.getString("barLabel", "").c_str(), sizeof(p.barcodeLabel));
  }
  prefs.end();
  return hasPermit(store.slot[0]) || hasPermit(store.slot[1]);
}

// Which slot to show now: the one valid right now (the newer if they overlap), else the next
// one to start, else the most recent expired one. Without a clock, slot 0. -1 if empty.
int chooseSlot(const PermitStore &store)
{
  if (!timeValid()) return hasPermit(store.slot[0]) ? 0 : (hasPermit(store.slot[1]) ? 1 : -1);

  time_t now = time(nullptr);
  int current = -1, future = -1, expired = -1;
  for (int i = 0; i < SLOT_COUNT; i++) {
    const PermitData &p = store.slot[i];
    if (!hasPermit(p)) continue;
    time_t from = parsePermitTime(p.validFrom);
    time_t to = parsePermitTime(p.validTo) + 59;  // validTo is the last minute
    if (from <= now && now <= to) {
      if (current < 0 || from > parsePermitTime(store.slot[current].validFrom)) current = i;
    } else if (from > now) {
      if (future < 0 || from < parsePermitTime(store.slot[future].validFrom)) future = i;
    } else {
      if (expired < 0 || to > parsePermitTime(store.slot[expired].validTo)) expired = i;
    }
  }
  if (current >= 0) return current;
  if (future >= 0) return future;
  return expired;
}

// The next moment a different permit should be on screen (a cached permit starting), or 0
time_t nextHandoff(const PermitStore &store)
{
  if (!timeValid()) return 0;
  time_t now = time(nullptr), best = 0;
  for (int i = 0; i < SLOT_COUNT; i++) {
    if (!hasPermit(store.slot[i])) continue;
    time_t from = parsePermitTime(store.slot[i].validFrom);
    if (from > now && (best == 0 || from < best)) best = from;
  }
  return best;
}

// Latest validTo across both slots: how long the car is covered without a new sync
time_t coveredUntil(const PermitStore &store)
{
  time_t best = 0;
  for (int i = 0; i < SLOT_COUNT; i++) {
    if (!hasPermit(store.slot[i])) continue;
    time_t to = parsePermitTime(store.slot[i].validTo);
    if (to > best) best = to;
  }
  return best;
}

#endif
