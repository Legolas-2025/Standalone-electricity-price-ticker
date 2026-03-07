# Changelog

All notable changes to this project are documented here.

---

## v6.1.1 – Daily Min/Max Includes Negative & Zero Prices (2026‑03‑07)

**Summary**

This release fixes a bug where the **daily lowest / highest hourly price marker** ignored negative prices (and also ignored 0.0), which could cause the ticker to incorrectly mark the **lowest positive** hour as the daily minimum.

### Fixed: Daily Low/High Marker Ignored Negative & Zero Prices

**Problem (v6.1.0):**

- In `processJsonData()` the daily min/max scan used:

  - `if (hourlyAvg > 0) { ... }`

- This had two side effects:
  1. **Negative** hourly averages were completely skipped.
  2. A true price of **0.0** was also skipped (even though 0 can be a valid market price).

- Additionally, the daily average was computed as `sum / 24.0` even though hours were being skipped from `sum`, making the daily average incorrect whenever any hour was excluded.

**Solution (v6.1.1):**

- The min/max and average scan now:
  - Treats an hour as valid based on **data availability** (having all 4×15‑minute entries), not based on value sign.
  - Includes **all values** (negative, zero, positive) when computing:
    - `lowestPriceIndex`
    - `highestPriceIndex`
    - `averagePrice`
  - Computes `averagePrice` using the number of valid hours (normally 24).

---

## v6.1.0 – Midnight Fetch & Market Day Fix (2026‑01‑30)

**Summary**

This release fixes a bug where the ticker could remain indefinitely on the **“No data for today”** screen after midnight, even though the API was already returning fresh data. It also refines the after‑midnight retry schedule.

### Fixed: Stuck on NO_DATA_OFFSET After Midnight

**Problem (v6.0.0):**

- After midnight, the device:
  - Detected day rollover and entered a “no data” state.
  - Scheduled an immediate API fetch.
- However:
  - The API can continue to serve **yesterday’s market day** for some time after local midnight.
  - The code only checked the **first** `unix_seconds` entry against the current local day.
  - HTTP + JSON success always incremented `apiSuccessCount`, even if `processJsonData()` subsequently decided the dataset was “not for today”.
  - The scheduler treated such fetches as **successful**, pushed `nextScheduledFetchTime` 24 hours into the future, and never retried.
  - Result: the ticker stayed on **NO_DATA_OFFSET** forever, until:
    - A manual long‑press triggered a fresh fetch at a time when the API finally returned recognized “today” data, or
    - The device was rebooted later in the day.

**Solution (v6.1.0):**

1. **Market Day Detection**:
   - `processJsonData()` now determines the “market day” using the **last** `unix_seconds` timestamp from the API’s dataset (assumed to cover one full day in 15‑minute steps).
   - It compares that calendar date (local time) against the current local date.
   - If they differ:
     - The dataset is treated as **“not for today”**.
     - `isTodayDataAvailable = false`.
     - The function returns **false**, and a new flag `lastProcessJsonAcceptedToday` remains `false`.

2. **Logical Failure vs HTTP/JSON Failure**:
   - New global flag:
     - `lastProcessJsonAcceptedToday` – `true` only when `processJsonData()` accepts the dataset as “today’s” data.
   - In `handleDataFetching()`:
     - A scheduled fetch is considered a **real success** only if:
       - HTTP + JSON succeeded, **and**
       - `lastProcessJsonAcceptedToday == true`.
     - In all other cases (including “HTTP 200 + parse OK but data still for yesterday”):
       - The fetch is treated as **failure** for scheduling purposes.
       - If `midnightPhaseActive == true`, `scheduleAfterMidnightFailure()` is invoked to plan a retry.

3. **Midnight Phase Cleanup**:
   - When a fetch finally provides a dataset for today:
     - `isTodayDataAvailable = true`.
     - `lastProcessJsonAcceptedToday = true`.
     - `midnightPhaseActive` is cleared; `midnightRetryCount` reset to 0.
     - `nextScheduledFetchTime` is set to 24 hours ahead.

As a result, the ticker will **keep retrying** after midnight until a correct market‑day dataset appears, instead of giving up after the first HTTP 200.

---

### Changed: After‑Midnight Retry Schedule

In `scheduleAfterMidnightFailure()` the retry strategy when `midnightPhaseActive == true` has been tuned.

**Old behavior (v6.0.0, conceptual):**

- First few failures after midnight:
  - Retried every 10 minutes up to 5 attempts.
- Afterwards:
  - Switched to hourly retries (top of each hour).

**New behavior (v6.1.0 + user configuration):**

- Fast retry phase fully contained within the **first hour after midnight**.
- You configured:

  ```cpp
  if (midnightRetryCount < 2) {
      // Retry every 20 minutes for first 2 attempts (~1 hour window)
      midnightRetryCount++;
      nextScheduledFetchTime = now + 1200; // 20 minutes
      debugPrint(2, "Midnight retry " + String(midnightRetryCount) + "/2 in 20 minutes");
  } else {
      // After that, retry only at top of each hour
      ...
  }
  ```

- Timeline:
  - 00:00 – initial attempt (triggered by day rollover).
  - If dataset is still for previous day:
    - 1st retry at ~00:20.
    - 2nd retry at ~00:40.
  - After that:
    - Retries only at the **top of the next hours** (01:00, 02:00, …),
      until a dataset with market day = today is accepted.

This configuration significantly reduces overnight API load while still ensuring the ticker picks up the new day as soon as the API publishes it.

---

### Other Behavior (Retained From v6.0.0)

- **NVS caching** of daily data:
  - On boot, if NVS contains a dataset whose date matches today’s local date:
    - The JSON is deserialized and processed.
    - A new API call is **skipped**.
  - After each accepted “today” fetch:
    - Raw JSON payload, date, and a timestamp are stored in NVS.
- **Manual long‑press refresh**:
  - Still triggers immediate fetch via `nextScheduledFetchTime = now;`.
  - Now also respects the improved “today” detection; “yesterday’s” data is not accepted as today.
- **CET/CEST time handling**:
  - Unchanged, still uses `TZ_CET_CEST` with `configTzTime`.
- **Secondary menu and NVS status lines**:
  - Retained from v6.0.0; updated only for version string and minor wording.

---

## v6.0.0 – NVS Storage & Daily Fetch (2026‑01‑27)

**Summary**

First major redesign focused on reducing API traffic and improving resilience using non‑volatile storage.

### New

- NVS namespace `"my-ticker"` introduced with keys:
  - `ssid`, `pass` – Wi‑Fi credentials.
  - `data_day`, `data_mon`, `data_year` – stored data calendar day.
  - `data_prc` – raw JSON string from API.
  - `data_last_store` – Unix time when data was stored.
- Boot behavior:
  - Try to load and validate NVS data.
  - If date matches today → reuse it and **skip** initial API call.
- After‑midnight behavior:
  - NVS is overwritten with each successful new‑day dataset.
  - In‑RAM data for yesterday is invalidated at day rollover.

### UI / Menu

- Primary list:
  - 15‑minute detail for current hour.
  - Hourly averages for current + next 2 hours.
- Secondary list:
  - Expanded to 20 lines to include:
    - Time/date, last update, daily average.
    - Wi‑Fi RSSI, IP address.
    - API success rate, uptime.
    - NVS status block.
    - Credits & version line.

---

## v5.x – Earlier Versions

Earlier versions (v5.x and below) had:

- No NVS‑based caching of daily API data.
- More frequent API calls (e.g., hourly refresh pattern).
- Less robust handling of DST and daily boundaries.

For exact details, see older `.ino` files and their header comments in this repository.
