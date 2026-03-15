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
