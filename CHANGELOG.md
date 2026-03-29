# Changelog

All notable changes to this project are documented here.

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
|----------|--------------|
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
