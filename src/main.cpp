// Parking Permit Display - battery version
//
// Every wake is one pass through setup(): read the battery, sync with the phone if a check-in
// is due, redraw only if the permit on screen should change, then deep sleep (~20uA). The
// e-ink keeps its image with no power. Wakes: the button, a timer for the next scheduled
// check-in, and the handoff to the cached next permit at its validFrom.
#include <Arduino.h>
#include "heltec-eink-modules.h"

#include "Fonts/FreeSansBold8pt7b.h"

#include "Code39Generator.h"
#include "imgs/toronto_logo.h"
#include "permit_config.h"
#include "power_helper.h"
#include "permit_store.h"
#include "bluetooth_helper.h"

// ----- Battery thresholds (single-cell LiPo, no BMS on the board) -----
static const int BATTERY_LOW_MV      = 3500;  // show the low-battery icon
static const int BATTERY_CRITICAL_MV = 3300;  // stop timed check-ins; handoffs + button only
static const int BATTERY_PRESENT_MV  = 2500;  // below this the reading is junk (USB, no cell)
static const int BATTERY_FULL_MV     = 4150;  // the app's 100%
static const int CHARGING_RISE_MV    = 30;    // above the last check-in = it's been charging
static const int UNPLUG_DROP_MV      = 30;    // this far below the charging peak = unplugged
static const uint32_t CHARGE_POLL_S  = 300;   // while charging, check the battery this often
static const int CHARGE_MAX_POLLS    = 72;    // and give up after 6 hours of it
static const int RESTART_WATCH_POLLS = 3;     // after a restart: 3 checks, CHARGE_POLL_S apart
static const int WATCH_RISE_MV       = 20;    // climbing this much since the restart = charging

// ----- Check-in schedule (local time) -----
static const int CHECKIN_HOURS[] = {8, 19};         // routine daily check-ins
static const time_t HUNGRY_BEFORE_END = 24 * 3600;  // start looking for the next permit this long before coverage ends
static const time_t HUNGRY_INTERVAL = 30 * 60;      // how often while looking
static const int HUNGRY_FIRST_HOUR = 7;             // ...only between these hours
static const int HUNGRY_LAST_HOUR = 21;
static const time_t NO_CLOCK_INTERVAL = 60 * 60;    // clock lost (battery died): retry hourly
static const uint32_t SCAN_SECONDS_TIMER = 4;       // phone advertises every ~100ms, 4s is plenty
static const uint32_t SCAN_SECONDS_BUTTON = 10;
static const uint32_t LONG_PRESS_MS = 3000;
static const uint32_t RESET_PRESS_MS = 10000;      // forget the phone and the permits

// What's on the e-ink right now. RTC memory survives deep sleep but not a power loss, in
// which case we redraw once.
RTC_DATA_ATTR char shownPermit[20] = "";
RTC_DATA_ATTR bool shownFlipped = false;
RTC_DATA_ATTR int shownIcon = 0;  // BatteryIcon on screen
RTC_DATA_ATTR time_t nextCheckinAt = 0;

// Charging mode. The board can't see USB, so: a button press reading well above the last
// scheduled wake (baselineMv) means it was plugged in since. Then poll every few minutes, show a
// charging / full icon on the permit, and leave the mode when the voltage drops off its peak
// (unplugged) or after CHARGE_MAX_POLLS.
enum BatteryIcon { ICON_NONE = 0, ICON_LOW, ICON_CHARGING, ICON_FULL };
RTC_DATA_ATTR int baselineMv = 0;
RTC_DATA_ATTR bool chargingMode = false;
RTC_DATA_ATTR int chargePeakMv = 0;
RTC_DATA_ATTR int chargePolls = 0;
// After a restart (e.g. a battery swap while plugged in) there's no earlier reading to compare
// a button press with, so check a few more times: still climbing = it's on the charger.
RTC_DATA_ATTR int watchPolls = 0;
RTC_DATA_ATTR int watchStartMv = 0;

EInkDisplay_VisionMasterE290 *display = nullptr;

