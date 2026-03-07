# Electricity Price Ticker for XIAO ESP32‑C3 (Energy‑Charts)

This project is an Arduino‑IDE‑friendly firmware for the **Seeed XIAO ESP32‑C3** that:

- Connects to Wi‑Fi.
- Fetches **day‑ahead electricity prices** from [Energy‑Charts.info](https://energy-charts.info) (Bundesnetzagentur / SMARD.de).
- Computes final consumer prices (including configurable power‑company fee + VAT).
- Displays current and upcoming prices on a **20x4 I²C 2004 LCD**.
- Uses a white LED and an optional presence sensor to give quick visual feedback.
- Stores daily price data in **NVS** to survive reboots and reduce API calls.

The latest sketch implements **Version 6.1.1**, focusing on:

- Version 6.1.1 fix: Correct daily **low/high hourly markers** (now includes negative and **0.0** prices).
- Daily (not hourly) API fetching.
- Robust **NVS storage** of daily price data.
- Correct **CET/CEST** handling.
- Resilient **after‑midnight refresh** (no more getting stuck on “No data for today”).
- Preserved UI and button behavior from v5.5.

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

All available bidding zones (from the original README):

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

This section merges the original v5.5 instructions with the current v6.1 hardware expectations.  
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

- **LCD backpack → XIAO ESP32‑C3**
  - `VCC` → **5V** (or 3V3 if your module explicitly supports 3.3V I²C)
  - `GND` → **GND**
  - `SDA` → board I²C SDA pin (see XIAO ESP32‑C3 documentation)
  - `SCL` → board I²C SCL pin

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

**Wiring (from the v5.5 header, preserved in v6.x):**

- `VCC` → **3.3V**
- `GND` → **GND**
- `OUT` → `GPIO 9` (`presencePin`)
- **Required**: 10 kΩ pull‑down resistor between `GPIO 9` and `GND`.

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

The LED is driven with various patterns to indicate price level; see “LED Price Signalling” below.

---

### 6. Power

- XIAO ESP32‑C3:
  - Via USB‑C (recommended for development).
  - Or via 5V pin if you have a regulated 5V supply (check Seeed docs).
- Ensure **all modules** (LCD, presence sensor, LED driver) share a **common ground** with the XIAO.

---

## Firmware Features (v6.1.1)

### Core Display & Pricing

- Data source:

  ```text
  https://api.energy-charts.info/price?bzn=SI
  ```

- Resolution:
  - Prices in **15‑minute intervals** (`price[]`, `unix_seconds[]`).
  - Display shows:
    - **Row 0**: Current hour, four 15‑minute values in compact format (`XX XX XX XX`).
    - **Rows 1–3**: Current hour + next two hours as hourly averages.
- Price calculation:
  - Raw MWh prices are converted to **EUR/kWh**.
  - Two configurable surcharges:
    - `POWER_COMPANY_FEE_PERCENTAGE` (default `12.0` %).
    - `VAT_PERCENTAGE` (default `22.0` %).
- LCD:
  - `LiquidCrystal_I2C` with custom characters for:
    - Local language letters.
    - Low‑price and high‑price indicators.
- **Daily min/max markers**:
  - The low/high hourly indicators now consider **negative**, **0.0**, and positive prices (v6.1.1 fix).

### LED Price Signalling

The white LED (GPIO 5) reflects the **current 15‑minute interval** price:

- Very cheap (`<= 0.05 EUR/kWh`) → smooth breathing.
- Cheap / normal → steady on.
- Moderately expensive → slow blink.
- Expensive → faster blink.
- Very expensive → complex “double‑blink with long on” pattern.
- Negative price or no data → LED off.

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
  - On primary screen: scrolls the time offset (future hours).
  - On secondary screen: scrolls through the 20‑line status text (4 lines at a time).
- **Double press**:
  - Toggles between:
    - Primary price view.
    - Secondary status/info view.
- **Long press (~3 seconds in v6.1)**:
  - While held:
    - LCD shows: “Long press detected! Release to refresh”.
  - On release:
    - Forces a **manual data refresh**:
      - Sets `nextScheduledFetchTime = now`.
      - Shows “Manual Refresh… Please wait…”.
      - `handleDataFetching()` will perform an immediate API fetch outside the normal schedule.

An **auto‑scroll timeout** resets the view to “current hour / top of lists” after inactivity.

---

## NVS Storage (Daily Data Cache)

This firmware uses ESP32‑C3 **Preferences API** (`Preferences`) under namespace `"my-ticker"`.

Stored keys:

- **Wi‑Fi credentials:**
  - `ssid`
  - `pass`
- **Daily price data:**
  - `data_day`   – calendar day (1–31)
  - `data_mon`   – month (0–11)
  - `data_year`  – full year (e.g. 2026)
  - `data_prc`   – full raw JSON payload from the API
  - `data_last_store` – Unix time (`time_t`) when data was last written

### On Boot

After successful NTP time sync:

1. Attempt to load `data_day`, `data_mon`, `data_year`, and `data_prc` from NVS.
2. If **stored date matches current local date**:
   - Deserialize `data_prc` into `StaticJsonDocument doc`.
   - Run `processJsonData()` as if it were fresh from the API.
   - Set `isTodayDataAvailable = true`.
   - **Skip** the initial API call to save traffic.
3. If the stored date does **not** match today or JSON parsing fails:
   - NVS data is **ignored** for display.
   - System starts from “No data for today”.
   - Schedules an immediate API fetch.

### After Each Successful Fetch for Today

- Raw JSON payload is stored into NVS as `data_prc`, along with date (`data_day`, `data_mon`, `data_year`) and `data_last_store`.
- On reboot later the same day, the device will show prices immediately from NVS without hitting the API.

---

## Daily Fetch Strategy (v6.1.0+)

### Goals

- **Avoid hourly polling** of the API.
- Fetch:
  - Once after boot (if no valid NVS data for today).
  - Once per **new day** (after midnight), with robust retries while the next‑day dataset is not yet published.

### Time Sync & First Fetch

- `configTzTime(TZ_CET_CEST, "pool.ntp.org")` is used to enable CET/CEST aware `localtime()` and `getLocalTime()`.
- Until time sync completes, the UI only shows “Syncing Time… Please wait…”.
- On first successful sync:
  - `isTimeSynced = true`.
  - `trackedDay` is set to the current `tm_mday`.
  - Either NVS is used (if it has today’s data) or an initial fetch is scheduled.

### Day‑Rollover Detection

In the main `loop()`:

- `trackedDay` holds the last seen local day.
- Each iteration:
  - Get `localtime()` for `now`.
  - If `tm_mday != trackedDay`:
    - Day rollover detected (midnight).
    - `trackedDay` updated.
    - Immediately:
      - `isTodayDataAvailable = false`.
      - `displayState = NO_DATA_OFFSET`.
      - `timeOffsetHours = 0`.
      - White LED turned off.
      - **Midnight phase** is entered:
        - `midnightPhaseActive = true`.
        - `midnightRetryCount = 0`.
      - `nextScheduledFetchTime = now` (immediate attempt).
      - LCD updated to “No data for today. Press & hold to refresh manually”.

### “Today” Detection (Market Day Logic)

The Energy‑Charts API can keep serving **yesterday’s** market day for some time after local midnight.  
To avoid accidentally accepting yesterday’s data as today’s, v6.1 uses a more robust rule.

In `processJsonData()`:

1. Read `unix_seconds[]`.
2. Interpret the **LAST** timestamp as representing the end of the dataset’s market day.
3. Convert it to local time (`localtime()`).
4. Compare its date (day, month, year) to the current local date.
   - If they **match**:
     - Dataset is accepted as “today’s” data.
     - `isTodayDataAvailable = true`.
     - `lastProcessJsonAcceptedToday = true`.
     - Prices are processed (hourly averages, min/max, daily average).
   - If they **do not match**:
     - Dataset is considered to belong to a **different** day (e.g. yesterday).
     - `isTodayDataAvailable = false`.
     - `lastProcessJsonAcceptedToday = false`.
     - Function returns without updating display data.

This prevents the device from accidentally treating “yesterday’s day‑ahead curve” as if it were already “today”.

### Midnight Retry Logic (v6.1.0 + your tuning)

When `midnightPhaseActive == true`, any scheduled fetch that:

- Fails at HTTP/JSON level, **or**
- Succeeds at HTTP/JSON level but `processJsonData()` **rejects** the dataset as “not today”

is treated as a **failure** for scheduling.

The retry rules:

1. **First hour after midnight – fast retries:**

   In `scheduleAfterMidnightFailure()` (with your current config):

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

   Timeline:

   - 00:00 – first attempt at rollover.
   - If data is still yesterday’s:
     - 00:20 – 1st retry.
     - 00:40 – 2nd retry.
   - All “fast retries” remain fully within the first post‑midnight hour.

2. **After the first hour – hourly retries:**

   Once `midnightRetryCount >= 2`, next retries are scheduled at the **top of each hour**:

   ```cpp
   struct tm* ti = localtime(&now);
   if (ti != NULL) {
       time_t nextHour = now - (ti->tm_min * 60) - ti->tm_sec + 3600;
       nextScheduledFetchTime = nextHour;
       debugPrint(2, "Midnight retries exhausted; next fetch top-of-hour");
   } else {
       nextScheduledFetchTime = now + 3600;
       debugPrint(2, "Midnight retries exhausted; fallback 1h");
   }
   ```

   So after ~00:40, if still no valid dataset for today, the device tries again at ~01:00, 02:00, 03:00, … until success.

3. **Success condition & exit from midnight phase:**

   A scheduled fetch is treated as a **real success** only if:

   - HTTP + JSON succeed **and**
   - `lastProcessJsonAcceptedToday == true` (dataset’s last timestamp’s date matches today).

   When this happens:

   - `isTodayDataAvailable = true`.
   - `midnightPhaseActive = false`.
   - `midnightRetryCount = 0`.
   - LCD leaves `NO_DATA_OFFSET` back to `CURRENT_PRICES`.
   - White LED resumes price indication.
   - `nextScheduledFetchTime` is set ≈24 hours ahead (until the next midnight rollover resets it).

---

## Secondary Status Screen (Debug / Info)

A **secondary screen** (toggled via **double‑click**) provides 20 lines of status information, displayed 4 lines at a time:

Typical content:

1. Current date and time (`HH:MM  DD.MM.YYYY`)
2. Separator line (`--------------------`)
3. “Zadnja posodobitev:” (Last update header)
4. Last successful fetch (for today) date & time
5. Blank
6. “Dnevno povprečje:” (Daily average)
7. Daily average price in EUR/kWh (with surcharges) or “Cene niso na voljo.”
8. Blank
9. Wi‑Fi status and RSSI
10. Local IP address
11. API success rate (`API: xx% (succ/fail)`)
12. Device uptime in days, hours, minutes
13–16. **NVS status block**:
    - `NVS status:`
    - `Data day: DD.MM.YYYY` or `Data day: none`
    - `Last save: DD.MM.YY` or `Last save: none`
    - `NVS: OK (today)` / `NVS: old data` / `NVS: empty`
17–20. Credits and version:
    - `energy-charts.info`
    - `dynamic electricity`
    - `price ticker v6.1`
    - `by Legolas-2025` (or your preferred credit line)

---

## Wi‑Fi Provisioning

If NVS does not contain valid Wi‑Fi credentials, or if connecting fails repeatedly:

1. The device starts an **Access Point** with SSID:

   ```text
   MyTicker_Setup
   ```

2. LCD shows “No Wi‑Fi access! Setup Wi‑Fi: SSID: MyTicker_Setup” and the AP IP.
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
3. Open the v6.1 `.ino` file (e.g. `20260130_electricity_ticker_6_1_nvs_daily_fetch.ino`).
4. In Tools:
   - Board: `Seeed XIAO ESP32C3`
   - Port: choose the correct serial port.
5. Upload the sketch.
6. Open Serial Monitor at **115200 baud** to see:
   - Wi‑Fi connection logs.
   - NTP sync messages.
   - NVS load/save status.
   - Midnight rollover and retry debug output.

---

## Versioning & Changelog

- **v5.5** – 15‑minute detail mode, LED based on current 15‑minute slot, improved DST handling (see file `20251027a_electricity_ticker_10_5_5_latest_DST_and_midnight_fix.ino`).
- **v6.0.0** – First NVS‑enabled version:
  - Store daily price data in NVS.
  - Reduce API calls to “boot + after‑midnight”.
  - Add NVS status section to secondary menu.
- **v6.1.0** – Midnight fetch & “today” detection fixes:
  - Correctly detect **market day** using the last `unix_seconds` timestamp.
  - Distinguish between:
    - HTTP/JSON success, but data for **wrong day** (treated as failure).
    - Full success with accepted “today” dataset.
  - Robust midnight retry scheme:
    - Two retries every 20 minutes in the first hour (~00:20, ~00:40).
    - Then hourly retries (top‑of‑hour) until today’s dataset is available.
  - Behavior on reboot and manual long‑press is unchanged, but now respects the improved “today” logic.

See [`CHANGELOG.md`](./CHANGELOG.md) for more details.

---

## License

This project is licensed under the MIT License – see the [`LICENSE`](./LICENSE) file for details.
