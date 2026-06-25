# Parking Pass Display - Notes & TODO

## Barcode Scannability Fix

**Problem:** Got a parking ticket from a parking enforcement officer — their handheld scanner failed
to read the barcode on the e-ink display. Phone barcode apps scan it fine, but dedicated handheld
scanners are stricter. The barcode value and Code 39 patterns are correct (`6103268` = permit number
minus the "T" prefix, which is what the original permit barcode encodes). The issue is rendering quality.

**Root causes:**
1. `NARROW_BAR_WIDTH = 1` in `permit_config.h` — bars are only 1px wide. Too thin for strict scanners.
2. `BARCODE_X = 0` — no quiet zone on the left edge.

**Why we can't just bump n=1 to n=2:**
The barcode for `6103268` (9 Code 39 symbols) needs `143 * n` pixels wide.
- n=1 → 143px (fits left column max ~148px)
- n=2 → 286px (overflows into text column at x=150)

**Fix plan — restructure to full-width bottom barcode:**
- Top ~65px: logo + permit/plate/dates text
- Bottom ~63px: barcode spans full width (n=2, ~286px) with quiet zone on both sides
- Label (`00435`) below barcode

**Files to change:**
- `src/permit_config.h` — update `BARCODE_X`, `BARCODE_Y`, `BARCODE_HEIGHT`, `NARROW_BAR_WIDTH`, and y-position constants
- `src/main.cpp` — rearrange `displayPermit()` layout: text on top, barcode on bottom

**Status:** Fix implemented in `dev` branch (commit `08b53cf`). Needs flashing and testing on hardware.

**Other notes:**
- `00435` is a static area/zone code printed as text under the barcode on every Toronto permit — it is NOT the encoded barcode value. Our code handles this correctly.
- Code 39 patterns in `Code39Generator.h` are verified correct for all digits 0-9.
- Phone barcode apps can scan the current display fine. The issue is stricter dedicated handheld scanners.

---

## TODO: Flash & Test on Other PC

1. `git pull` on the other PC (branch: `dev`)
2. Open in VSCode with PlatformIO
3. Flash to the Vision Master E290 device
4. Scan the barcode with the strictest scanner available — verify it reads `6103268`
5. Check layout visually — Y positions may need minor tweaking depending on font rendering:
   - Logo top-left (140×50, white on black)
   - Title + Permit # + Plate # to the right of logo
   - From/To dates below logo on the left
   - Full-width barcode at the bottom (n=2, ~286px wide)
   - `00435` label centered below barcode
6. If layout needs adjusting, tweak the Y constants in `src/permit_config.h`:
   - `TITLE_Y1`, `TITLE_Y2`, `PERMIT_Y`, `PLATE_Y` (right column)
   - `DATE_FROM_Y`, `DATE_TO_Y` (left column, below logo)
   - `SEPARATOR_Y`, `BARCODE_Y`, `BARCODE_LABEL_Y_OFFSET`
7. Once happy, merge `dev` → `main`

---

# Battery + deep sleep (months unplugged)

Goal: **run for months on battery without ever plugging it in**, and auto-flip from the
expiring permit to the next one at **exactly midnight** (on-device, not the buyer's server-side
~4 PM approximation). Context: the buyer (`Toronto-Parking-Pass-Buyer`) buys the next permit on
the expiry day and currently flips the *displayed* permit server-side at 4 PM via `DISPLAY_FLIP_HOUR`
+ `--refresh-display`. Once the on-device midnight flip below lands, that server-side flip can be retired.