void displayInit(bool flipped)
{
  if (!display) display = new EInkDisplay_VisionMasterE290();
  display->landscape();
  display->setRotation(flipped ? 3 : 1);
}

// Battery at the right end of the "permit" title line (free space there): nearly empty, charging
// (bolt), or full
void drawBatteryIcon(int icon)
{
  if (icon == ICON_NONE) return;
  const int x = 264, y = 20, w = 26, h = 13;
  display->drawRect(x, y, w, h, 0x0000);
  display->drawRect(x + 1, y + 1, w - 2, h - 2, 0x0000);  // 2px outline
  display->fillRect(x + w, y + 4, 3, h - 8, 0x0000);       // terminal nub
  if (icon == ICON_LOW) {
    display->fillRect(x + 3, y + 3, 4, h - 6, 0x0000);     // low charge sliver
  } else if (icon == ICON_FULL) {
    display->fillRect(x + 3, y + 3, w - 6, h - 6, 0x0000); // full
  } else {
    // Bolt: two filled triangles meeting in the middle
    const int cx = x + w / 2;
    display->fillTriangle(cx + 2, y + 2, cx - 4, y + 7, cx + 1, y + 7, 0x0000);
    display->fillTriangle(cx - 1, y + 6, cx + 4, y + 6, cx - 2, y + h - 2, 0x0000);
  }
}

void displayPermit(const PermitData &p, int batteryIcon)
{
  display->clearMemory();
  display->setFont((GFXfont *)&FreeSansBold8pt7b);
  display->setTextSize(1);

  // ========== LOGO (top-left, white on black) ==========
  display->fillRect(0, 0, LOGO_WIDTH, LOGO_HEIGHT, 0x0000);
  display->drawBitmap(0, 0, logo_toronto, LOGO_WIDTH, LOGO_HEIGHT, 0xFFFF);

  // ========== TITLE (right of logo) ==========
  display->setCursor(RIGHT_COL_X, TITLE_Y1);
  display->print("Temporary parking");
  display->setCursor(RIGHT_COL_X, TITLE_Y2);
  display->print("permit");

  // ========== PERMIT INFO (right column) ==========
  char line[40];
  snprintf(line, sizeof(line), "Permit: %s", p.permitNumber);
  display->setCursor(RIGHT_COL_X, PERMIT_Y);
  display->print(line);
  snprintf(line, sizeof(line), "Plate: %s", p.plateNumber);
  display->setCursor(RIGHT_COL_X, PLATE_Y);
  display->print(line);

  // ========== BARCODE LABEL (below plate) ==========
  snprintf(line, sizeof(line), "Area: %s", p.barcodeLabel);
  display->setCursor(RIGHT_COL_X, DATE_TO_Y);
  display->print(line);

  // ========== DATES (left column, below logo) ==========
  // Date portion only (first 12 chars = "Sep 05, 2025"), drop the time
  char dateVal[16];
  const int DATE_VAL_X = 47;
  display->setCursor(0, DATE_FROM_Y);
  display->print("From:");
  snprintf(dateVal, sizeof(dateVal), "%.12s", p.validFrom);
  display->setCursor(DATE_VAL_X, DATE_FROM_Y);
  display->print(dateVal);
  display->setCursor(0, DATE_TO_Y);
  display->print("To:");
  snprintf(dateVal, sizeof(dateVal), "%.12s", p.validTo);
  display->setCursor(DATE_VAL_X, DATE_TO_Y);
  display->print(dateVal);

  drawBatteryIcon(batteryIcon);

  // ========== SEPARATOR ==========
  display->drawLine(0, SEPARATOR_Y, SCREEN_W - 1, SEPARATOR_Y, 0x0000);

  // ========== BARCODE (full width, bottom) ==========
  Code39Generator barcodeGen(display);
  barcodeGen.drawBarcode(p.barcodeValue, BARCODE_X, BARCODE_Y, BARCODE_HEIGHT, NARROW_BAR_WIDTH);

  display->update();
}

