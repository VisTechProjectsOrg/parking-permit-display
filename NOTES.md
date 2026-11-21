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
- **RTC timer wake at the handoff** -- the next permit's `validFrom` (its actual start; in practice
  local midnight / 00:00) -> redraw the cached next permit -> sleep (no radio).
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

**Result (measured 2026-06-25):** active ~40mA (CPU) with ~70mA peaks (e-ink refresh), **deep
sleep = 0.02mA (~20µA)** -- best-case floor, LoRa confirmed asleep. Validates ~13-14 months
theoretical on 200mAh (~6-10 realistic after derating + self-discharge). Premise proven on hardware.

## TODO (battery firmware)
- [x] Minimal sleep-test sketch (`sleep_test.cpp`) -- LoRa sleep + Vext/LED off + deep sleep.
- [x] Flash `sleep_test` + measure deep-sleep current -- **measured ~20µA (0.02mA) on 2026-06-25.** PASS.
- [x] `VBAT_Read` = GPIO7, `ADC_Ctrl` = GPIO46 active HIGH (Meshtastic `heltec_vision_master_e290/variant.h`, multiplier 4.9 x 1.03).
- [x] Battery read in `power_helper.h` (divider on only while measuring).
- [x] Power-source detect: dropped. Same sleep cycle on USB and battery; USB just charges. Simpler, and a
      car USB port with no data would have looked like battery anyway.
- [x] Wake dispatch: button -> sync; timer -> check-in and/or handoff; power-on -> sync + redraw.
- [x] Two permits cached in NVS; handoff at the next permit's `validFrom` from cache, no BLE needed.
- [x] Display scans for the phone instead of advertising (the phone already advertises 24/7):
      8 AM + 7 PM daily, every 30 min by day once coverage ends within 24h.
- [x] Low battery: icon below 3.5V, timed check-ins stop below 3.3V (button + handoffs still work).
- [x] Charge current: schematic V0.3 has R18 = 1k on PROG -> ~1A (LGS4056H: I = 1000/R_PROG; a retailer
      says 500mA, unmeasured). Too much for the 200mAh / 0.2A cell. Plan: protected 1000mAh+ cell rated
      1A+. Alternative: R18 -> 4.7k-10k (~100-210mA) keeps a small cell. TEMP is tied to GND (no thermal
      cutoff); Q4 gives reverse-polarity protection on the battery plug.
- [ ] VERIFY on hardware: battery reading vs a multimeter; sleep current with the new firmware
      (target still ~20uA); a full sync cycle; the midnight handoff.
- [ ] VERIFY: clock drift in deep sleep (runs on the internal RC clock). Each phone check-in (twice a
      day) resets it, so it only matters if the phone isn't near the car for days. The schematic shows a
      32.768kHz crystal (X1) on XTAL_32K_P/N: if drift is a problem, switch the RTC slow clock to it
      (needs care, a bad switch breaks timekeeping and the handoff).
- [ ] Confirm graceful failure: if the battery dies, the e-ink just holds the last image.
- [ ] Retire the buyer's server-side 4 PM flip once the battery firmware is proven.

---

# Set-and-forget sync design (no cables, charge ~every 6 months)

Use model: never plug in to sync; charge maybe twice a year. So the weekly permit handoff must
happen over BLE **while on battery**, and the e-ink is **enforcement-facing** (a stale/expired
barcode = a real ticket -- that's how the last ticket happened), so caching the next permit before
the midnight flip has to be reliable.

## Advance purchase (buyer side) -- CONFIRMED the site allows it
The Toronto site accepts a **future start date** (verified 2026-06-25: bought a permit Valid From
Jul 02 while today was Jun 25). So:
- Buyer buys the next permit **~2-3 days early** (configurable lead time), with **start date =
  current permit's expiry + 1 day** (today it's hardcoded to "tomorrow" -- needs changing).
- This widens the window for the display to catch the BLE sync before the handoff.
- Tradeoffs of going too early: money tied up sooner, price-change exposure, wasted permit if the
  vehicle is switched during the window. So "a few days," not a week.

## Time source -- app sets the clock (required)
The display has no clock that survives a dead battery. **The app sends current LOCAL time over BLE
on each sync** -> sets the ESP32 RTC. The display uses it to know "midnight" (flip) and "daytime"
(advertise windows) and to compute sleep duration. Local time from the app => DST handled for free.

## Display behaviour on battery
- Holds **two permits** (current + next) and runs the on-device version of the server-side flip:
  show current until its `validTo`, then **flip to the cached next permit at its `validFrom`** --
  the actual handoff time, which in practice is local midnight (00:00) but use the real timestamp.
- **Sleep flat at ~20µA** for the quiet ~5 days.
- **Last ~2 days:** duty-cycled advertise (e.g. ~3s every 15 min) **only during daytime hours**
  (~8am-8pm) so the phone can push the new permit when you next drive. This is the main battery
  lever -- measure + tune the cadence like we did the sleep floor.
- **Button = manual force-sync**, near-zero standby cost -- the reliable backstop that lets the
  auto-advertise cadence stay conservative.

