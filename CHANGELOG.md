# Changelog

All notable changes to this project are documented here.

## v7.1 - Negative Price Provider Fee (2026-04-06)

**Summary**

Added a separate configurable provider fee for negative spot prices, correctly
modelling contracts where the provider's fee structure differs between positive
and negative market prices.

### What changed

**New constant (in `// Price computation` globals block):**
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

**All 5 fee calculation sites updated:**

| Function | Purpose |
|---|---|
| `updateLeds()` | LED brightness reflects correct negative price |
| `format15MinPrice()` | 15-min row values on primary display |
| `displayPriceRow()` | Hourly price rows on primary display |
| `displaySecondaryList()` | Daily average on secondary info screen |
| Version strings | `connectToWiFi()` LCD and `displaySecondaryList()` credit line |

---

## v7.0 - Rolling 48-Hour Logic & Midnight Bridge (2026-04-03)

**Summary**

This is the **"Golden Build"** for this hardware platform. It combines all the hardware stability fixes from v6.2.4 with a revolutionary new 48-hour price prediction system that eliminates the "1 AM fetch gap" problem that plagues most electricity tickers.

### New Features

#### 1. Dual-Buffer NVS System

The ticker now stores "Today" and "Tomorrow" data independently in NVS, allowing seamless display of up to 47 hours of price data.

**New NVS Keys:**
- `data_prc_t` – Raw JSON payload for tomorrow's prices
- `data_store_t` – Unix timestamp when tomorrow's data was stored

**New Global Variables:**
- `StaticJsonDocument<Config::JSON_BUFFER_SIZE> docTomorrow` – Tomorrow's price data buffer
- `bool isTomorrowDataAvailable` – Flag indicating tomorrow's data availability
- `float averagePriceTomorrow` – Tomorrow's daily average price
- `int lowestPriceIndexTomorrow` – Index of tomorrow's lowest price hour
- `int highestPriceIndexTomorrow` – Index of tomorrow's highest price hour

#### 2. The Midnight Bridge (Rollover Logic)

**Problem:**
Most electricity tickers fail at midnight because they rely on slow API calls to fetch new data. The Energy-Charts API typically doesn't publish next-day data until 1-2 AM, leaving users with a "No Data" screen for hours.

**Solution:**
The Midnight Bridge detects the moment the local clock moves from 23:59:59 to 00:00:00 and instantly promotes the pre-fetched "Tomorrow" data to become "Today" data.

**Implementation (in `loop()`):**
```cpp
if (daycheck->tm_mday != trackedDay) {
    // Midnight rollover detected
    if (isTomorrowDataAvailable) {
        // Swap tomorrow to today instantly
        doc = docTomorrow;
        docTomorrow.clear();

        // Update all statistics
        averagePrice = averagePriceTomorrow;
        lowestPriceIndex = lowestPriceIndexTomorrow;
        highestPriceIndex = highestPriceIndexTomorrow;

        // Save to NVS and clear tomorrow slot
        serializeJson(doc, payload);
        saveDataToNVS(payload, false);
        clearTomorrowNVS();

        timeOffsetHours = 0;
        displayPrices();
    }
}
```

**NVS Power-Failure Protection:**
Immediately after the swap, the new "Today" data is serialized and saved to NVS. If power is cut at 00:05 AM, the device reboots with correct data already loaded.

#### 3. Smart Fetching & Tomorrow's Data

**Automatic Tomorrow Fetch:**
After 14:00 (2 PM) local time, the ticker automatically fetches tomorrow's data using the `&start=YYYY-MM-DD` parameter:

```cpp
void fetchAndProcessData(bool fetchTomorrow) {
    String url = api_url;
    if (fetchTomorrow) {
        time_t now = time(nullptr);
        now += 24 * 3600; // Add 24 hours
        struct tm* tmr = localtime(&now);
        char dateStr[20];
        snprintf(dateStr, sizeof(dateStr), "%04d-%02d-%02d",
                 tmr->tm_year + 1900, tmr->tm_mon + 1, tmr->tm_mday);
        url += "&start=";
        url += dateStr;
    }
    // ... HTTP request follows
}
```