void displayMessage(const char *message)
{
  display->clearMemory();
  display->setFont(&FreeSansBold8pt7b);
  display->setTextSize(1);

  int16_t x1, y1;
  uint16_t w, h;
  display->getTextBounds((char *)message, 0, 0, &x1, &y1, &w, &h);
  display->setCursor((SCREEN_W - w) / 2, (SCREEN_H + h) / 2);
  display->print(message);
  display->update();
}

// Not paired with a phone: how to pair, and this unit's name as the app will show it
void displayUnpaired()
{
  char id[13];
  getDisplayId(id);
  char name[24];
  snprintf(name, sizeof(name), "Display %s", id + 8);
  for (char *c = name; *c; c++) *c = toupper(*c);

  display->clearMemory();
  display->setFont(&FreeSansBold8pt7b);
  display->setTextSize(1);
  const char *lines[] = {"Not paired", "Press the button near your phone", name};
  const int ys[] = {40, 72, 110};
  for (int i = 0; i < 3; i++) {
    int16_t x1, y1;
    uint16_t w, h;
    display->getTextBounds((char *)lines[i], 0, 0, &x1, &y1, &w, &h);
    display->setCursor((SCREEN_W - w) / 2, ys[i]);
    display->print(lines[i]);
  }
  display->update();
  strlcpy(shownPermit, "-", sizeof(shownPermit));
}

// Seconds-of-day helpers on local time (stored as UTC, see permit_store.h)
static time_t atHourOnDay(time_t t, int hour)
{
  return t - (t % 86400) + hour * 3600;
}

// When to next look for the phone, or 0 for never (critical battery: button only)
time_t scheduleCheckin(const PermitStore &store, int batteryMv)
{
  if (batteryMv >= BATTERY_PRESENT_MV && batteryMv < BATTERY_CRITICAL_MV) return 0;

  time_t now = time(nullptr);
  if (!timeValid()) return now + NO_CLOCK_INTERVAL;

  // Close to the end of coverage with nothing newer cached: look often, but only by day
  if (coveredUntil(store) - now < HUNGRY_BEFORE_END) {
    time_t t = now + HUNGRY_INTERVAL;
    int hour = (t % 86400) / 3600;
    if (hour < HUNGRY_FIRST_HOUR) t = atHourOnDay(t, HUNGRY_FIRST_HOUR);
    else if (hour > HUNGRY_LAST_HOUR) t = atHourOnDay(t + 86400, HUNGRY_FIRST_HOUR);
    return t;
  }

  // Routine: next daily check-in (keeps the clock and the app's battery reading fresh)
  for (int day = 0; day < 2; day++) {
    for (int hour : CHECKIN_HOURS) {
      time_t t = atHourOnDay(now + day * 86400, hour);
      if (t > now + 60) return t;
    }
  }
  return now + 86400;
}

