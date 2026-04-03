# Electricity Price Ticker – Version Information

## Current firmware

- **Version:** 7.0
- **Release date:** 2026-04-03
- **Target MCU:** Seeed XIAO ESP32‑C3
- **Display:** 20x4 I²C LCD (PCF8574, default address `0x27`)
- **API endpoint:** `https://api.energy-charts.info/price?bzn=SI`
- **Resolution:** 15‑minute intervals, hourly averages for overview

## Highlights of v7.0

### MAJOR UPGRADE: Rolling 48-Hour Logic & Midnight Bridge

This version is the **"Golden Build"** for this hardware. It represents the culmination of hardware stability fixes from v6.2.4 combined with revolutionary new 48-hour price prediction capabilities.

#### 1. The Midnight Bridge (Rollover Logic)

The most complex part of electricity tickers is handling the midnight transition. This code now correctly detects the moment the local clock moves from 23:59:59 to 00:00:00.

**The Swap:** Instead of waiting for a slow API call at midnight (which usually fails because the server hasn't updated yet), the code instantly promotes the "Tomorrow" buffer to become "Today" data.

**The NVS Update:** The code correctly serializes the new "Today" data and saves it to NVS immediately after the swap. This ensures that if power cuts at 00:05 AM, the device reboots with the correct data already loaded.

#### 2. Dual-Buffer NVS System

The ticker now stores "Today" and "Tomorrow" data independently in NVS:

- **Today buffer (`doc`)**: Contains the current day's price data
- **Tomorrow buffer (`docTomorrow`)**: Contains the next day's price data
- **NVS keys**: `data_prc`/`data_day`/`data_mon`/`data_year` for today, `data_prc_t`/`data_store_t` for tomorrow

#### 3. Smart Fetching & API URL

The logic for fetching tomorrow's data is implemented correctly:

- **URL Construction**: Adding `&start=YYYY-MM-DD` dynamically after 14:00 (2 PM) queries the Energy-Charts API for the next day
- **Validation**: In `processJsonData()`, the code compares the timestamp in the JSON against the target date, preventing the "Tomorrow" buffer from being filled with "Today's" data if the API is lagging

#### 4. Seamless 48H Scrolling

If next-day data is available, the button allows scrolling up to **47 hours ahead**:

- **Visual Distinction**: Using `HH:>>` for tomorrow's hours prevents the user from confusing a cheap price "tomorrow" with a cheap price "today"
- **Index Safety**: The code correctly uses `lowestPriceIndexTomorrow` and `highestPriceIndexTomorrow` when the display is in the "tomorrow" range, ensuring the Min/Max icons appear on the correct 15-minute segments

#### 5. Hardware Stability (Inherited from v6.2.4)

All v6.2.4 hardware stability fixes are preserved:

- **Refresh Logic**: "State-Based" refresh ensures the display updates exactly at 00, 15, 30, and 45 minutes past the hour, even if the CPU is busy with a background fetch
- **LED Indicators**: White LED for low price and Built-in LED for connectivity remain pinned to the actual current price, even when the user is scrolling through future data on the screen

#### Final "Sanity Check" Verdict

**Status:** Verified. The code is safe to deploy. The transition from 15-minute intervals to the midnight rollover is now seamless. The "1 AM fetch gap" that plagues most electricity tickers has been successfully bypassed.

---

## Highlights of v6.2.4

- **BUG FIX:** Exact-boundary display refresh bug
- Problem: At the exact top of the hour (e.g., 20:00:00), the display automatically refreshed but showed the PREVIOUS hour's data (19:00). This happened because the "next-boundary" rounding logic in findCurrentPriceIndex() incorrectly excluded the current interval if the time was exactly on the boundary.
- Fix: Simplified findCurrentPriceIndex() to use a robust "last entry <= now" comparison. This ensures the display transitions to the new hour instantaneously at XX:00:00.

## Highlights of v6.2.3

- **BUG FIX**: State-based display refresh logic
- Problem: Screen would occasionally fail to update if the ESP32 was busy (fetching data or reconnecting WiFi) during the exact 00/15/30/45 minute mark.
- Fix: Switched from "Event-Based" (refresh only AT minute X) to "State-Based" (refresh IF current time != last refresh time). This ensures the screen updates immediately even if the device was busy during the transition.

## Highlights of v6.2.2

- **BUG FIX**: Display blank lines issue
- Problem: Sometimes rows 0 and 1 (current 15-min prices and current hour) were blank
- Cause: The "hour suppression" logic was hiding the current hour unexpectedly
- Fix:
  - Row 1 (current hour) now ALWAYS shows - suppression logic only applies to rows 2-3
  - Row 0 (15-min details) also always shows for the current hour

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

For full details, see:

- [CHANGELOG.md](./CHANGELOG.md)
- [README.md](./README.md)