## How the display works today
- Board: **Heltec Vision Master E290** (ESP32-S3 + 2.9" e-ink).
- Transport: **BLE only.** The Android app (ParkingPermitSync) pushes permit.json over Bluetooth.
  **No WiFi on the device.**
- e-ink holds its image with zero power.
- **Current firmware never sleeps** (`loop()` polls the button, BLE server stays up). In practice
  the display is **plugged into USB ~weekly** to sync, then unplugged. No battery in use yet.

## The premise
**The battery is useless until the firmware deep-sleeps.** As-is (~30-60mA always on) a 200mAh
cell lasts only ~3-6 h. BLE syncs are cheap (~0.02mAh each), so months is entirely about getting
deep-sleep current low. After checking the schematic + datasheets the realistic floor is **~20-40µA**
(LDO only 6µA, no USB-UART chip to leak) -- **measure to confirm before trusting it.**

## Wake strategy
- Deep sleep by default.
- **Button (GPIO21)** -> ext0/GPIO wake -> BLE sync with phone -> sleep.
- **RTC timer wake at midnight** on expiry day -> redraw the cached next permit -> sleep (no radio).
- **Last 1-2 days:** short periodic advertise windows so the phone can push the freshly-bought
  permit without a button press; cache it.
- BLE caveat: while asleep it isn't advertising, so the phone can only sync during a wake window
  or button press.

### Power source aware (USB vs battery)
Detect power source at boot (charge IC status or VBAT_Read) and branch:
- **On USB:** stay awake, BLE server up, charging -- the current weekly "plug in to update" flow,
  unchanged. Power doesn't matter while plugged in.
- **On battery:** deep sleep aggressively; wake only on button or the RTC midnight flip.

## VME290 pin map (from heltec-eink-modules `Platforms/VisionMasterE290`)
- Vext (peripheral power): **GPIO18, active HIGH**. Lib has `Platform::VExtOff()` / `VExtOn()`.
- LED (white): **GPIO45** (`LED_BUILTIN`) -- **active LOW** (LOW=on, HIGH=off). Button: **GPIO21** (+ BOOT GPIO0).
- Display: CS=3, DC=4, RST=5, BUSY=6, SDI/MOSI=1, CLK=2.
- LoRa SX1262: NSS=8, SCK=9, MOSI=10, MISO=11, NRST=12, BUSY=13, DIO1=14.

## Getting deep-sleep current down (power gating) -- per HT-VME290 schematic + datasheets
`sleep current = ESP32-S3 chip (~10µA) + every ungated peripheral`. Offenders, biggest first:
- **LoRa SX1262 (HT-RA62, U8)** -- THE one that matters. Core is on the always-on `VDD_3V3` rail,
  so Vext does NOT kill it; un-slept it draws ~0.6-1.5mA (~1 week on 200mAh alone). Code already
  exists in the lib: `Platforms/WirelessPaper/power_controls.cpp` does a software-SPI `SetSleep`
  (0x84, 0x04) then sets LoRa pins high-Z. The VME290 platform lists the LoRa pins "only used for
  prepareToSleep()" but does NOT implement it -- ported into `sleep_test.cpp` (`loraSleep()`).
  Drops it to ~1µA. No rail cut / hardware mod needed.
- **Vext (GPIO18)** -- cuts the e-ink panel + its HV boost (L5/D4-D6) and the LoRa RF front-end.
  Leave OFF for sleep via `VExtOff()`. Safe: e-ink is bistable, holds its image with no power.
- **LED (GPIO45)** -- drive HIGH (off) before sleep. (LED1 orange is charge-status off VDD_5V,
  only lit while charging on USB -- ignore.)
- **Battery ADC divider already gated**: R13 390k / R15 100k (ratio 0.204, VBAT_Read = VBAT*0.204)
  switched by Q2 via `ADC_Ctrl` -- only draws while measuring. Leave disabled in sleep.
- **NO USB-UART bridge** -- native S3 USB (`ARDUINO_USB_MODE=1`). Nothing to do.
- **LDOs are NOT the floor**: CE6260 Iq = **6µA typ** (datasheet); always-on VDD_3V3 LDO ~6µA,
  switched Ve_3V3 LDO ~0.1µA when off. Charge IC (LGS4056H) adds a few µA.

Floor ~= S3 deep sleep (~10µA) + LDO (6µA) + charge IC (~few µA) + LoRa slept (~1µA) ~= **~20-40µA**.
LoRa is the whole ballgame; everything else is small. Measure to confirm.

## Battery life estimate (deep-sleep design; wakes negligible => runtime ~= capacity / floor)
| Sleep floor | 100mAh | 200mAh |
|-------------|--------|--------|
| ~20µA (best, clean board) | ~6-7 months | ~12-14 months |
| ~30µA (realistic)         | ~4-5 months | ~8-9 months   |
| ~40µA (conservative)      | ~3-3.5 months | ~6-7 months |
| **LoRa NOT slept (~1mA)** | **~4 days** | **~8 days** |

Practical: **100mAh ~= 3-6 months, 200mAh ~= 6-12 months** unplugged. Caveats: usable capacity
~80-85% of rating, and LiPo self-discharge (~2-3%/month) eats in over months -- treat the top
figures as theoretical. It recharges over USB on every drive, so it only bridges days/weeks between
charges. Cell size isn't the deciding factor -- sleeping the LoRa is.

## Charging + battery protection (from schematic)
- **Charges the LiPo: YES.** U6 **LGS4056H** = TP4056-class single-cell charger (CC/CV + termination)
  fed from USB `VDD_5V`. Orange LED1 = charge status. TEMP pin for a thermistor. Charge current set
  by the PROG resistor -- **verify vs a small cell**: often set ~0.5-1A, but a 200mAh cell wants
  <=~0.5-1C (~100-200mA), so a high PROG current could over-stress it.
- **Full BMS / protection: NO dedicated IC on the board.** Charger + USB/battery power-path mux
  (Q1/Q4/Q3/D2) + input fuse (F1), but no DW01-style over-discharge / over-current / short protection.
  That lives on the **battery's own PCB** -- use a **protected** cell, and add firmware low-voltage cutoff.
- **Firmware safeguard:** read VBAT and stop waking / show "low battery" before ~3.0V. Insurance against
  deep-discharge when left unplugged a long time.

## Battery read (custom -- not in the display lib)
heltec-eink-modules has no battery read. Divider known (0.204), gated by `ADC_Ctrl`/Q2. Missing: the
GPIO numbers for `VBAT_Read` (ADC) and `ADC_Ctrl` -- pull from Heltec's own VisionMaster board file/docs.
Then: assert ADC_Ctrl -> analogRead -> / 0.204 -> de-assert.

## USB-vs-battery detection
No dedicated VBUS-sense GPIO obvious in the schematic. Options: charge IC (LGS4056H) `CHRG`/`DONE`
pins, or sample `VBAT_Read` (~4.2V + charging => on USB). Check Heltec's board file first.

## Testing the sleep current  (-> `sleep_test.cpp`, env `[env:sleep_test]`)
The production firmware never sleeps, so it can't test battery life. `sleep_test.cpp` is the minimal
slice: LoRa sleep + `VExtOff` + LED off + `esp_deep_sleep`, wake on button (GPIO21).
- Flash: `pio run -e sleep_test -t upload`
- Measure: meter in series with the **battery** (JP1). Deep sleep -> **µA range**, target ~20-40µA.
  Wakes spike to tens of mA (switch to mA range or the µA fuse may trip).
- Read it: ~20-40µA => months confirmed, build the full firmware. ~1mA+ => LoRa didn't sleep, debug.

## TODO (battery firmware)
- [x] Minimal sleep-test sketch (`sleep_test.cpp`) -- LoRa sleep + Vext/LED off + deep sleep.
- [ ] Flash `sleep_test` + measure deep-sleep current on the battery line (confirm ~20-40µA).
- [ ] Find `VBAT_Read` + `ADC_Ctrl` GPIOs from Heltec's VisionMaster board file/docs.
- [ ] Battery read: assert ADC_Ctrl -> analogRead -> / 0.204 -> de-assert (divider 390k/100k).
- [ ] Power-source detect (charge-IC CHRG/DONE or VBAT_Read): USB -> stay awake + charge; battery -> deep sleep.
- [ ] Store `validTo` + "next permit cached" flag in RTC memory (survives deep sleep).
- [ ] Wake-source dispatch: button -> BLE sync; timer -> midnight flip / advertise window.
- [ ] Compute sleep duration from `validTo` (long normally; precise wake at midnight on expiry day).
- [ ] Last-day periodic advertise windows to auto-receive the new permit over BLE.
- [ ] Cache next permit in flash; midnight redraw from cache (no BLE needed).
- [ ] Low-voltage cutoff: stop waking / show "low battery" before VBAT ~3.0V (no hardware BMS).
- [ ] Verify charge current (PROG resistor) is safe for a 200mAh cell; use a protected cell.
- [ ] Confirm graceful failure: if the battery dies, the e-ink just holds the last image.
- [ ] Integrate into `main.cpp` behind power-source detect; retire the buyer's server-side 4 PM flip.