// One wake: sync if due, redraw if needed. Returns seconds until the next wake (0 = button only).
uint32_t runWake(esp_sleep_wakeup_cause_t cause)
{
  int batteryMv = readBatteryMv();
  bool batteryKnown = batteryMv >= BATTERY_PRESENT_MV;
  bool lowBattery = batteryKnown && batteryMv < BATTERY_LOW_MV;
  Serial.printf("\n=== Wake: cause %d, battery %d mV, clock %s ===\n",
                (int)cause, batteryMv, timeValid() ? "set" : "unset");
  // ----- Charging mode (see chargingMode) -----
  if (chargingMode) {
    chargePolls++;
    if (!batteryKnown || batteryMv < chargePeakMv - UNPLUG_DROP_MV || chargePolls > CHARGE_MAX_POLLS) {
      Serial.println("Charging mode off (unplugged or timed out)");
      chargingMode = false;
    } else if (batteryMv > chargePeakMv) {
      chargePeakMv = batteryMv;
    }
  } else if ((cause == ESP_SLEEP_WAKEUP_EXT0 && batteryKnown && baselineMv >= BATTERY_PRESENT_MV
              && batteryMv > baselineMv + CHARGING_RISE_MV)
             || (cause == ESP_SLEEP_WAKEUP_TIMER && watchPolls > 0 && batteryKnown
                 && batteryMv > watchStartMv + WATCH_RISE_MV)) {
    Serial.println("Charging mode on");
    chargingMode = true;
    chargePeakMv = batteryMv;
    chargePolls = 0;
    watchPolls = 0;
  } else if (cause == ESP_SLEEP_WAKEUP_TIMER && watchPolls > 0) {
    watchPolls--;
  }
  bool restarted = cause != ESP_SLEEP_WAKEUP_EXT0 && cause != ESP_SLEEP_WAKEUP_TIMER;
  if (restarted && batteryKnown && !chargingMode) {
    watchPolls = RESTART_WATCH_POLLS;
    watchStartMv = batteryMv;
  }
  if (!chargingMode && cause != ESP_SLEEP_WAKEUP_EXT0 && batteryKnown) baselineMv = batteryMv;
  int batteryIcon = chargingMode ? (batteryMv >= BATTERY_FULL_MV ? ICON_FULL : ICON_CHARGING)
                                 : (lowBattery ? ICON_LOW : ICON_NONE);

  PermitStore store;
  loadPermits(store);

  // ----- Decide whether to sync, and how -----
  bool doSync = false;
  bool forceRedraw = false;
  uint8_t syncType = SYNC_TYPE_AUTO;
  uint32_t scanSeconds = SCAN_SECONDS_TIMER;

  if (cause == ESP_SLEEP_WAKEUP_EXT0) {
    // Button: short press = sync, hold 3s = force (redraw even if nothing changed),
    // hold 10s = reset. The LED goes out at 3s and back on at 10s so you know where you are.
    unsigned long start = millis();
    ledOn();
    while (digitalRead(PIN_BUTTON) == LOW && millis() - start < RESET_PRESS_MS) {
      unsigned long held = millis() - start;
      if (held >= LONG_PRESS_MS) ledOff();
      delay(10);
    }
    unsigned long held = millis() - start;
    // Released: dark during the sync, then ledSignal shows the result (one clean signal)
    if (held >= RESET_PRESS_MS) ledOn();
    else ledOff();
    if (held >= RESET_PRESS_MS) {
      Serial.println("10s hold: reset pairing and permits");
      clearOwner();
      clearPermits();
      displayInit(false);
      displayUnpaired();
      while (digitalRead(PIN_BUTTON) == LOW) delay(20);  // released = no extra press
      return 0;
    }
    bool longPress = held >= LONG_PRESS_MS;
    Serial.println(longPress ? "Long press: force sync" : "Button: sync");
    doSync = true;
    forceRedraw = longPress;
    syncType = longPress ? SYNC_TYPE_FORCE : SYNC_TYPE_MANUAL;
    scanSeconds = SCAN_SECONDS_BUTTON;
  } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
    // Timer: a check-in, or just the handoff to the cached permit
    doSync = nextCheckinAt != 0 && time(nullptr) >= nextCheckinAt - 120;
    // Charging: every poll syncs too, so the app's bolt and % follow along and drop at unplug
    // (on USB power this shouldn't cost the battery anything)
    if (chargingMode) doSync = true;
  } else {
    // Power-on (battery connected, flashed, reset): sync and redraw once
    doSync = true;
    forceRedraw = true;
    scanSeconds = SCAN_SECONDS_BUTTON;
  }

  if (doSync) {
    PermitStore fresh = store;
    // Timer check-ins never pair: nobody is standing next to it
    bool allowClaim = cause != ESP_SLEEP_WAKEUP_TIMER;
    SyncResult result = syncWithPhone(scanSeconds, syncType, fresh, batteryMv, allowClaim);
    // The phone got the miss counts with this sync's status, so start over
    // (a charging poll that misses the phone isn't a missed check-in)
    bool countsAsCheckin = !(chargingMode && cause == ESP_SLEEP_WAKEUP_TIMER
                             && !(nextCheckinAt != 0 && time(nullptr) >= nextCheckinAt - 120));
    if (result == SYNC_OK) missNotFound = missFailed = 0;
    else if (result == SYNC_NOT_FOUND && countsAsCheckin) missNotFound++;
    else if (result == SYNC_FAILED && countsAsCheckin) missFailed++;
    if (result == SYNC_UNPAIRED) {
      displayInit(false);
      displayUnpaired();
      return 0;
    } else if (result == SYNC_OK) {
      if (memcmp(&fresh, &store, sizeof(store)) != 0) {
        store = fresh;
        savePermits(store);
        Serial.println("Permits updated");
      }
      if (syncType != SYNC_TYPE_AUTO) ledSignal(2, 150);  // button press: two quick blinks = synced
    } else if (syncType != SYNC_TYPE_AUTO) {
      ledSignal(1, 1000);  // one long blink = couldn't sync
      // Someone is looking at it: say why nothing changed, then put the permit back
      displayInit(store.displayFlipped);
      displayMessage(result == SYNC_NOT_FOUND ? "Phone not found" : "Sync failed");
      delay(2000);
      forceRedraw = true;
    }
  }

  // ----- Redraw if what should be shown differs from what is shown -----
  int slot = chooseSlot(store);
  if (slot >= 0) {
    const PermitData &p = store.slot[slot];
    if (forceRedraw || strcmp(shownPermit, p.permitNumber) != 0 ||
        shownFlipped != store.displayFlipped || shownIcon != batteryIcon) {
      Serial.printf("Drawing permit %s\n", p.permitNumber);
      displayInit(store.displayFlipped);
      displayPermit(p, batteryIcon);
      strlcpy(shownPermit, p.permitNumber, sizeof(shownPermit));
      shownFlipped = store.displayFlipped;
      shownIcon = batteryIcon;
    }
  } else if (strcmp(shownPermit, "-") != 0) {
    displayInit(store.displayFlipped);
    uint8_t owner[OWNER_ID_LEN];
    if (loadOwner(owner)) displayMessage("No permit yet. Press the button to sync.");
    else displayUnpaired();
    strlcpy(shownPermit, "-", sizeof(shownPermit));
  }

  // ----- Sleep until the next check-in or handoff, whichever is first -----
  nextCheckinAt = scheduleCheckin(store, batteryMv);
  time_t handoff = nextHandoff(store);
  time_t wakeAt = nextCheckinAt;
  if (handoff && (!wakeAt || handoff < wakeAt)) wakeAt = handoff + 5;

  uint32_t seconds = 0;  // 0 = button only
  if (wakeAt) {
    time_t delta = wakeAt - time(nullptr);
    seconds = delta < 60 ? 60 : (delta > 86400 ? 86400 : (uint32_t)delta);
  }
  if ((chargingMode || watchPolls > 0) && (seconds == 0 || seconds > CHARGE_POLL_S)) seconds = CHARGE_POLL_S;
  return seconds;
}

