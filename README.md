# Toronto Parking Permit Display

![Platform](https://img.shields.io/badge/platform-ESP32-orange)
![Display](https://img.shields.io/badge/display-E--Ink%20296x128-green)
![BLE](https://img.shields.io/badge/BLE-Bluetooth%20LE-blue)

![Display Photo](docs/display.jpg)

Battery-powered ESP32 e-ink display that shows your Toronto temporary parking permit. Gets new permits over Bluetooth from the [parking-permit-android](https://github.com/VisTechProjectsOrg/parking-permit-android) app and switches to the next permit on its own at midnight. No cable needed.

## Hardware

- [Heltec Vision Master E290](https://heltec.org/project/vision-master-e290/) (ESP32-S3 + 2.9" e-ink)
- 1S LiPo on the JST 1.25mm battery plug: protected cell, 1000mAh+ rated 1A+ (the board charges at ~1A)
- Button on GPIO 21, LED on GPIO 45, battery sense on GPIO 7

## How It Works

```mermaid
flowchart LR
  B[Buyer on the server] -->|permit.json| S[Site /api/permit]
  S -->|daily + on open| A[Android app]
  A -->|BLE, at each check-in| D[E-ink display]
```

1. [parking-permit-buyer](https://github.com/VisTechProjectsOrg/parking-permit-buyer) buys the next permit on the server.
2. The app fetches it from the site and keeps advertising over Bluetooth.
3. The display deep-sleeps (~20µA) and wakes to check in with the phone: 8 AM and 7 PM, and every 30 minutes during the day once the current permit ends within 24 hours.
4. It stores the current and next permit, gets the time from the phone, and switches to the next permit when it starts.
5. It reports its battery level back, which the app shows.

## Usage

| Action | Result |
|--------|--------|
| **Short press** | Sync with the phone now |
| **Long press (3s)** | Sync and redraw even if nothing changed |
| **Hold 10s** | Reset: forget the phone and the permits (LED goes out at 3s, back on at 10s) |
| **Low battery** | Battery icon on the display, notification in the app |

## Setup

### 1. Upload Firmware

```bash
pio run -e vision_e290 --target upload
```

On a sleeping board: hold BOOT, tap RST, release BOOT, then upload. For development, `pio run -e dev --target upload` builds a version that never sleeps.

### 2. Install Android App

Install [parking-permit-android](https://github.com/VisTechProjectsOrg/parking-permit-android) and allow Bluetooth, battery and "keep running" access when it asks.

### 3. Sync

Press the display's button with the phone nearby. It pairs with that phone and only syncs with it from then on, and after that it checks in on its own. One phone can have several displays: each shows up in the app's Display Settings, where it can be renamed, flipped or forgotten. A forgotten display unpairs at its next check-in.

## BLE Protocol

The phone runs the GATT server (service `0000ff00-...`) and advertises an 8-byte owner ID as service data on `ff00`; a paired display only connects to the phone with its owner ID. The display connects as a client:
- `ff02` write: sync type (auto, button, long press) followed by the display's 12-char ID
- `ff01` read: permit JSON with the cached next permit and the phone's local time, or `{"unpair":true}` for a display forgotten in the app
- `ff03` write: status `shownPermit,cachedPermit,batteryMv,fwVersion,missNotFound,missFailed,lastRestartReason,restarts` (misses since the last good check-in; restart reason = `esp_reset_reason()`; falls back to the first four fields if the MTU is still 23)

## Files

- `src/main.cpp` - Wake, sync, draw, sleep
- `src/permit_store.h` - Current + next permit, which one to show
- `src/power_helper.h` - Battery read, LoRa sleep, deep sleep
- `src/bluetooth_helper.h` - BLE sync with the phone
- `src/permit_config.h` - Display layout constants
- `src/Code39Generator.h` - Barcode rendering

## Branches

- `main` / `dev` - Battery BLE version (current)
- `wifi-only` - Original WiFi version
- `permit` - Old permit data branch, no longer used

## Related Projects

- [parking-permit-buyer](https://github.com/VisTechProjectsOrg/parking-permit-buyer) - Buys permits and serves them from the site
- [parking-permit-android](https://github.com/VisTechProjectsOrg/parking-permit-android) - Android companion app
- [Parking permit website](https://fucktorontoparking.ca/) - Web dashboard

## Notes

- E-ink keeps the image with no power, so a dead battery still shows the last permit
- Display can be flipped 180° in the app's display settings
