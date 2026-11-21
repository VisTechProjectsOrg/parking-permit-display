# Parking Pass Display - ESP32 E-Ink

## Project Overview
ESP32-based e-ink display for showing Toronto parking permit barcode. Syncs via BLE from Android app.

## Build Commands

### Heltec Vision Master E290 (production)
```bash
pio run -e vision_e290 --target upload
pio device monitor
```

### Dev build (no deep sleep: serial + flashing keep working, button and check-ins still run)
```bash
pio run -e dev --target upload
```
`fake_low_battery` = dev build reporting 3.45V, to check the low-battery icon and app notification.
Flashing the production build onto a sleeping board: hold BOOT, tap RST, release BOOT, then upload.

### Standard ESP32 (testing, no display)
```bash
pio run -e esp32dev --target upload
pio device monitor
```

## Hardware
- **Board**: Heltec Vision Master E290 (ESP32-S3 + 2.9" e-ink)
- **Button**: GPIO 21 (short press = BLE sync, hold 3s = force redraw); wakes from deep sleep
- **LED**: GPIO 45 (active LOW)
- **Battery**: sense on GPIO 7, divider switched on by GPIO 46 (HIGH), multiplier 4.9 x 1.03
- **Charger**: LGS4056H, fixed ~500mA: use a protected cell of 500mAh or more
- **Display**: 296x128 e-ink

## How it runs (battery firmware)
Every wake is one pass through `setup()`, then deep sleep (~20uA measured):
1. Read battery. 2. Sync with the phone if a check-in is due (or button / power-on).
3. Redraw only if the permit that should show changed. 4. Sleep until the next check-in or handoff.
- Holds two permits (current + next) and switches at the next one's `validFrom` on its own.
- Clock comes from the phone on each sync (local time, stored as UTC); lost on power loss.
- Check-ins: 8 AM and 7 PM; every 30 min (7 AM-9 PM) once coverage ends within 24h;
  hourly if the clock is unset; none below 3.3V (button + handoffs only). Low-battery icon below 3.5V.

## Key Files
- `src/main.cpp` - Wake/sync/draw/sleep flow and drawing
- `src/permit_store.h` - Two-permit cache (NVS), permit time parsing, which permit to show
- `src/power_helper.h` - Pins, battery read, LoRa sleep, deep sleep
- `src/bluetooth_helper.h` - BLE client sync with the phone
- `src/Code39Generator.h` - Barcode rendering for Code39 format
- `src/permit_config.h` - Layout constants

## BLE Protocol
- Display is the BLE client; the phone (ParkingPermitSync) is the GATT server and advertises constantly
- Service `0000ff00-...`:
  - `ff01` read: permit JSON (permit fields, `displayFlipped`, `next` = cached next permit, `now` = phone local epoch)
  - `ff02` write: sync type (1 auto, 2 button, 3 long press)
  - `ff03` write: status `shownPermit,cachedPermit,batteryMv,fwVersion`
- The phone gets the permit from `https://fucktorontoparking.ca/api/permit` (no more GitHub `permit` branch)

## Branches
- `dev` - Working branch (battery firmware)
- `main` - Last released firmware
- `wifi-only` - Legacy WiFi version (pulls from GitHub)
- `permit` - Old permit JSON data branch, no longer updated (the app reads the site now)

## Notes
- Requires 3s USB CDC delay on ESP32-S3 for serial output
- Permit data persists in flash (Preferences library); the old single-permit keys are migrated on first boot
- Display can be flipped 180 via app setting
- E-ink retains image when powered off