void setup()
{
  powerInit();
  Serial.begin(115200);
  timeInit();

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool fromSleep = cause == ESP_SLEEP_WAKEUP_EXT0 || cause == ESP_SLEEP_WAKEUP_TIMER;
  if (!fromSleep) {
    recordRestart((int)esp_reset_reason());  // power-on, crash, brownout...: counted for the phone
    delay(1500);  // power-on/flash: give the USB serial monitor a moment
  }

  uint32_t seconds = runWake(cause);

#ifdef DEV_NO_SLEEP
  // Dev build (env:dev): stay awake so USB, serial and flashing keep working. A button press
  // or the scheduled time runs the same wake logic as a real wake from deep sleep.
  while (true) {
    ledOff();  // deepSleep() normally does this; without it the LED stays on after a restart
    Serial.printf("DEV: not sleeping. Next wake in %u s (button wakes now)\n", seconds);
    unsigned long start = millis();
    cause = ESP_SLEEP_WAKEUP_TIMER;
    while (seconds == 0 || millis() - start < seconds * 1000UL) {
      if (digitalRead(PIN_BUTTON) == LOW) {
        cause = ESP_SLEEP_WAKEUP_EXT0;
        break;
      }
      delay(20);
    }
    seconds = runWake(cause);
    ledOff();  // deepSleep() normally does this
    while (digitalRead(PIN_BUTTON) == LOW) delay(20);  // wait for release
  }
#else
  deepSleep(seconds);
#endif
}

void loop() {}  // never reached: deep sleep wakes into setup()