**Smart Scheduling (`handleDataFetching()`):**
```cpp
struct tm* ti = localtime(&now);
// Priority 1: If it's after 14:00 and we don't have tomorrow's data yet
if (ti->tm_hour >= 14 && !isTomorrowDataAvailable) {
    fetchAndProcessData(true); // Fetch tomorrow
}
```

#### 4. Seamless 48H Scrolling

**Extended Display Range:**
When tomorrow's data is available, users can scroll up to **47 hours ahead**:

```cpp
int maxOffsetLimit = isTomorrowDataAvailable ? 47 : 23;
if (displayStartHourOffset > maxOffsetLimit)
    displayStartHourOffset %= (maxOffsetLimit + 1);
```

**Visual Distinction for Tomorrow:**
Future hours are marked with `HH:>>` to clearly distinguish tomorrow's prices from today's:

```cpp
if (showTomorrow) {
    snprintf(buffer, sizeof(buffer), "%02d:>>", localHourIndex);
} else {
    snprintf(buffer, sizeof(buffer), "%02d:00", localHourIndex);
}
```

**Correct Min/Max Indicators:**
The code correctly uses tomorrow's statistics when displaying future hours:

```cpp
int lowIdx = showTomorrow ? lowestPriceIndexTomorrow : lowestPriceIndex;
int highIdx = showTomorrow ? highestPriceIndexTomorrow : highestPriceIndex;
```

#### 5. Hardware Stability (Preserved from v6.2.4)

All v6.2.4 stability fixes remain intact:

- **State-Based Refresh**: Display updates exactly at 00, 15, 30, and 45 minutes past the hour, even if the CPU is busy
- **LED Indicators Pinned to Current**: White LED and built-in LED reflect actual current prices, regardless of what the user is viewing on screen

### Technical Implementation Details

#### Dual-Buffer Display Helpers

**`display15MinuteDetails(int row, int totalHourOffset)`:**
- Now accepts `totalHourOffset` (0-47) instead of just hour index
- Automatically selects correct buffer (`doc` or `docTomorrow`) based on offset
- Shows past segments ("> ") for current hour

**`displayPriceRow(int row, int totalHourOffset, bool isCurrentHourRow)`:**
- Extended to handle tomorrow's data with visual indicators
- Correctly applies hour suppression logic only to today's hours

#### NVS Persistence Updates

**`saveDataToNVS(const String& rawJson, bool isTomorrow)`:**
```cpp
if (isTomorrow) {
    preferences.putString("data_prc_t", rawJson);
    preferences.putULong("data_store_t", (unsigned long)now);
} else {
    // Original "today" save logic
    preferences.putInt("data_day", timeinfo.tm_mday);
    // ...
}
```

**`loadDataFromNVS()`:**
- Now loads both today and tomorrow data from NVS
- Validates and processes both buffers independently

#### API Date Validation

The `processJsonData()` function now validates data against the correct target date:

```cpp
time_t targetTime = time(nullptr);
if (isTomorrow) targetTime += 24 * 3600;
struct tm* targetDayTm = localtime(&targetTime);

bool sameDate = (lastDataTm->tm_mday == targetDayTm->tm_mday &&
                 lastDataTm->tm_mon == targetDayTm->tm_mon &&
                 lastDataTm->tm_year == targetDayTm->tm_year);
```

### Why This Is the "Golden Build"

| Feature | v6.2.4 | v7.0 |
|---------|--------|------|
| Display hours ahead | 23 hours (today only) | 47 hours (today + tomorrow) |
| Midnight transition | "No Data" until API updates | Seamless swap from buffer |
| Power failure resilience | Relies on API availability | NVS contains valid data |
| Visual tomorrow indication | None | `HH:>>` format |
| Tomorrow min/max markers | N/A | Correct indices |
| Fetch strategy | Once per day | Smart: today + tomorrow after 14:00 |

### User Experience: Behavior & Display States

The display changes based on which data buffer is being used and the status of the fetch:

| **State** | **Display Output** | **LED Behavior** |
|----------|-------------------|-----------------|
| **Normal (Today)** | Shows current prices and 15-min details. Hours are marked as HH:00. | White LED reflects current price status (Breathe, Solid, or Blink). |
| **Scrolling (Tomorrow)** | Future prices are displayed. Hours are marked with HH:>> to indicate "Tomorrow". | **Pinned to Today:** The LEDs continue showing the _actual current_ price status even while you scroll through tomorrow. |
| **No Data** | Displays: "No data for today, Press & hold to, refresh manually." | White LED is turned **OFF** to avoid misleading price signals. |
| **Connecting** | "Elec. Rate SI v7.0" followed by "Connecting..." and progress dots. | Built-in LED is **OFF** until connection is established. |

