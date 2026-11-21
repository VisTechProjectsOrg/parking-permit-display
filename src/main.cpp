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
RTC_DATA_ATTR bool shownLowBattery = false;
RTC_DATA_ATTR time_t nextCheckinAt = 0;

EInkDisplay_VisionMasterE290 *display = nullptr;

void displayInit(bool flipped)
{
  if (!display) display = new EInkDisplay_VisionMasterE290();
  display->landscape();
  display->setRotation(flipped ? 3 : 1);
}

void drawLowBatteryIcon()
{
  // Nearly empty battery at the right end of the "permit" title line (free space there)
  const int x = 264, y = 20, w = 26, h = 13;
  display->drawRect(x, y, w, h, 0x0000);
  display->drawRect(x + 1, y + 1, w - 2, h - 2, 0x0000);  // 2px outline
  display->fillRect(x + w, y + 4, 3, h - 8, 0x0000);       // terminal nub
  display->fillRect(x + 3, y + 3, 4, h - 6, 0x0000);       // low charge sliver
}

void displayPermit(const PermitData &p, bool lowBattery)
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

  if (lowBattery) drawLowBatteryIcon();

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
    if (result == SYNC_OK) missNotFound = missFailed = 0;
    else if (result == SYNC_NOT_FOUND) missNotFound++;
    else if (result == SYNC_FAILED) missFailed++;
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
        shownFlipped != store.displayFlipped || shownLowBattery != lowBattery) {
      Serial.printf("Drawing permit %s\n", p.permitNumber);
      displayInit(store.displayFlipped);
      displayPermit(p, lowBattery);
      strlcpy(shownPermit, p.permitNumber, sizeof(shownPermit));
      shownFlipped = store.displayFlipped;
      shownLowBattery = lowBattery;
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
