# Electricity Price Ticker – Version Information

## Current firmware

- **Version:** 6.2.1
- **Release date:** 2026-03-29
- **Target MCU:** Seeed XIAO ESP32‑C3
- **Display:** 20x4 I²C LCD (PCF8574, default address `0x27`)
- **API endpoint:** `https://api.energy-charts.info/price?bzn=SI`
- **Resolution:** 15‑minute intervals, hourly averages for overview

## Highlights of v6.2.1

- **BUG FIX**: Fixed `findCurrentPriceIndex()` to return the correct current interval.
- Problem: At 17:57, it returned index for 18:00 instead of 17:45, causing display to show hour 18 instead of hour 17.
- Fix: Now calculates next 15-minute boundary and finds the last entry before that boundary.

## Highlights of v6.2.0

- **CRITICAL FIX**: DST (Daylight Saving Time) handling is now fully fixed for all days.
- Previously, the code assumed every day has exactly 96 price entries (24h × 4). This caused incorrect price display on DST switch days:
  - Spring forward (March): Only 92 entries → wrong prices displayed
  - Fall back (October): 100 entries → wrong prices displayed
- **Solution**: All price lookups now use timestamp-based searching through the `unix_seconds` array instead of arithmetic calculation (`hourIndex * 4`).
- New functions: `findPriceIndexForHour()`, `findCurrentPriceIndex()`, `getHourFromPriceIndex()`
- Updated functions: `getHourlyAverage()`, `display15MinuteDetails()`, `displayPriceRow()`, `displayPrimaryList()`, `updateLeds()`
- The ticker now works correctly on all days, including DST switch days, with no manual intervention.
- **Future-proof**: If EU cancels DST, only the `TZ_CET_CEST` string needs updating (one line of code).

For full details, see:

- [CHANGELOG.md](./CHANGELOG.md)
- [README.md](./README.md)

## Previous firmware

- **Version:** 6.1.2
- **Release date:** 2026-03-11
- **Target MCU:** Seeed XIAO ESP32‑C3

## Highlights of v6.1.2

- Fix: restore proper white LED price indicator behavior on ESP32 by avoiding mixing PWM (`analogWrite`) and `digitalWrite` on the same pin.
- Fix: LED is now truly off when backlight/LED gating turns it off (no more "dim glow").

## Earlier firmware

- **Version:** 6.1.1 (2026-03-07) – Daily low/high marker includes negative and zero prices
- **Version:** 6.1.0 (2026-01-30) – Midnight fetch and market day detection fixes
- **Version:** 6.0.0 (2026-01-27) – NVS storage and daily fetch

For detailed history, see [CHANGELOG.md](./CHANGELOG.md).
