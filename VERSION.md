# Electricity Price Ticker – Version Information

## Current firmware

- **Version:** 7.2
- **Release date:** 2026-08-04
- **Target MCU:** Seeed XIAO ESP32‑C3
- **Display:** 20x4 I²C LCD (PCF8574, default address `0x27`)
- **API endpoint:** `https://api.energy-charts.info/price?bzn=SI`
- **Resolution:** 15‑minute intervals, hourly averages for overview

## Highlights of v7.2

### Button Robustness & Screen-Control Fixes

Bug-fix release that resolves the three control glitches reported for the v7.1
firmware on the Seeed XIAO ESP32‑C3: the primary screen looked unscrollable,
double-clicks failed to switch to the secondary status screen, and the end-of-day
behaviour (no tomorrow data yet) was unstable around 22:00–23:59. No fee/VAT
math, NVS layout, API scheduling or 48-hour scrolling behaviour was changed.

#### Fix 1 – Primary-screen scroll now works at any hour of the day

Two compounding bugs in `displayPrimaryList()` and `displayPriceRow()` were
cancelling each other out and made single-click scrolling look dead:

- `displayPriceRow()` only blanked past hours while `currentHour < 22`. After
  22:00 the screen could repaint already-finished morning hours, so as soon
  as the user scrolled forward the new "top" hour was visually over-written
  by the previous morning's data. The guard is now
  `if (localHourIndex < currentHour) blank();`, so past hours of today are
  hidden at every hour of the day.
- `displayPrimaryList()` contained an override
  `if (currentHour >= 21 && timeOffsetHours > 0) displayStartHourOffset = 21 + timeOffsetHours;`
  which pinned the top row at 21:00 + offset from 21:00 onward. At 22:15
  every click computed start = 22+offset, was then clamped to 21+offset, and
  the user saw no movement. The override is no longer needed and has been
  removed.

#### Fix 2 – Double-click on the secondary screen now fires reliably

The double-click path itself was correct; it was being starved by a false
"Long press detected!" message that fired immediately after every reset.
On the ESP32-C3 the button pin (`INPUT_PULLUP`) floats HIGH for a few
seconds during boot, while `buttonPressStartTime` is initialised to `0`.
As soon as `millis()` crossed the 3 s threshold, the long-press detector
tripped on a phantom 3-second hold, cleared the LCD to
"Long press detected! / Release to refresh", and from then on the user
could not see any prices to click on (single- and double-click recognisers
both still ran, but their visible effect was hidden behind the long-press
splash).

Fix: a new `bool buttonEverReleased` is set to `true` the first time the
pin is observed LOW after boot, and the long-press detector is gated on
it: `if (buttonState == LOW && !longPressDetected && buttonEverReleased)`.
The detector refuses to fire until the user (or the power-rail noise) has
released the button at least once. The v7.1 button logic (debounce, 3 s
long-press threshold, 500 ms double-click window, TTP223 timing) is
otherwise preserved verbatim.

#### Fix 3 – End-of-day scroll is now stable when no tomorrow data is available

`advanceDisplayOffset()` contained a hack
`if (allowedAhead < 2 && currentHour >= 21 && !isTomorrowDataAvailable) allowedAhead = 2;`
which at 22:00 (with no tomorrow data) let the user click past hour 23 into
"24:00 / 25:00", where `displayStartHourOffset` wrapped back to 0 and filled
the LCD with the now-unblanked past-hour rows. The hack has been removed;
the natural cap is now sufficient:

- **22:00** → can step 22 → 23, then wraps back to current
- **23:00** → cannot step forward at all

No wrap to 00:00 of the previous day is reachable any more.

#### Fix 4 (bonus) – Auto-return timer now resets on every click

`lastButtonActivity` / `autoScrollExecuted` were only updated in the
primary-list branches of `advanceDisplayOffset()`. Scrolling the secondary
status page therefore did not push the 10 s auto-return-to-top timeout
forward. The two resets are now at the top of `advanceDisplayOffset()` so
every successful click (single, double, or long-press-release) refreshes
the timer regardless of which list is showing.

#### Cosmetic / non-behavioural changes

- Filename and three user-visible version strings bumped to v7.2:
  - `connectToWiFi()` splash: `"Elec. Rate SI v7.1"` → `"v7.2"`
  - `displaySecondaryList()` credit line: `"price ticker v7.1"` → `"v7.2"`
  - `setup()` debug banner: `"v7.1 (Neg Price Fee)"` → `"v7.2 (Button Robustness)"`
- Inline comments added at each fix site explaining what v7.1 did wrong,
  so future maintainers do not re-introduce the overrides.
- New global flag `bool buttonEverReleased` (see Fix 2).

See `CHANGELOG.md` for full implementation details.

---

## Highlights of v7.1

### Negative Price Provider Fee

Added a separate provider fee for negative spot prices via a new constant
`NEG_PRICE_COMPANY_FEE_PERCENTAGE`. Positive and negative market prices now
use independent fee multipliers, correctly modelling contracts where the
provider's fee structure differs between the two cases.

**Price calculation:**

| Market price | Formula |
|---|---|
| Positive (`raw >= 0`) | `raw × (1 + POWER_COMPANY_FEE_PERCENTAGE/100) × (1 + VAT_PERCENTAGE/100)` |
| Negative (`raw < 0`) | `raw × (1 - NEG_PRICE_COMPANY_FEE_PERCENTAGE/100) × (1 + VAT_PERCENTAGE/100)` |

VAT is applied to both, consistent with net billing where VAT is calculated
on the monthly net sum (linear equivalence applies).

All 5 fee calculation sites updated: `updateLeds()`, `format15MinPrice()`,
`displayPriceRow()`, `displaySecondaryList()` (daily average), and version strings.

See `CHANGELOG.md` for full implementation details.

---

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
