#ifndef POWER_HELPER_H
#define POWER_HELPER_H

// Battery read, peripheral power gating and deep sleep for the Heltec Vision Master E290.
// Sleep sequence measured at ~20uA on 2026-06-25 (see NOTES.md, sleep_test.cpp).

#include <Arduino.h>
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"

static const int PIN_LED       = 45;  // active LOW
static const int PIN_BUTTON    = 21;  // active LOW, RTC-capable (ext0 wake)
static const int PIN_VEXT      = 18;  // e-ink + LoRa RF front-end power, active HIGH
static const int PIN_VBAT      = 7;   // battery divider output (ADC1_CH6)
static const int PIN_ADC_CTRL  = 46;  // switches the battery divider on (HIGH), via Q2
static const float VBAT_MULTIPLIER = 4.9f * 1.03f;  // 390k/100k divider, per Meshtastic's E290 variant

static const int LORA_NSS  = 8;
static const int LORA_SCK  = 9;
static const int LORA_MOSI = 10;
static const int LORA_MISO = 11;
static const int LORA_NRST = 12;
static const int LORA_BUSY = 13;

void ledOn()  { digitalWrite(PIN_LED, LOW); }
void ledOff() { digitalWrite(PIN_LED, HIGH); }

// Button-press result after a dark sync: a short gap, then `count` blinks
// of `onMs`, ending off. Two quick = synced, one long = couldn't sync.
void ledSignal(int count, int onMs)
{
    ledOff();
    delay(250);
    for (int i = 0; i < count; i++) {
        ledOn();
        delay(onMs);
        ledOff();
        delay(150);
    }
}

// Battery voltage in mV. Returns ~4200+ or a bogus value on USB with no cell attached.
int readBatteryMv()
{
#ifdef FAKE_BATTERY_MV
    return FAKE_BATTERY_MV;  // test build only (env:fake_low_battery)
#endif
    pinMode(PIN_ADC_CTRL, OUTPUT);
    digitalWrite(PIN_ADC_CTRL, HIGH);
    analogSetPinAttenuation(PIN_VBAT, ADC_2_5db);  // divider output tops out ~0.86V
    delay(10);  // let the divider settle

    uint32_t sum = 0;
    for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(PIN_VBAT);
    digitalWrite(PIN_ADC_CTRL, LOW);  // divider off: no drain while asleep

    return (int)((sum / 8) * VBAT_MULTIPLIER);
}

// Port of heltec-eink-modules WirelessPaper prepareToSleep(): software-SPI SX1262 SetSleep.
// The LoRa core sits on the always-on rail, so without this it alone draws ~1mA.
static void loraSleep()
{
    digitalWrite(LORA_NSS, HIGH);
    digitalWrite(LORA_SCK, LOW);
    digitalWrite(LORA_MOSI, LOW);
    pinMode(LORA_NSS, OUTPUT);
    pinMode(LORA_SCK, OUTPUT);
    pinMode(LORA_MOSI, OUTPUT);
    pinMode(LORA_NRST, OUTPUT);
    digitalWrite(LORA_NRST, HIGH);
    delay(10);

    digitalWrite(LORA_NSS, LOW);
    shiftOut(LORA_MOSI, LORA_SCK, MSBFIRST, 0x84);  // SetSleep
    shiftOut(LORA_MOSI, LORA_SCK, MSBFIRST, 0x04);  // warm start
    digitalWrite(LORA_NSS, HIGH);

    pinMode(LORA_NRST, ANALOG);
    pinMode(LORA_BUSY, ANALOG);
    pinMode(LORA_SCK,  ANALOG);
    pinMode(LORA_MISO, ANALOG);
    pinMode(LORA_MOSI, ANALOG);
    pinMode(LORA_NSS,  OUTPUT);
    digitalWrite(LORA_NSS, HIGH);
    gpio_hold_en((gpio_num_t)LORA_NSS);
}

// Call first thing on every boot: undo the holds set before the last sleep
void powerInit()
{
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis((gpio_num_t)LORA_NSS);
    pinMode(PIN_LED, OUTPUT);
    ledOff();
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    pinMode(PIN_ADC_CTRL, OUTPUT);
    digitalWrite(PIN_ADC_CTRL, LOW);
}

// Never returns: wake is a full reboot into setup(). Wakes on the button, and on the
// timer when seconds > 0.
void deepSleep(uint32_t seconds)
{
    Serial.printf("Sleeping %u s\n", seconds);
    Serial.flush();

    loraSleep();
    ledOff();
    digitalWrite(PIN_ADC_CTRL, LOW);
    pinMode(PIN_VEXT, OUTPUT);
    digitalWrite(PIN_VEXT, LOW);  // e-ink is bistable, the image stays

    rtc_gpio_pullup_en((gpio_num_t)PIN_BUTTON);
    rtc_gpio_pulldown_dis((gpio_num_t)PIN_BUTTON);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BUTTON, 0);
    if (seconds > 0)
        esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);

    gpio_deep_sleep_hold_en();
    esp_deep_sleep_start();
}

#endif