### API Call Intervals & Retry Strategy (v7.0)

The exact API call intervals in version 7.0 vary depending on the device's state, data availability, and time of day:

#### Primary Scheduling (Daily Fetch)

The device aims to maintain a rolling 48-hour data window by fetching today's and tomorrow's data at specific times:

- **Initial Boot:** An API call is attempted immediately upon startup and time synchronization.
- **Tomorrow's Data (Smart Fetching):** Starting at **14:00 (2 PM) local time**, the device begins checking for the next day's prices. It will attempt to fetch this data periodically until successful.
- **Midnight Rollover:** At exactly **00:00:00**, the device "promotes" tomorrow's data to the today buffer. If tomorrow's data was already successfully fetched and stored, **no API call is needed at midnight**.

#### Retry Logic (Exponential Backoff)

If a scheduled API call fails (e.g., due to a temporary server error or WiFi glitch), the device uses a safety-oriented retry interval:

- **Max Retries:** 5 attempts (`HTTP_GET_RETRY_MAX`)
- **Interval Formula:** Uses a backoff factor of **2** (`HTTP_GET_BACKOFF_FACTOR`)
- **Typical Progression:** After a failure, it waits a short period, then doubles that wait time for each subsequent failure until the maximum retry count is reached

#### "Midnight Phase" Recovery

If the device reaches midnight but **does not** have tomorrow's data ready (meaning the afternoon fetches failed), it enters a high-priority state called `midnightPhaseActive`:

- **Interval:** It bypasses the standard daily schedule and retries the API **more aggressively** (initially every minute).
- **Goal:** To clear the "No Data" screen and restore the price display as quickly as possible once the energy provider's server updates.

#### Background Monitoring

While not a full API call, the device performs these checks constantly:

- **Loop Pacing:** The main system loop runs every **100ms** to check if it's time for a scheduled fetch.
- **Display Refresh:** The screen logic checks the time every loop but only refreshes the UI every **15 minutes** (at :00, :15, :30, :45) to match the price data intervals.

---

## v6.2.4 - Exact-boundary display refresh bug (2026-04-01):

**Summary**

Top of the hour auto display refresh glitch fix where display automatically refreshed but showed the PREVIOUS hour's data.

### Problem:
- At the exact top of the hour (e.g., 20:00:00), the display automatically refreshed but showed the PREVIOUS hour's data (19:00). This happened because the "next-boundary" rounding logic in findCurrentPriceIndex() incorrectly excluded the current interval if the time was exactly on the boundary.

### Solution:
- Simplified findCurrentPriceIndex() to use a robust "last entry <= now" comparison. This ensures the display transitions to the new hour instantaneously at XX:00:00.

---

## v6.2.3 - State-based display refresh logic fix (2026-04-01):

**Summary**

The refresh logic should be "State-Based" rather than "Event-Based." Instead of checking if the minute is zero, it should check if the current hour is different from the last recorded hour.

### Problem: Screen would occasionally fail to update if the ESP32 was busy
   - fetching data or reconnecting WiFi) during the exact 00/15/30/45 minute mark.

### Solution: Switched from "Event-Based" (refresh only AT minute X) to "State-Based"

    (refresh IF current time != last refresh time).
  - This ensures the screen updates immediately even if the device was busy during the transition.

---

## v6.2.2 - Display blank lines issue fix (2026-03-31):

**Summary**

Fixed a bug where the display was showing blank lines

### Problem: Sometimes rows 0 and 1 (current 15-min prices and current hour) were blank

**Cause:** The "hour suppression" logic was hiding the current hour unexpectedly

### Solution:

  - Row 1 (current hour) now ALWAYS shows - suppression logic only applies to rows 2-3
  - Row 0 (15-min details) also always shows for the current hour

---

## v6.2.1 – Current Interval Fix (2026‑03‑29)

**Summary**

Fixed a bug where the display was showing prices one hour ahead of the current time.

