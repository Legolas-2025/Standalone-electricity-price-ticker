# Changelog

All notable changes to this project will be documented in this file.

This project follows a simple semantic versioning style:  
`MAJOR.MINOR.PATCH`

---

## [6.0.0] - 2026-01-27

### Added
- **Daily fetch + NVS storage (ESP32‑C3 NVS)**
  - Introduced non‑volatile storage for daily price data using the existing `Preferences` (NVS) subsystem under the `"my-ticker"` namespace.
  - Stored fields:
    - `data_day`, `data_mon`, `data_year` (calendar date of the data)
    - `data_prc` (raw JSON returned from the Energy‑Charts API)
    - `data_last_store` (UNIX timestamp of last successful store)
  - On boot, after Wi‑Fi and NTP time sync:
    - The ticker checks NVS for stored data.
    - If the stored date matches **today**, the JSON is deserialized and used directly (no initial API call needed).
    - If the stored date is **not** today (or invalid), the data is ignored and the ticker behaves as if no data is available yet.

- **Midnight rollover + daily fetch logic**
  - The system now performs **one daily fetch per day**, instead of hourly:
    - **On boot**: fetch only if NVS does not already contain valid data for today.
    - **After local midnight**: invalidate the previous day’s data and trigger a new fetch for the new day.
  - As soon as a local‑time day change is detected:
    - `isTodayDataAvailable` is set to `false`.
    - The display state is forced to `"No data for today"` (`NO_DATA_OFFSET`).
    - The white LED is turned off (same behavior as “no data”).
    - A midnight fetch phase is activated.

- **Midnight fetch retry strategy**
  - When midnight is detected:
    - First fetch is scheduled **immediately**.
    - If it fails (HTTP error or JSON/“day mismatch”), the system stays in `"No data for today"` and the LEDs remain off.
  - Retry policy during midnight phase:
    1. Retry every **10 minutes**, up to **5 attempts**.
    2. If still unsuccessful, retry only once **at the top of each following hour** until fresh data for the new day is retrieved.
  - As soon as a successful fetch for today is obtained:
    - `isTodayDataAvailable = true`.
    - The enforced `NO_DATA_OFFSET` state is cleared back to `CURRENT_PRICES`.
    - The white LED resumes indicating the current 15‑minute segment price.
    - The entire successful JSON payload and metadata are saved back into NVS.

- **Improved resilience after power loss**
  - If the device reboots during the same day:
    - It can **restore and reuse** the last successfully stored daily data from NVS.
    - This avoids unnecessary API calls and gives a fast “warm start” after power outages.
  - If the device reboots on the **next** day:
    - Yesterday’s stored data is **not** used for display (to avoid confusion).
    - The display starts in `"No data for today"` until the first successful fetch.

- **Extended secondary (info) menu**
  - The secondary info screen (reachable via double‑click on the button) is extended from 16 to **20 lines**, still shown as 4‑line pages.
  - Existing info retained:
    - Current time and date.
    - Last successful update timestamp.
    - Daily average price.
    - Wi‑Fi RSSI and device IP.
    - API success ratio.
    - Uptime.
    - Credits and version information.
  - **New NVS status section** (first defined around lines 12–15, later rearranged in your version):
    - Shows:
      - NVS data status (`NVS status:`)
      - Stored data date (`Data day: DD.MM.YYYY` or `Data day: none`)
      - Last store timestamp (`Last save: DD.MM.YY` or `Last save: none`)
      - Basic quick status:
        - `NVS: OK (today)` – valid data for today loaded from NVS
        - `NVS: old data` – NVS has data, but not from today (ignored for display)
        - `NVS: empty` – no stored price data present

### Changed
- **API call strategy**
  - Removed regular **top‑of‑hour** automatic fetching.
  - API calls now occur only:
    - Once at boot (if NVS has no valid data for today).
    - After midnight (with the retry strategy described above).
    - On user‑initiated **long‑press** (manual refresh).
  - This significantly reduces network load while keeping behavior safe and predictable.

- **UI & behavior around day boundaries**
  - At midnight / day change:
    - Display is immediately set to:
      - Line 0: `No data for today`
      - Line 1: `Press & hold to`
      - Line 2: `refresh manually`
    - White LED is turned off (no price indication until new data arrives).
  - Once data for the new day is available:
    - The ticker returns to the usual 15‑minute detail + hourly display mode.

- **Versioning and info screens**
  - Version bumped to **v6.0** and reflected in:
    - Source file header comment.
    - Secondary menu text (`price ticker v6.0`).
    - New version documentation (`VERSION.md` / `CHANGELOG.md` / `README.md`).

- **Long‑press threshold (in repository version)**
  - Long‑press detection threshold extended from 2s to **3s** (in your repository copy) to avoid accidental manual refreshes.

### Fixed / Ensured
- Yesterday’s data is **never shown** as if it were today’s:
  - On boot: previous‑day NVS data is ignored for display.
  - After midnight: in‑RAM data is invalidated and the UI explicitly shows `"No data for today"` until fresh data is fetched.
- LED state is always consistent with the availability of **today’s** data:
  - LED off → no today data (or negative price).
  - LED patterns → valid today data and a positive price in the current 15‑minute interval.

---

## [5.5.0] - 2025-10-27

> First 15‑minute detail version and DST‑fixed base, which v6.0 builds upon.

### Added
- **15‑minute detail mode**:
  - Primary display shows:
    - Current hour average.
    - Four 15‑minute prices in compact format (`XX XX XX XX`).
    - Next 2 hours’ averages.
  - LED behavior switched from hourly‑based to **15‑minute‑segment based**.
- **Compact price format**:
  - 15‑minute values shown as 2‑digit hundredths (e.g. `+99 -07  24  11`).
- **DST / timezone handling**:
  - `configTzTime()` with `TZ_CET_CEST` for automatic CET ↔ CEST switching.
- **Aggressive retry and state enforcement**:
  - Improved logic for:
    - Handling stale data.
    - Enforcing `"No data for today"` state when needed.
    - Aggressively retrying fetches after HTTP/JSON failures.
- **UI and usability tweaks**:
  - Button:
    - Single click: scroll primary list (hourly view).
    - Double click: switch primary/secondary list.
    - Long press: manual refresh.
  - Secondary info list:
    - Date/time, last update, daily average, Wi‑Fi and API stats, uptime, credits.

---

## Older versions

Earlier versions (≤5.4) introduced the basic ticker behavior, LCD layout, Energy‑Charts API integration, and the initial presence sensor / backlight / LED logic.

Those versions are not fully documented here, but key user‑visible behavior is maintained in v6.0 unless explicitly noted in this changelog.
