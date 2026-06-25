// sleep_test.cpp - Deep-sleep current test for Heltec Vision Master E290
//
// Purpose: validate the ~20-40uA deep-sleep floor (LoRa slept, Vext off, LED off)
//          before building the full battery firmware. This is a throwaway test sketch.
//
// Build/flash:  pio run -e sleep_test -t upload
// Wake:         press the USER button (GPIO21). (Optional safety timer wake below.)
//
// Measure: put the multimeter in series with the BATTERY line (JP1), not USB.
//   - Deep sleep  -> uA range. Target ~20-40uA.
//   - On wake it spikes to tens of mA for a few seconds (display refresh) -- switch to
//     the mA range around wakes or the uA fuse may blow. Those wakes don't dominate.
//   - Expected "bad" result if LoRa isn't actually sleeping: ~1mA+ floor.

#include <Arduino.h>
#include "heltec-eink-modules.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"

// ----- VME290 pins -----
static const int PIN_LED   = 45;   // status LED, ACTIVE LOW (LOW = on, HIGH = off)
static const int PIN_BTN   = 21;   // user button, active LOW (INPUT_PULLUP), RTC-capable
static const int PIN_VEXT  = 18;   // peripheral power rail, ACTIVE HIGH (HIGH = on)
static const int LORA_NSS  = 8;
static const int LORA_SCK  = 9;
static const int LORA_MOSI = 10;
static const int LORA_MISO = 11;
static const int LORA_NRST = 12;
static const int LORA_BUSY = 13;

// 0 = button-only wake (cleanest steady uA reading). Set e.g. 300 for a 5-min safety wake.
static const uint64_t TIMER_WAKE_SEC = 0;

RTC_DATA_ATTR int bootCount = 0;

EInkDisplay_VisionMasterE290 *display = nullptr;

// Port of heltec-eink-modules WirelessPaper prepareToSleep(): software-SPI SX1262 SetSleep.
static void loraSleep() {
  digitalWrite(LORA_NSS, HIGH);
  digitalWrite(LORA_SCK, LOW);          // SPI mode 0 idle low
  digitalWrite(LORA_MOSI, LOW);
  pinMode(LORA_NSS, OUTPUT);
  pinMode(LORA_SCK, OUTPUT);
  pinMode(LORA_MOSI, OUTPUT);
  pinMode(LORA_NRST, OUTPUT);
  digitalWrite(LORA_NRST, HIGH);        // not held in reset, so SLEEP is accepted
  delay(10);

  digitalWrite(LORA_NSS, LOW);
  shiftOut(LORA_MOSI, LORA_SCK, MSBFIRST, 0x84); // SetSleep opcode
  shiftOut(LORA_MOSI, LORA_SCK, MSBFIRST, 0x04); // sleepConfig: warm start (retain config)
  digitalWrite(LORA_NSS, HIGH);

  // LoRa pins to high-Z so they don't source current; NSS held HIGH through deep sleep.
  pinMode(LORA_NRST, ANALOG);
  pinMode(LORA_BUSY, ANALOG);
  pinMode(LORA_SCK,  ANALOG);
  pinMode(LORA_MISO, ANALOG);
  pinMode(LORA_MOSI, ANALOG);
  pinMode(LORA_NSS,  OUTPUT);
  digitalWrite(LORA_NSS, HIGH);
  gpio_hold_en((gpio_num_t)LORA_NSS);
}

static void vextOff() {
  pinMode(PIN_VEXT, OUTPUT);
  digitalWrite(PIN_VEXT, LOW);          // active HIGH -> LOW = peripherals powered off
}

static void goToSleep() {
  Serial.println("LoRa sleep + Vext off + LED off -> deep sleep. Press button to wake.");
  Serial.flush();

  loraSleep();
  digitalWrite(PIN_LED, HIGH);          // active LOW -> HIGH = LED off
  vextOff();

  // Wake on button press (active LOW) via ext0 (GPIO21 is RTC-capable on the S3).
  rtc_gpio_pullup_en((gpio_num_t)PIN_BTN);
  rtc_gpio_pulldown_dis((gpio_num_t)PIN_BTN);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BTN, 0);   // 0 = wake when pulled LOW

  if (TIMER_WAKE_SEC > 0)
    esp_sleep_enable_timer_wakeup(TIMER_WAKE_SEC * 1000000ULL);

  gpio_deep_sleep_hold_en();            // keep NSS held HIGH while asleep
  esp_deep_sleep_start();               // never returns; wake = full reboot
}

void setup() {
  // Release any pin holds left from the previous sleep so we can re-drive them.
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)LORA_NSS);

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);           // LED on while awake (visual heartbeat)
  pinMode(PIN_BTN, INPUT_PULLUP);

  Serial.begin(115200);
  delay(300);

  bootCount++;
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  Serial.printf("\n=== Sleep test: boot #%d (wake cause %d) ===\n", bootCount, (int)cause);

  // Show we're awake -- confirms the Vext + e-ink path still works.
  if (!display) display = new EInkDisplay_VisionMasterE290();
  display->landscape();
  display->setRotation(1);
  display->clearMemory();
  display->setCursor(6, 24);
  display->print("Sleep test - awake");
  char line[48];
  sprintf(line, "boot #%d  wake cause %d", bootCount, (int)cause);
  display->setCursor(6, 50);
  display->print(line);
  display->setCursor(6, 76);
  display->print("Press button to wake");
  display->update();

  // Stay awake ~6s (so the image refreshes / you can re-flash), then sleep.
  delay(6000);
  goToSleep();
}

void loop() {}   // never reached -- deep sleep reboots into setup()