### Problem: Display Showing Next Hour Instead of Current

**Root Cause:**

The `findCurrentPriceIndex()` function was finding the **next** 15-minute interval (first entry with timestamp >= now), but it should find the **current** interval (the one we're currently IN).

For example, at 17:57:
- The current 15-minute interval is **17:45-18:00** (price indexed at 17:45)
- The **next** interval is 18:00-18:15 (price indexed at 18:00)
- The buggy function returned the index for **18:00** instead of **17:45**
- Result: Display showed hour **18** instead of hour **17**

### Solution

The fix calculates the **next 15-minute boundary** and finds the last entry **strictly before** that boundary:

```cpp
// Calculate the next 15-minute boundary
const int QUARTER_SECONDS = 15 * 60; // 900 seconds
time_t nextQuarter = ((now + QUARTER_SECONDS - 1) / QUARTER_SECONDS) * QUARTER_SECONDS;

// Find the last entry strictly before nextQuarter
for (size_t i = unixSeconds.size(); i > 0; i--) {
    if ((time_t)unixTime < nextQuarter) {
        return (int)(i - 1);
    }
}
```

**Example:**
- At 17:57: nextQuarter = 18:00, finds last entry < 18:00 = 17:45
- At 18:00: nextQuarter = 18:15, finds last entry < 18:15 = 18:00
- At 18:46: nextQuarter = 19:00, finds last entry < 19:00 = 18:45

---

## v6.2.0 – DST (Daylight Saving Time) Handling Fixed (2026‑03‑29)

**Summary**

This release fixes a critical bug that caused incorrect price display on DST switch days. On March 29, 2026 (the spring forward day), the ticker at 12:41 showed prices for hours 13:00, 14:00, and 15:00 instead of the correct 12:00, 13:00, and 14:00.

### Problem: Arithmetic-Based Index Calculation

**Root Cause (v6.0.0 – v6.1.2):**

The code assumed every day has exactly 96 price entries:

```cpp
int startIndex = hourIndex * 4;  // e.g., hour 12 → index 48
```

This assumption breaks on DST switch days:

| Day Type | Hours | Price Entries | Example |
|----------|-------|---------------|---------|
| Normal day | 24 | 96 | Array indices 0-95 |
| Spring forward (March) | 23 | 92 | Index 48 points to wrong time |
| Fall back (October) | 25 | 100 | Index 48 points to wrong time |

At 12:41 on March 29, 2026:
- ESP32 correctly reported `timeinfo.tm_hour = 12`
- Old code calculated `12 × 4 = 48`
- But array only had 92 entries (no index 48 that maps to local hour 12)
- Result: Displayed prices for 13:00, 14:00, 15:00 instead of 12:00, 13:00, 14:00

### Solution: Timestamp-Based Lookups

**New approach (v6.2.0):**

All price lookups now search the `unix_seconds` array using actual timestamps:

```cpp
// Find index by searching for matching hour in timestamps
int findPriceIndexForHour(const JsonArray& unixSeconds, int targetHour) {
    for (size_t i = 0; i < unixSeconds.size(); i++) {
        unsigned long unixTime = unixSeconds[i].as<unsigned long>();
        time_t t = (time_t)unixTime;
        struct tm* ptm = localtime(&t);
        if (ptm != NULL && ptm->tm_hour == targetHour) {
            return (int)i;  // Found correct index for this hour
        }
    }
    return -1;
}
```

**New functions added:**
- `findPriceIndexForHour()` – Finds first price index for a given hour
- `findCurrentPriceIndex()` – Finds current 15-minute slot using Unix timestamp
- `getHourFromPriceIndex()` – Gets hour from price array index

**Updated functions:**
- `getHourlyAverage()` – Now uses timestamp lookup instead of `hourIndex * 4`
- `display15MinuteDetails()` – Timestamp-based with hour verification in loop
- `displayPriceRow()` – Timestamp-based data index lookup
- `displayPrimaryList()` – Timestamp-based current hour detection
- `updateLeds()` – Timestamp-based current interval lookup

### Why This Is Future-Proof

| Scenario | Code Behavior |
|----------|---------------|
| Normal days (96 entries) | Works as before |
| DST spring forward (92 entries) | Timestamp lookup finds correct indices |
| DST fall back (100 entries) | Timestamp lookup finds correct indices |
| EU cancels DST | Only update `TZ_CET_CEST` string; code works unchanged |

If EU parliament ever cancels DST switching, you only need to update the `TZ_CET_CEST` line (one line of code). The price lookup logic requires no changes.

---

## v6.1.2 – LED Indicator Restored (ESP32 PWM Fix) (2026‑03‑11)

**Summary**

This release fixes a regression introduced in **v6.1.1** where the **white LED price indicator** could remain **dimly lit** even when the LCD backlight turned off, and the intended **blink/breathe patterns** no longer behaved correctly.

### Fixed: White LED Stuck Dim / Patterns Broken (PWM vs Digital)

**Problem (v6.1.1):**

- The sketch uses `analogWrite()` to drive the LED with PWM for breathe/blink patterns.
- However, in some "LED OFF" branches the code used `digitalWrite(LOW)` on the same `whiteLedPin`.
- On ESP32 (LEDC), once PWM is attached to a pin, `digitalWrite(LOW)` may **not fully disable** PWM output.
- Result:
  - LED could remain **faintly on** (dim glow) when LEDs were supposed to be off.
  - Some patterns could appear "stuck" or inconsistent.

**Solution (v6.1.2):**

- LED control is now **PWM‑only** inside `updateLeds()`:
  - Use `analogWrite(whiteLedPin, 0)` instead of `digitalWrite(whiteLedPin, LOW)`.
  - Use `analogWrite(whiteLedPin, 255)` instead of `digitalWrite(whiteLedPin, HIGH)`.
  - Blink/double‑blink toggles now switch between PWM **0** and **255**.
- This ensures the LED is **truly off** whenever LED output is gated off.

---

## v6.1.1 – Daily Min/Max Includes Negative & Zero Prices (2026‑03‑07)

**Summary**

This release fixes a bug where the **daily lowest / highest hourly price marker** ignored negative prices (and also ignored 0.0), which could cause the ticker to incorrectly mark the **lowest positive** price as the daily minimum.

### Fixed: Daily Low/High Marker Ignored Negative & Zero Prices

**Problem (v6.1.0):**

- In `processJsonData()` the daily min/max scan used:
  ```cpp
  if (hourlyAvg > 0) { ... }
  ```
- This had two side effects:
  1. **Negative** hourly averages were completely skipped.
  2. A true price of **0.0** was also skipped (even though 0 can be a valid market price).

**Solution (v6.1.1):**

- The min/max and average scan now:
  - Treats an hour as valid based on **data availability** (having all 4×15‑minute entries), not based on value sign.
  - Includes **all values** (negative, zero, positive).

---

## v6.1.0 – Midnight Fetch & Market Day Fix (2026‑01‑30)

**Summary**

This release fixes a bug where the ticker could remain indefinitely on the **"No data for today"** screen after midnight, even though the API was already returning fresh data.

### Fixed: Stuck on NO_DATA_OFFSET After Midnight

**Problem (v6.0.0):**

- The API can continue to serve **yesterday's market day** for some time after local midnight.
- HTTP + JSON success always incremented `apiSuccessCount`, even if the data was "not for today".
- The scheduler treated such fetches as **successful** and never retried.

**Solution (v6.1.0):**

- `processJsonData()` now determines the "market day" using the **last** `unix_seconds` timestamp.
- A scheduled fetch is only successful if `lastProcessJsonAcceptedToday == true`.
- Midnight retry logic keeps trying until valid "today" data is received.

---

## v6.0.0 – NVS Storage & Daily Fetch (2026‑01‑27)

**Summary**

First major redesign focused on reducing API traffic and improving resilience using non‑volatile storage.

### New

- NVS namespace `"my-ticker"` with Wi‑Fi credentials and daily price data caching.
- Boot behavior: Try to load and validate NVS data; if date matches today → reuse it and **skip** initial API call.
- After‑midnight: NVS is overwritten with each successful new‑day dataset.

---

## v5.x – Earlier Versions

Earlier versions (v5.x and below) had:

- No NVS‑based caching of daily API data.
- More frequent API calls (e.g., hourly refresh pattern).
- Less robust handling of DST and daily boundaries.

For exact details, see older `.ino` files and their header comments in this repository.