## App + site tracking (display has no internet -- all telemetry via the app)
- App pushes the permit, then **reads back the display's stored permit # over BLE to confirm sync**;
  records permit# + timestamp + battery level.
- App **reports up to the site** (app has internet; the display never does). Site shows last-synced
  time / permit on display / battery % -- as fresh as the last app<->display contact.
- App **notifies** if the display is behind (mismatch / no contact in N days) **or battery is low**.

## Low battery
- E-ink shows a low-battery icon; app push notification; site flags it. Warn around ~3.4V, hard
  stop ~3.0V (no hardware BMS -- use a protected cell too).

## Edge cases to handle
- **Lost clock after dead/recharged battery:** time unknown -> advertise aggressively + show
  "needs sync" + rely on button, until the app supplies time + permit.
- **First run / nothing cached:** same fallback (advertise, get time + permit from app).
- **Vehicle switch during the advance window:** pre-bought permit is wasted (known tradeoff).

## Open / decided
- [x] Site allows future start date (verified) -> advance purchase is viable.
- [x] Lead time = **2-3 days** (configurable).
- [x] Low-battery = e-ink icon + app notif + site flag.
- [x] Cadence (as built, the display scans for the phone rather than advertising): check-ins at
      8 AM and 7 PM; every 30 min 7 AM-9 PM once coverage ends within 24h; hourly with no clock.
- [x] BLE protocol: see CLAUDE.md (pairing by owner ID, display ID, status with dev counters).
- [ ] Buyer change: start date = current expiry + 1, configurable lead time. Checklist in the
      buyer repo's NOTES.md ("Next session checklist"). Do this next.

## Where things stand (2026-11-21)
Built and live: pairing with one phone (owner ID, 10s reset, remote unpair), several displays per
phone, first-run setup in the app, activity log, check-in stats from the display (missed
check-ins, restarts), low-battery alerts by days left, skip-while-away on the site.

Decided, keep as is:
- **Check-in schedule stays.** Battery cost depends on the number of check-ins, not when they
  happen, and sleep (~3.4 of ~6 mAh/week) dominates. Rough week: sleep ~3.4, routine check-ins
  ~2.0, last-day check-ins ~0.5 => over a year on a 500 mAh cell. Moving the evening check-in
  later (9-10 PM) is fine if the phone is near the car then; midnight is worse.
- **Don't drop routine check-ins** to sleep until the buy day: they keep the clock corrected,
  feed the "hasn't checked in" alert, and pick up mid-week changes. 1/day is the most to cut.
- The expensive case is last-day check-ins with **no phone in range** (up to 28 tries, ~5 mAh).
  Buying ahead fixes that better than any schedule change.

VERIFY before changing the schedule (unmeasured):
- [ ] Energy of one check-in (estimated ~0.17 mAh: ~6 s at ~100 mA).
- [ ] RTC drift per day. Idea: at each sync, log (phone time - display time) to the activity log.
      At 1% drift a week without a check-in moves the midnight switch by over an hour.
- [ ] Real drain from the app's days-left estimate after a few weeks in the car.

Ideas, not started:
- **Show charging on the e-ink automatically (hardware mod).** The board has no USB-present pin, so
  the display can't tell it's plugged in. Plan (board is **V0.3.1**, matches
  `datasheets/HT-VME290_Schematic_V0.3.1.pdf`; the datasheet's header table is a different revision):
  - **Chip side: GPIO17 = header P4 pin 35.** Free in both docs, RTC-capable (deep-sleep wake), not a
    strapping pin (0/3/45/46), no pull-up or other part on it in the schematic. No soldering.
  - **Signal: VDD_5V (USB)** via a divider, 100k top / 150k bottom => ~2.85-3.15V over USB 4.75-5.25V,
    0V unplugged (no battery drain). Header pins 2/4 ("5V" in the datasheet) are **not connected on
    V0.3.1**, so this needs one soldered wire to a VDD_5V point: R17's right end, U6 CE (pin 8) or
    C26 per the schematic.
  - Not CHRG: on this board U6 DONE (pin 6) is tied to GND, and CHRG idles near 5V through R17 330R
    + LED1, so it can't tell full from unplugged and isn't 3.3V-safe without protection.
  - Firmware: ext1 wake on GPIO17 high (plug-in) and low (unplug); button stays on ext0. "Full" from
    voltage. Replaces the guesswork in charging mode.
  - **VERIFY before soldering:** meter the chosen pad (5V on USB, 0V off); ESP32-S3 datasheet for
    GPIO17 input limits and ext1 wake modes (not read yet); cold review of the plan.
  Until then (firmware 6): a button press after plugging in, or a restart while plugged in that sees
  the voltage climb, starts charging mode (bolt on the e-ink and in the app, polls every 5 min,
  clears on unplug or after 6 h).
- **Permit per display:** each display tied to a car, so one stays in each car. Usually only one
  permit is active, so the autobuyer keeps buying for the default vehicle; needs `/api/permit?plate=`
  and a Car picker per display in the app. Two cars at once would mean per-car autobuy (bigger).
- **Buy now** button for "need a permit today" after midnight: see the buyer repo's NOTES.md.
