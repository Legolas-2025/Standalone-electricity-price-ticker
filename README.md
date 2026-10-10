![Standalone ESP32 Electricity Price Ticker](device_photo.jpg)

# Electricity Price Ticker for XIAO ESP32‑C3 (Energy‑Charts)

This project is an Arduino‑IDE‑friendly firmware for the **Seeed XIAO ESP32‑C3** that:

- Connects to Wi‑Fi.
- Fetches **day‑ahead electricity prices** from [Energy‑Charts.info](https://energy-charts.info).
- Computes final consumer prices (including configurable power‑company fee + VAT).
- Displays current and upcoming prices on a **20x4 I²C 2004 LCD**.
- Uses a white LED and an optional presence sensor to give quick visual feedback.
- Stores daily price data in **NVS** to survive reboots and reduce API calls.

The latest sketch implements **Version 7.5** — DST `mktime()` hardening,
explicit date bounds on today's API fetch, a complete sweep of the
remaining non-reentrant `localtime()` call-sites, a real fall-back-day
bug in the daily average / min-max calculation, and two small additions
to the fetch path. No fee/VAT math, NVS layout, 48-hour scrolling
behaviour, Midnight Bridge logic, or button handling was changed.

---

## Version Highlights

### v7.5 - DST Hardening + API URL Date Bounds + Fall-Back Average Fix (2026-10-10)

Changes from v7.4: DST hardening, explicit API URL date bounds, a complete
`localtime_r()` sweep, a real fall-back-day bug in the daily average, and two
small additions to the fetch path. Button handling, fee/VAT math, NVS layout,
48-hour scrolling behaviour and Midnight Bridge logic are unchanged, and all
v7.3 and v7.4 fixes are preserved verbatim.

- **Fix A – `tm_isdst = -1` before `mktime()`.** The calendar-day increment
  (`tm_mday += 1; tm_hour = 12`) was followed by `mktime()` without resetting
  `tm_isdst`, so the source day's DST state leaked into the normalisation. Both
  sites now set it first.
  *Measured:* the ±1 h offset is real, but the midday anchor keeps the calendar
  day — the only part this firmware consumes — correct either way, so this is
  defensive hardening rather than a reachable mis-display in v7.4.
- **Fix B – Explicit `&start=&end=` on today's fetch URL.** Today's fetch no
  longer uses the bare `api_url`, removing the dependence on server-side
  caching of the bare endpoint.
  *Verified against the live API:* `end` is **inclusive**, so `start == end`
  returns exactly one market day (96 entries normally, 92 on a spring-forward
  day, 100 on a fall-back day), interpreted in local exchange time. The
  reported stale two-month-old window did not reproduce at the time of
  writing, so this is hardening against a non-deterministic server default.
- **Fix C – `localtime()` → `localtime_r()` (8 call-sites, 7 functions).**
  `localtime()` returns a pointer to a single shared static `struct tm`, so
  every call invalidates the previous result. `handleDataFetching()` is the one
  that mattered — it dereferenced the result with **no NULL check**, which
  would have been a NULL dereference during the busiest part of the day.
- **Fix D – DST-aware hour-block detection in the daily average.** On the
  fall-back day local 02:00 occurs **twice**, so the day holds 25 hour-blocks
  and 100 quarter-hour entries. The average / min-max loop compared `tm_hour`
  alone, so the second 02:xx block was skipped entirely — averaging 24 blocks
  instead of 25, and misplacing the low/high marker if that block held the
  day's extreme. Block detection now compares `tm_hour` **and** `tm_isdst`.
  *Verified:* on a simulated 25-hour fall-back day the loop went from 24
  blocks averaged to 25, and a −30.00 minimum that was previously invisible is
  correctly located. The 24-row display still shows the first 02:xx block,
  which is correct for a 24-row layout — only the aggregates changed.
- **Fix E – today-fetch no longer spins the loop in a blocking `http.GET()`.**
  Both rejection branches advanced `nextScheduledFetchTime` only when
  `fetchTomorrow` was true, so a rejected today-fetch left the schedule in the
  past and `handleDataFetching()` re-entered on the next iteration — a
  blocking request every loop iteration. Simulated: 12 fetches / 6000 ms
  blocked before, 1 fetch / 500 ms after. A single forward-progress guard now
  matches the v7.4 Fix B guard on the tomorrow path.
  *Note:* the gap is identical in v7.4 — this is not a v7.5 regression. Fix B
  reduces how often it is entered.
- **Fix F2 – the HTTP request is measured rather than guarded.** A wall-clock
  stall guard was considered and not adopted: it would have had to measure
  `millis()` *after* `http.GET()` had already returned, so it could not
  prevent any blocking while risking the discard of a large but valid
  response. Every request now prints its actual duration instead, so a future
  freeze is measurable on the serial monitor.
- **Fix G – no fetch is *started* while the user is pressing the button.** The
  ISR records the time of the last physical edge; `handleDataFetching()` returns
  early if that edge was under 800 ms ago, which covers the full double-click
  window plus the confirmation wait. The fetch is only *deferred*, never
  cancelled, so the data still arrives.
- **Cosmetic.** Version strings bumped to v7.5; the long-press log line now
  carries the measured press duration.

**Known limitation.** `http.GET()` is still synchronous and this ESP32-C3 is
single-core, so an in-flight fetch still pauses the display and LED animation.
Fix G makes a fetch/click collision much less likely but does not make an
in-progress fetch non-blocking; removing the freeze requires porting the fetch
to the asynchronous `esp_http_client` API — a substantial rewrite deliberately
not undertaken here. A second limitation, inherited unchanged from v7.4: a
bounce gap within ~10 ms of the 50 ms `debounceDelay` can split one press into
two. Any fixed threshold has that edge, and real tactile switches bounce far
below it.

### v7.4 - Fetch Scheduling Fix: Button Responsiveness (2026-10-04)

Bug-fix release that eliminates the afternoon/evening button freeze. After
14:00 local time, if the Energy-Charts API had not yet published next-day
prices (typical until ~01:00–02:00 UTC = 03:00–04:00 CEST), the main loop
called `fetchAndProcessData(true)` every iteration because
`nextScheduledFetchTime` was never advanced after a failed or rejected
fetch. Each iteration blocked the loop for 5–15 s inside `http.GET()`,
starving `handleButton()`. No fee/VAT math, NVS layout, API URL
construction, DST logic, 48-hour scrolling behaviour, or Midnight Bridge
logic was changed. All four v7.3 DST fixes and all four v7.2 button/screen
fixes are preserved verbatim.

- **Fix A – Advance `nextScheduledFetchTime` after every failed tomorrow fetch.**
  Every failure/rejection path in `fetchAndProcessData()` now advances the
  schedule by 1800 s (30 min) when `fetchTomorrow == true`, breaking the
  tight retry loop that starved `handleButton()` from 14:00 onward.
- **Fix B – Belt-and-suspenders guard in `handleDataFetching()`.**
  After calling `fetchAndProcessData(true)`, if `isTomorrowDataAvailable`
  is still false, `nextScheduledFetchTime` is forced to at least
  `now + 1800`. Protects against any future refactor that removes the
  advance from the helper.
- **Fix C – Long-press detector: only honour genuine long presses.**
  The press handler now checks `pressDuration >= longPressThreshold`
  before honouring `longPressDetected`. A spurious idle-time flag (the
  detector previously measured time since last release, not press duration)
  no longer hijacks a normal short click into a forced manual refresh.
- **Fix D – Button edge interrupt (non-blocking press capture).**
  A `CHANGE` interrupt on `buttonPin` sets a volatile flag. At the top of
  `handleButton()`, if the flag is set and the pin is now released, a
  synthetic press+release event is injected into the state machine. This
  guarantees that a press occurring during a legitimate 10–15 s HTTP block
  is not silently lost.

### v7.3 - DST Edge-Case Hardening (2026-10-03)

Bug-fix release that corrects three DST-related edge cases identified by
static analysis of the v7.2 firmware. No fee/VAT math, NVS layout, button
logic, API scheduling or 48-hour scrolling behaviour was changed.

- **Fix A – Date-validation gate in `processJsonData()` was a no-op.**
  Two consecutive `localtime()` calls return the same static pointer; the
  second call silently overwrote the first result, making the date comparison
  always `X == X` (unconditionally true). Stale or wrong-day payloads could
  be accepted silently. Fixed by using `localtime_r()` into two separate
  `struct tm` variables.
- **Fix B – "Tomorrow" URL date wrong on spring-forward Saturday evening.**
  Adding `+24*3600` UTC seconds near a DST boundary could land on the day
  after tomorrow. Fixed by advancing the calendar day (`tm_mday += 1`) and
  re-normalising with `mktime()`, using a midday anchor (12:00) to stay away
  from DST boundary hours.
- **Fix C – Fall-back day (25-hour) daily average was slightly off.**
  The averaging loop iterated `hour = 0..23` and called
  `findPriceIndexForHour(2)` once, missing the second 02:xx block on
  DST fall-back days (100 price entries). The loop now scans by array index
  so both 02:xx blocks are included.

### v7.2 - Button Robustness & Screen-Control Fixes (2026-08-04)

Bug-fix release. Resolves the three control glitches reported for v7.1:
"cannot scroll the primary screen at 20:35", "double-click does not switch
to the secondary screen", and "strange end-of-day behaviour at 22:15 with
no tomorrow data". No fee/VAT math, NVS layout, API scheduling or
48-hour scrolling behaviour was changed.

- **Fix 1 – Primary-screen scroll** works at any hour of the day:
  past-hour blanking in `displayPriceRow()` simplified from
  `currentHour < 22` to a plain `localHourIndex < currentHour`, and the
  `currentHour >= 21` override in `displayPrimaryList()` removed.
- **Fix 2 – Double-click** reliably toggles to the secondary screen.
  A new `bool buttonEverReleased` flag prevents a false
  "Long press detected!" from firing right after every reset on the
  ESP32-C3 (floating button pin + `buttonPressStartTime = 0` was making
  the long-press detector trip on phantom 3-second holds during boot).
- **Fix 3 – End-of-day scroll** is stable with no tomorrow data. The
  `allowedAhead = 2` hack in `advanceDisplayOffset()` is gone, so the
  user can no longer scroll into "24:00 / 25:00" and wrap back to 00:00.
- **Fix 4 (bonus) – Auto-return timer** now resets on every click, so
  scrolling the secondary status page no longer jumps back to the primary
  view mid-read.

See [`VERSION.md`](./VERSION.md) for the highlights and
[`CHANGELOG.md`](./CHANGELOG.md) for full implementation details.

---

### v7.1 - Negative Price Provider Fee (2026-04-06)

Added a separate configurable provider fee for negative spot prices, correctly
modelling contracts where the provider's fee structure differs between positive
and negative market prices.

**New constant:**
```cpp
const float NEG_PRICE_COMPANY_FEE_PERCENTAGE = 30.0;
```

**Price calculation is now:**

| Market price | Formula |
|---|---|
| Positive (`raw >= 0`) | `raw × (1 + POWER_COMPANY_FEE_PERCENTAGE/100) × (1 + VAT_PERCENTAGE/100)` |
| Negative (`raw < 0`) | `raw × (1 - NEG_PRICE_COMPANY_FEE_PERCENTAGE/100) × (1 + VAT_PERCENTAGE/100)` |

The switch happens on the **raw API price** before any multiplier is applied.
VAT is applied to both cases, consistent with net billing contracts where VAT
is calculated on the monthly net sum (mathematically equivalent due to VAT
being a linear multiplier).

**Key values for `NEG_PRICE_COMPANY_FEE_PERCENTAGE`:**

| Value | Meaning |
|---|---|
| `30.0` | Provider keeps 30%, pays you 70% of the negative market price |
| `0.0` | Provider passes the full negative price to you (no fee deducted) |

All 5 fee calculation sites updated: `updateLeds()`, `format15MinPrice()`,
`displayPriceRow()`, `displaySecondaryList()` (daily average), and version strings.

---

### v7.0 - Rolling 48-Hour Logic & Midnight Bridge (Major Upgrade)

This version is the **"Golden Build"** for this hardware platform. It combines all hardware stability fixes from v6.2.4 with a revolutionary new 48-hour price prediction system.

#### Key New Features

- **Midnight Bridge**: Instantly promotes pre-fetched tomorrow's data to become today's data at midnight, eliminating the "1 AM fetch gap"
- **Dual-Buffer NVS**: Stores "Today" and "Tomorrow" data independently
- **47-Hour Scrolling**: View up to 47 hours of price data when tomorrow's data is available
- **Visual Tomorrow Indicators**: Future hours are marked with `HH:>>` format
- **Smart Fetching**: Automatically fetches tomorrow's data after 14:00 (2 PM)

#### Technical Highlights

- **Instant Midnight Transition**: No more "No Data" screen at midnight
- **Power-Failure Resilience**: New day's data is saved to NVS immediately after midnight swap
- **Correct Min/Max for Tomorrow**: Price indicators correctly reference tomorrow's statistics
- **LED Indicators Pinned to Current**: White LED always reflects actual current prices

### v6.2.4 - Exact-boundary display refresh bug (critical fix of v6.2.3 update)
- Problem: At the exact top of the hour (e.g., 20:00:00), the display automatically refreshed but showed the PREVIOUS hour's data (19:00). This happened because the "next-boundary" rounding logic in findCurrentPriceIndex() pre-calculated the next boundary.
- Fix: Simplified findCurrentPriceIndex() to use a robust "last entry <= now" comparison. This ensures the display transitions to the new hour instantaneously at XX:00:00.

### v6.2.3 - State-based display refresh logic (critical fix of v6.2.2 update)
- Problem: Screen would occasionally fail to update if the ESP32 was busy (fetching data or reconnecting WiFi) during the exact 00/15/30/45 minute mark.
- Fix: Switched from "Event-Based" (refresh only AT minute X) to "State-Based" (refresh IF current time != last refresh time). This ensures the screen updates immediately even if the device was busy during the transition.

### v6.2.2 - Display Blank Lines Issue Fix (critical fix of v6.2.1 update)
  - Problem: Sometimes rows 0 and 1 (current 15-min prices and current hour) were blank.
  - Cause: The "hour suppression" logic was hiding the current hour unexpectedly.
  - Fix:
    - Row 1 (current hour) now ALWAYS shows - suppression logic only applies to rows 2-3.
    - Row 0 (15-min details) also always shows for the current hour.

### v6.2.1 – Current Interval Fix (critical fix of v6.2.0 update)
- Fixed display showing prices one hour ahead of the current time.
- `findCurrentPriceIndex()` now correctly returns the current 15-minute interval.

Major update sketch implements **Version 6.2.0**, focusing on:

- **Version 6.2.0 FIX**: **DST (Daylight Saving Time) handling fully fixed** – the ticker now works correctly on ALL days including DST switch days (spring forward and fall back). Uses timestamp-based lookups throughout.
- Version 6.1.2 fix: restore correct **white LED indicator** behavior (ESP32 PWM fix; no dim glow when off).
- Version 6.1.1 fix: Correct daily **low/high hourly markers** (now includes negative and **0.0** prices).
- Daily (not hourly) API fetching.
- Robust **NVS storage** of daily price data.
- Correct **CET/CEST** handling.
- Resilient **after‑midnight refresh** (no more getting stuck on "No data for today").
- Preserved UI and button behavior from v5.5.

---

## DST (Daylight Saving Time) – How It Works

### v6.2.0+: Fully DST-Safe

**Important**: Starting with v6.2.0, the ticker is **fully DST-safe** and requires **no manual intervention** on DST switch days.

The firmware uses **timestamp-based price lookups** that work correctly regardless of whether the day has 23, 24, or 25 hours:

| Day Type | Hours in Day | Price Entries | Status |
|----------|-------------|---------------|--------|
| Normal | 24 | 96 | Works |
| Spring forward (March) | 23 | 92 | Works (fixed in v6.2.0) |
| Fall back (October) | 25 | 100 | Works (fixed in v6.2.0) |

### Timezone Configuration

The firmware uses the `TZ_CET_CEST` timezone string for displaying local time:

```cpp
const char* TZ_CET_CEST = "CET-1CEST,M3.5.0/02:00,M10.5.0/03:00";
```

**Current behavior:**
- Spring forward: Last Sunday of March at 02:00 → 03:00 (CEST, UTC+2)
- Fall back: Last Sunday of October at 03:00 → 02:00 (CET, UTC+1)

### Future-Proof: If EU Cancels DST

If the EU parliament ever cancels DST switching, you only need to update **one line of code**:

```cpp
// Option A - Stay on CET (UTC+1, winter time) permanently:
const char* TZ_CET_CEST = "CET-1";

// Option B - Stay on CEST (UTC+2, summer time) permanently:
const char* TZ_CET_CEST = "CEST-2";
```

The rest of the code works unchanged because it uses timestamp-based lookups.

---

## The Midnight Bridge (v7.0)

### The Problem with Traditional Tickers

Most electricity tickers fail at midnight because they rely on slow API calls to fetch new data. The Energy-Charts API typically doesn't publish next-day data until 1-2 AM, leaving users with a "No Data" screen for hours.

### The Solution: Midnight Bridge

The Midnight Bridge detects the moment the local clock moves from 23:59:59 to 00:00:00 and instantly promotes the pre-fetched "Tomorrow" data to become "Today" data.

**How it works:**

1. **Pre-fetching**: After 14:00 (2 PM), the ticker automatically fetches tomorrow's prices using the `&start=YYYY-MM-DD` API parameter
2. **Buffer Storage**: Tomorrow's data is stored in a separate NVS slot (`data_prc_t`)
3. **Midnight Detection**: The main loop detects day rollover by comparing `tm_mday`
4. **Instant Swap**: At 00:00:00, tomorrow's buffer instantly becomes today's data
5. **NVS Persistence**: New day's data is saved immediately after swap (power-failure protection)

### Power-Failure Protection

Immediately after the midnight swap, the new "Today" data is serialized and saved to NVS. If power is cut at 00:05 AM, the device reboots with correct data already loaded.

---

## Dual-Buffer System (v7.0)

The v7.0 firmware implements a dual-buffer system that stores today and tomorrow data independently:

### Buffer Comparison

| Buffer | Variable | NVS Keys | Contents |
|--------|----------|----------|----------|
| Today | `doc` | `data_prc`, `data_day`, `data_mon`, `data_year` | Current day's prices |
| Tomorrow | `docTomorrow` | `data_prc_t`, `data_store_t` | Next day's prices |

### Statistics Per Buffer

Each buffer maintains its own statistics:
- **Daily average**: `averagePrice` / `averagePriceTomorrow`
- **Lowest price index**: `lowestPriceIndex` / `lowestPriceIndexTomorrow`
- **Highest price index**: `highestPriceIndex` / `highestPriceIndexTomorrow`

### Display Selection

The display logic automatically selects the correct buffer based on the time offset:

```cpp
bool showTomorrow = (totalHourOffset >= 24);
StaticJsonDocument<Config::JSON_BUFFER_SIZE>& targetDoc = showTomorrow ? docTomorrow : doc;
int lowIdx = showTomorrow ? lowestPriceIndexTomorrow : lowestPriceIndex;
```

---

## 48-Hour Scrolling (v7.0)

### Extended Range

When tomorrow's data is available, users can scroll up to **47 hours ahead**:

```cpp
int maxOffsetLimit = isTomorrowDataAvailable ? 47 : 23;
```

### Visual Tomorrow Indication

Future hours (tomorrow) are displayed with `HH:>>` format to clearly distinguish them from today's hours:

```
Today's hour:  14:00 | Tomorrow's hour: 14:>>
```

### Correct Min/Max Indicators

The low/high price markers (arrows) correctly reference tomorrow's statistics when viewing tomorrow's hours:

```cpp
if (dataIndex == lowIdx) {
    lcd.write(byte(3)); // Low price arrow
}
```

---

## Behavior & Display States (v7.5)

The display changes based on which data buffer is being used and the status of the fetch:

| **State** | **Display Output** | **LED Behavior** |
|----------|-------------------|-----------------|
| **Normal (Today)** | Shows current prices and 15-min details. Hours are marked as HH:00. | White LED reflects current price status (Breathe, Solid, or Blink). |
| **Scrolling (Tomorrow)** | Future prices are displayed. Hours are marked with HH:>> to indicate "Tomorrow". | **Pinned to Today:** The LEDs continue showing the _actual current_ price status even while browsing future hours. |
| **No Data** | Displays: "No data for today, Press & hold to, refresh manually." | White LED is turned **OFF** to avoid misleading price signals. |
| **Connecting** | "Elec. Rate SI v7.5" followed by "Connecting..." and progress dots. | Built-in LED is **OFF** until connection is established. |

### Key UX Principle: LEDs Stay Pinned to Current Time

Unlike the display which can scroll through future hours, the white LED **always** reflects the actual current price status. This means:
- Even while browsing tomorrow's cheap hours, the LED tells you the **true current** price situation
- This prevents confusion and helps you decide "should I turn on the dishwasher **now**?"

---

## API Call Intervals & Retry Strategy (v7.5)

### Primary Scheduling (Daily Fetch)

The device aims to maintain a rolling 48-hour data window by fetching today's and tomorrow's data at specific times:

- **Initial Boot:** An API call is attempted immediately upon startup and time synchronization.
- **Today's Data:** Requested with explicit `&start=YYYY-MM-DD&end=YYYY-MM-DD` bounds for the current local date (v7.5 Fix B), so the returned window never depends on server-side caching of the bare endpoint.
- **Tomorrow's Data (Smart Fetching):** Starting at **14:00 (2 PM)** and ending at **23:00 local time**, the device checks for the next day's prices every **30 minutes** (max 19 attempts per day). After 23:00 no further tomorrow-fetch HTTP calls are issued; the Midnight Bridge handles the 00:00 rollover. This bounded window prevents excessive API calls that could trigger rate-limiting or IP bans.

- **Midnight Rollover:** At exactly **00:00:00**, the device "promotes" tomorrow's data to the today buffer. If tomorrow's data was already successfully fetched and stored, **no API call is needed at midnight**.

### Retry Logic (Exponential Backoff)

If a scheduled API call fails (e.g., due to a temporary server error or WiFi glitch), the device uses a safety-oriented retry interval:

- **Max Retries:** 5 attempts (`HTTP_GET_RETRY_MAX = 5`)
- **Backoff Factor:** 2 (`HTTP_GET_BACKOFF_FACTOR = 2`)
- **Typical Progression:** After a failure, it waits a short period, then doubles that wait time for each subsequent failure until the maximum retry count is reached

### "Midnight Phase" Recovery

If the device reaches midnight but **does not** have tomorrow's data ready (meaning the afternoon fetches failed), it enters a high-priority state called `midnightPhaseActive`:

- **Behavior:** Bypasses the standard daily schedule and retries the API **more aggressively**
- **Initial Interval:** Attempts every minute until successful
- **Goal:** Clear the "No Data" screen and restore the price display as quickly as possible once the energy provider's server updates

### Background Monitoring

While not making API calls constantly, the device performs these checks continuously:

- **Loop Pacing:** The main system loop runs every **100ms** to check if it's time for a scheduled fetch
- **Display Refresh:** The screen logic checks the time every loop but only refreshes the UI every **15 minutes** (at :00, :15, :30, :45) to match the price data intervals

---

## Bidding Zones (BZN) / Region Selection

The firmware currently uses:

```text
https://api.energy-charts.info/price?bzn=SI
```

Where `bzn` is the **bidding zone** code. You can change this in the `.ino`:

```cpp
const char* api_url = "https://api.energy-charts.info/price?bzn=SI";
```

to any supported BZN.

> **Note (v7.5 Fix B):** `api_url` is the *base* URL. The firmware appends
> `&start=YYYY-MM-DD` (tomorrow's fetch) or `&start=YYYY-MM-DD&end=YYYY-MM-DD`
> (today's fetch) at request time. The Energy-Charts API treats `end` as
> **inclusive** and interprets both dates in **local exchange time**, so
> `start == end` returns exactly one market day — 96 entries normally,
> 92 on a spring-forward day and 100 on a fall-back day.

All available bidding zones:

- `AT` ‑ Austria
- `BE` ‑ Belgium
- `BG` ‑ Bulgaria
- `CH` ‑ Switzerland
- `CZ` ‑ Czech Republic
- `DE-LU` ‑ Germany, Luxembourg
- `DE-AT-LU` ‑ Germany, Austria, Luxembourg
- `DK1` ‑ Denmark 1
- `DK2` ‑ Denmark 2
- `EE` ‑ Estonia
- `ES` ‑ Spain
- `FI` ‑ Finland
- `FR` ‑ France
- `GR` ‑ Greece
- `HR` ‑ Croatia
- `HU` ‑ Hungary
- `IT-Calabria` ‑ Italy Calabria
- `IT-Centre-North` ‑ Italy Centre North
- `IT-Centre-South` ‑ Italy Centre South
- `IT-North` ‑ Italy North
- `IT-SACOAC` ‑ Italy Sardinia Corsica AC
- `IT-SACODC` ‑ Italy Sardinia Corsica DC
- `IT-Sardinia` ‑ Italy Sardinia
- `IT-Sicily` ‑ Italy Sicily
- `IT-South` ‑ Italy South
- `LT` ‑ Lithuania
- `LV` ‑ Latvia
- `ME` ‑ Montenegro
- `NL` ‑ Netherlands
- `NO1` ‑ Norway 1
- `NO2` ‑ Norway 2
- `NO2NSL` ‑ Norway North Sea Link
- `NO3` ‑ Norway 3
- `NO4` ‑ Norway 4
- `NO5` ‑ Norway 5
- `PL` ‑ Poland
- `PT` ‑ Portugal
- `RO` ‑ Romania
- `RS` ‑ Serbia
- `SE1` ‑ Sweden 1
- `SE2` ‑ Sweden 2
- `SE3` ‑ Sweden 3
- `SE4` ‑ Sweden 4
- `SI` ‑ Slovenia
- `SK` ‑ Slovakia

> Always verify up‑to‑date BZN support in the Energy‑Charts API docs.

---

## Hardware Setup (Detailed)

This section merges the original v5.5 instructions with the current v7.5 hardware expectations.
Follow it carefully to reproduce the working setup.

### 1. Microcontroller

- **Seeed XIAO ESP32‑C3**

Typical pins used in the sketch:

- `GPIO 5`  → white LED (`whiteLedPin`)
- `GPIO 21` → built‑in LED (`builtinLedPin`)
- `GPIO 4`  → user button / touch input (`buttonPin`)
- `GPIO 9`  → presence sensor (`presencePin`)
- I²C pins  → board‑default SDA/SCL (check XIAO ESP32‑C3 pinout)

---

### 2. 20x4 I²C LCD (2004) – PCF8574 Backpack

- LCD: **20x4 2004 character display** with I²C backpack (PCF8574 or compatible).
- Default I²C address (in code): `0x27`
  (Change in the sketch if your module differs: `LiquidCrystal_I2C lcd(0x27, 20, 4);`)

**Connections:**

| LCD Backpack | XIAO ESP32‑C3 |
|-------------|---------------|
| VCC | 5V |
| GND | GND |
| SDA | I²C SDA |
| SCL | I²C SCL |

> Note: On many XIAO ESP32‑C3 board definitions, SDA/SCL are mapped internally. Just use the default I²C pins as documented by Seeed.

---

### 3. Pushbutton (Default) / Capacitive Touch Alternative

The firmware assumes a **momentary pushbutton** on `GPIO 4` by default.

#### Mechanical Pushbutton (default config)

- One leg → `GPIO 4`
- Other leg → `GND`
- No external pull‑up is required; code uses:

```cpp
pinMode(buttonPin, INPUT_PULLUP);
```

And reads the button as **active‑LOW**:

```cpp
int reading = !digitalRead(buttonPin);
```

So:

- Button **pressed** ⇒ `reading == 1`
- Button **released** ⇒ `reading == 0`

#### Alternative: TTP223 Capacitive Touch Button

If you prefer a TTP223 capacitive touch input instead of a mechanical button:

**Wiring:**

- `VCC` → **3.3V**
- `GND` → **GND**
- `OUT` → `GPIO 4` (same as the pushbutton pin)

**Logic:**

- TTP223 output is **HIGH when touched**.

If you use TTP223, you may want to **remove the logical inversion** in the code:

```cpp
// For mechanical button (active LOW):
int reading = !digitalRead(buttonPin);

// For TTP223 (active HIGH), change to:
int reading = digitalRead(buttonPin);
```

Everything else (debounce, long‑press, double‑click) remains compatible.

---

### 4. Presence Sensor (RCWL‑0516, optional but supported)

The presence sensor is used to control LCD backlight and LEDs to save power and avoid annoying blinking when nobody is around.

Recommended module: **RCWL‑0516** microwave motion sensor.

**Wiring:**

| RCWL‑0516 | Connection |
|-----------|------------|
| VCC | 3.3V |
| GND | GND |
| OUT | GPIO 9 (`presencePin`) |
| **Required**: 10kΩ pull‑down | Between GPIO 9 and GND |

Characteristics:

- The module can be hidden behind non‑metallic surfaces.
- Firmware automatically detects if the presence sensor is connected at boot:
  - If **detected**:
    - Presence toggles backlight on and enables LED output.
    - Absence for `backlightOffDelay` (default 30 s) turns the backlight off and disables LED output.
  - If **not detected**:
    - Backlight is kept on permanently.
    - LEDs are allowed to operate normally.

---

### 5. White LED / LED Strip Output

The sketch uses a **white LED** (or LED strip control line) on `GPIO 5` (`whiteLedPin`).

**Basic single LED wiring:**

- `GPIO 5` → series resistor (e.g. 220–470 Ω) → LED anode
- LED cathode → GND

**For LED strips or higher currents:**

- Use a suitable NPN transistor / MOSFET:

  - GPIO 5 → gate/base (with proper gate/base resistor)
  - LED strip or load → external supply (with common GND)
  - Transistor sink/source → GND / load as per standard MOSFET wiring

- Ensure the **strip power supply shares ground** with the ESP32‑C3 board.
- Do **not** drive large loads directly from the GPIO pin.

The LED is driven with various patterns to indicate price level; see "LED Price Signalling" below.

---

### 6. Power

- XIAO ESP32‑C3:
  - Via USB‑C (recommended for development).
  - Or via 5V pin if you have a regulated 5V supply (check Seeed docs).
- Ensure **all modules** (LCD, presence sensor, LED driver) share a **common ground** with the XIAO.

---

## Firmware Features (v7.5)

### Core Display & Pricing

- Data source: `https://api.energy-charts.info/price?bzn=SI`
- Resolution: 15‑minute intervals with hourly averages
- Display shows up to **47 hours** of price data (when tomorrow's data is available)
  - **Row 0**: Current hour, four 15‑minute values
  - **Rows 1–3**: Current hour + next two hours as hourly averages
- **v7.0 Feature**: Tomorrow's hours are marked with `HH:>>` format
- Price calculation: Raw MWh → EUR/kWh with configurable surcharges
  - Three configurable constants:
    - `POWER_COMPANY_FEE_PERCENTAGE` (default `12.0` %) — fee for **positive** spot prices
    - `NEG_PRICE_COMPANY_FEE_PERCENTAGE` (default `30.0` %) — fee kept by provider on **negative** spot prices
    - `VAT_PERCENTAGE` (default `22.0` %)
  - **v7.1**: Positive and negative spot prices use independent fee multipliers:

    | Market price | Formula |
    |---|---|
    | Positive (`raw >= 0`) | `raw × (1 + POWER_COMPANY_FEE_PERCENTAGE/100) × (1 + VAT_PERCENTAGE/100)` |
    | Negative (`raw < 0`) | `raw × (1 - NEG_PRICE_COMPANY_FEE_PERCENTAGE/100) × (1 + VAT_PERCENTAGE/100)` |

- LCD:
  - `LiquidCrystal_I2C` with custom characters for:
    - Local language letters.
    - Low‑price and high‑price indicators.
- **Daily min/max markers**:
  - The low/high hourly indicators consider **negative**, **0.0**, and positive prices
  - **v7.0 Feature**: Tomorrow's min/max indices are tracked separately and displayed correctly

### LED Price Signalling

The white LED (GPIO 5) reflects the **current 15‑minute interval** price (regardless of what's displayed on screen):

| Price Level | LED Behavior |
|-------------|--------------|
| Negative / no data | LED off |
| ≤ 0.05 EUR/kWh | Smooth breathing |
| 0.05 – 0.15 | Steady on |
| 0.15 – 0.25 | Slow blink |
| 0.25 – 0.35 | Fast blink |
| 0.35 – 0.50 | Double blink |
| > 0.50 | Triple blink pattern |

**Important implementation note (from v6.1.2 on):**

- On ESP32, avoid mixing PWM (`analogWrite`) and `digitalWrite` on the same LED pin.
- The firmware now uses `analogWrite(pin, 0/255)` consistently to guarantee the LED is fully off when gated off.

LED is **disabled** when:

- No data for today.
- Time is not synced.
- Presence sensor has timed out (no presence, if installed).

### Presence Sensor & Backlight

- If presence sensor is **connected**:
  - Presence detected → LCD backlight on, LEDs enabled.
  - No presence for `backlightOffDelay` (30 s by default) → LCD backlight off, LEDs disabled.
- If **no presence sensor** is detected at boot:
  - LCD backlight is always on.
  - LEDs are not gated by presence.

### Button Behavior

One button (or touch) on GPIO 4 controls the UI:

- **Single short press**:
  - On primary screen: scrolls the time offset (future hours up to 47h in v7.0).
  - On secondary screen: scrolls through the 20‑line status text (4 lines at a time).
- **Double press**:
  - Toggles between:
    - Primary price view.
    - Secondary status/info view.
- **Long press (~3 seconds)**:
  - While held:
    - LCD shows: "Long press detected! Release to refresh".
  - On release:
    - Forces a **manual data refresh**:
      - Sets `nextScheduledFetchTime = now`.
      - Shows "Manual Refresh… Please wait…".
      - `handleDataFetching()` will perform an immediate API fetch outside the normal schedule.

An **auto‑scroll timeout** resets the view to "current hour / top of lists" after inactivity.

**v7.4 note:** A CHANGE interrupt on buttonPin captures presses that occur during blocking HTTP fetches, so a click is never silently lost. The long-press handler now requires a genuine press duration ≥ 3 s before triggering a forced refresh, preventing spurious idle-time triggers.

---

## NVS Storage (v7.0: Enhanced with Dual Buffers)

This firmware uses ESP32‑C3 **Preferences API** (`Preferences`) under namespace `"my-ticker"`.

### Stored Keys (v7.0)

**Wi‑Fi credentials:**
- `ssid`
- `pass`

**Today's price data:**
- `data_day`   – calendar day (1–31)
- `data_mon`   – month (0–11)
- `data_year`  – full year (e.g. 2026)
- `data_prc`   – full raw JSON payload from the API
- `data_last_store` – Unix time (`time_t`) when data was last written

**Tomorrow's price data (v7.0 new):**
- `data_prc_t` – full raw JSON payload for next day
- `data_store_t` – Unix time when tomorrow's data was stored

### On Boot

After successful NTP time sync:

1. Attempt to load `data_day`, `data_mon`, `data_year`, and `data_prc` from NVS.
2. If **stored date matches current local date**:
   - Deserialize `data_prc` into `StaticJsonDocument doc`.
   - Run `processJsonData(false)` as if it were fresh from the API.
   - Set `isTodayDataAvailable = true`.
   - **Skip** the initial API call to save traffic.
3. Attempt to load tomorrow's data from `data_prc_t`.
4. If the stored date does **not** match today or JSON parsing fails:
   - NVS data is **ignored** for display.
   - System starts from "No data for today".
   - Schedules an immediate API fetch.

### After Each Successful Fetch

- Today's data: Raw JSON payload is stored into NVS as `data_prc`, along with date and `data_last_store`.
- Tomorrow's data (v7.0): After 14:00, tomorrow's payload is stored as `data_prc_t` with `data_store_t`.

### Midnight Bridge NVS Update (v7.0)

At midnight rollover:
1. Tomorrow's buffer is swapped to become today's buffer
2. New "today" data is immediately serialized and saved to NVS
3. Tomorrow's NVS slot is cleared

This ensures power-failure resilience: if power is lost immediately after midnight, the device boots with valid data.

---

## Daily Fetch Strategy (v7.0: Enhanced with Smart Tomorrow Fetching)

### Goals

- **Avoid hourly polling** of the API.
- Fetch:
  - Once after boot (if no valid NVS data for today).
  - Once per **new day** (after midnight), with robust retries while the next‑day dataset is not yet published.
  - **NEW in v7.0**: Tomorrow's data automatically after 14:00 local time.

### Time Sync & First Fetch

- `configTzTime(TZ_CET_CEST, "pool.ntp.org")` is used to enable CET/CEST aware `localtime_r()` and `getLocalTime()`.
- Until time sync completes, the UI only shows "Syncing Time… Please wait…".
- On first successful sync:
  - `isTimeSynced = true`.
  - `trackedDay` is set to the current `tm_mday`.
  - Either NVS is used (if it has today's data) or an initial fetch is scheduled.

### Day‑Rollover Detection (v7.0: Enhanced with Midnight Bridge)

In the main `loop()`:

- `trackedDay` holds the last seen local day.
- Each iteration:
  - Get `localtime_r()` for `now`.
  - If `tm_mday != trackedDay`:
    - Day rollover detected (midnight).
    - `trackedDay` updated.
    - **v7.0 Midnight Bridge Logic**:
      - If tomorrow's data is available:
        - Instantly swap `docTomorrow` to `doc`
        - Update all statistics (`averagePrice`, `lowestPriceIndex`, etc.)
        - Save to NVS and clear tomorrow slot
        - Reset `timeOffsetHours` to 0
      - If tomorrow's data is NOT available:
        - Enter "No Data" mode
        - Start midnight retry phase

### Smart Tomorrow Fetching (v7.0)

After 14:00 local time, if tomorrow's data is not yet available:

```cpp
// v7.4: window capped to 14:00–23:00 (see Fix B in CHANGELOG)
// v7.5 Fix C: localtime_r() into a caller-owned buffer, and the return
// value is tested — v7.4 dereferenced localtime() here with no NULL check
struct tm ti;
bool tomorrowWindow = localtime_r(&now, &ti) &&
                      ti.tm_hour >= 14 && ti.tm_hour <= 23;
if (tomorrowWindow && !isTomorrowDataAvailable) {
    fetchAndProcessData(true); // Fetch tomorrow's data
}
```

The API URL is constructed with the `&start=YYYY-MM-DD` parameter for the next
day. Today's fetch uses `&start=YYYY-MM-DD&end=YYYY-MM-DD` for the current local
date (v7.5 Fix B); both are built from `localtime_r()` + a midday-anchored
calendar-day increment with `tm_isdst = -1` set before `mktime()`, so the date
is correct on DST transition days.

**v7.4 scheduling guarantee:** Every failed or rejected tomorrow fetch now advances nextScheduledFetchTime by 1800 s (30 min), preventing a tight retry loop that would block the main loop and starve handleButton(). Additionally, handleDataFetching() contains a belt-and-suspenders guard that forces the schedule forward if the helper somehow fails to advance it.

### "Today" Detection (Market Day Logic)

The Energy‑Charts API can keep serving **yesterday's** market day for some time after local midnight.
To avoid accidentally accepting yesterday's data as today's, v6.1+ uses a more robust rule.

In `processJsonData()`:

1. Read `unix_seconds[]`.
2. Interpret the **LAST** timestamp as representing the end of the dataset's market day.
3. Convert it to local time (`localtime_r()` — v7.5 Fix C; v7.4 used the shared-static `localtime()`).
4. Compare its date (day, month, year) to the current local date (or tomorrow's date if `isTomorrow` is true).
   - If they **match**:
     - Dataset is accepted as valid.
     - Statistics are updated for the appropriate buffer.
   - If they **do not match**:
     - Dataset is rejected.
     - Appropriate availability flag is set to false.

---

## Secondary Status Screen (Debug / Info)

A **secondary screen** (toggled via **double‑click**) provides 20 lines of status information, displayed 4 lines at a time:

Typical content (updated for v7.5):

1. Current date and time (`HH:MM  DD.MM.YYYY`)
2. Separator line (`--------------------`)
3. "Zadnja posodobitev:" (Last update header)
4. Last successful fetch (for today) date & time
5. Blank
6. "Dnevno povprečje:" (Daily average)
7. Daily average price in EUR/kWh (with surcharges) or "Cene niso na voljo."
8. Blank
9. Wi‑Fi status and RSSI
10. Local IP address
11. API success rate (`API: xx% (succ/fail)`)
12. Device uptime in days, hours, minutes
13–16. **NVS status block**:
    - `NVS status:`
    - `Data day: DD.MM.YYYY` or `Data day: none`
    - `Last save: DD.MM.YY` or `Last save: none`
    - `NVS: Today+Tomorrow` / `NVS: Today only` / `NVS: Empty/Old`
17–20. Credits and version:
    - `energy-charts.info`
    - `dynamic electricity`
    - `price ticker v7.5`
    - `by Legolas-2025`

---

## Wi‑Fi Provisioning

If NVS does not contain valid Wi‑Fi credentials, or if connecting fails repeatedly:

1. The device starts an **Access Point** with SSID:

   ```text
   MyTicker_Setup
   ```

2. LCD shows "No Wi‑Fi access! Setup Wi‑Fi: SSID: MyTicker_Setup" and the AP IP.
3. A simple captive portal is served:
   - Open any URL while connected to `MyTicker_Setup`.
   - Enter SSID and password in the HTML form.
   - Values are stored in NVS: `ssid`, `pass`.
   - Device reboots and attempts to connect with the new credentials.

---

## Building & Uploading

1. Install **Arduino IDE** with ESP32 board support (including XIAO ESP32‑C3).
2. Install required libraries:
   - `LiquidCrystal_I2C`
   - `ArduinoJson`
   - `DNSServer` (from ESP32 core)
   - `WebServer` (from ESP32 core)
   - `Preferences` (built‑in for ESP32)
3. Open the v7.5 `.ino` file (`ESP32_standalone_electricity_ticker_7_5.ino`).
4. In Tools:
   - Board: `Seeed XIAO ESP32C3`
   - Port: choose the correct serial port.
5. Upload the sketch.
6. Open Serial Monitor at **115200 baud** to see:
   - Wi‑Fi connection logs.
   - NTP sync messages.
   - NVS load/save status.
   - Midnight rollover and retry debug output.
   - Tomorrow fetch logs (`Fetching Tomorrow's Data...`)

---

## Known Issues / Not Yet Fixed (v7.5)

Two issues are known and deliberately **not** fixed in v7.5. Both are
**intermittent**, both are **pre-existing** (v7.4 behaved identically), and
neither has a code fix in this release — this section is documentation only.

### 1. Brief UI freeze during a double-click

On a double-click (primary ↔ secondary screen) the indicator LED stops blinking
for a second or several, and the screen switch only completes when the freeze
ends.

The button logic is **not** at fault: `toggleList()` only flips `currentList`
and redraws the LCD. The freeze is the blocking `http.GET()` inside
`handleDataFetching()` — while the loop is parked in that call, `handleButton()`
and `updateLeds()` never run, so the LED simply holds its last PWM value and
button events queue in the ISR.

`Config::HTTP_TIMEOUT` (10 s) and `HTTP_CONNECT_TIMEOUT` (5 s) bound TCP connect
and socket reads but **not** DNS resolution, which ESP32 lwIP performs
synchronously. A multi-second stall is therefore consistent with a DNS lookup,
and raising those two constants would not help.

Why it is intermittent: in normal operation a fetch is due roughly every 30
minutes, so the exposed window is only the request's own duration — a fraction
of a percent of the time. It becomes noticeably more likely in a degraded state
(today's data missing, or the API returning the wrong day), where the retry
cadence drops to 10 minutes.

**To confirm it,** watch the Serial Monitor at 115200 baud. A freeze is
preceded by:

```text
HTTP GET TODAY rc=200 took 4000 ms
  ^ slow request: loop was blocked for 4000 ms (button edges queued in ISR, applied after)
```

The 10-second auto-return to the primary screen (`autoScrollTimeout` →
`resetDisplayToTop()`) does no network work, so it never freezes.

### 2. A double-click can collapse to a single click during a freeze

If both presses of a double-click land inside one blocking fetch, the screen
scrolls instead of switching lists. `buttonInterruptFired` is a `volatile bool`
(line 501) — it records *that* an edge occurred, not *how many* — so when the
loop resumes, `handleButton()` synthesises exactly **one** press+release pair
and two real presses are reported as one. A double-click straddling the end of
a freeze fails the same way, because the 500 ms `doubleClickWindow` has already
expired by the time the second press is processed.

This is dormant whenever no fetch is in flight. It is recorded here so that a
future "the buttons went flaky again" report is not mistaken for a new
regression. Fixing it — and the freeze above — means counting edges in the ISR
and making the fetch non-blocking. Neither is a small patch, so both are
deferred rather than rushed into this release.

---

## Versioning & Changelog

- **v7.5** – DST hardening + API URL date bounds + fall-back average fix
  (2026-10-07):
  - `tm_isdst = -1` before both `mktime()` calls
  - Explicit `&start=&end=` date bounds on today's fetch
  - All 8 remaining `localtime()` call-sites converted to `localtime_r()`
  - DST-aware hour-block detection in the daily average
  - Forward-progress guard so a rejected fetch cannot re-enter in a tight loop
  - HTTP request duration measured instead of guarded
  - No fetch started within 800 ms of a button edge
  - Two issues remain open and unfixed — see
    [Known Issues / Not Yet Fixed](#known-issues--not-yet-fixed-v75).
  - See highlights above; full details in [`CHANGELOG.md`](./CHANGELOG.md).
- **v7.4** – Fetch scheduling fix: button responsiveness (2026-10-04). 
  See highlights above; full details in [CHANGELOG.md](./CHANGELOG.md).
- **v7.3** – DST edge-case hardening (2026-10-03). See highlights above; full
  details in [`CHANGELOG.md`](./CHANGELOG.md).
- **v7.2** – Button robustness & screen-control fixes (2026-08-04). See
  highlights above; full details in [`CHANGELOG.md`](./CHANGELOG.md).
- **v7.1** – Negative price provider fee:
  - Separate `NEG_PRICE_COMPANY_FEE_PERCENTAGE` constant (default `30.0` %)
  - Positive prices: `raw × (1 + pos_fee) × (1 + VAT)`
  - Negative prices: `raw × (1 - neg_fee) × (1 + VAT)`
  - Switch on raw API price before any multiplier
  - All 5 fee calculation sites updated
- **v7.0** – Rolling 48-Hour Logic & Midnight Bridge:
  - Dual-buffer NVS system for today and tomorrow data
  - Midnight Bridge for seamless day rollover
  - 47-hour scrolling with `HH:>>` visual indicators
  - Smart fetching of tomorrow's data after 14:00
  - Correct min/max indicators for tomorrow's hours
  - Power-failure resilient NVS updates
- **v6.2.4** – Exact-boundary display refresh bug fix
- **v6.2.3** – State-based display refresh logic fix
- **v6.2.2** – Display blank lines issue fix
- **v6.2.1** – Current interval fix
- **v6.2.0** – DST handling fully fixed via timestamp-based lookups
- **v6.1.2** – LED indicator restored (broken in previous version):
  - Avoid mixing PWM and `digitalWrite` on the same LED pin (ESP32 LEDC behavior).
  - Ensures LED is fully off when gated off; patterns operate correctly.
- **v6.1.1** – Daily low/high marker fix:
  - Daily min/max and average now include negative and **0.0** prices.
- **v6.1.0** – Midnight fetch & "today" detection fixes:
  - Correctly detect **market day** using the last `unix_seconds` timestamp.
  - Distinguish between:
    - HTTP/JSON success, but data for **wrong day** (treated as failure).
    - Full success with accepted "today" dataset.
  - Robust midnight retry scheme:
    - Two retries every 20 minutes in the first hour (~00:20, ~00:40).
    - Then hourly retries (top‑of‑hour) until today's dataset is available.
  - Behavior on reboot and manual long‑press is unchanged, but now respects the improved "today" logic.
- **v6.0.0** – NVS storage & daily fetch:
  - Store daily price data in NVS.
  - Reduce API calls to "boot + after‑midnight".
  - Add NVS status section to secondary menu.
- **v5.5** – 15‑minute detail mode, LED based on current 15‑minute slot, improved DST handling (see file `20251027a_electricity_ticker_10_5_5_latest_DST_and_midnight_fix.ino`).

See [`CHANGELOG.md`](./CHANGELOG.md) for more details.

---

## Data attribution

Electricity price data provided by
**[Energy-Charts](https://energy-charts.info)** (Fraunhofer ISE)
via the [Energy-Charts API](https://api.energy-charts.info),
licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).

---

## License

This project is licensed under the MIT License – see the [`LICENSE`](./LICENSE) file for details.
