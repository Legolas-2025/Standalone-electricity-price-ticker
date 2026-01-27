# Electricity Price Ticker for XIAO ESP32‑C3 (Slovenia, Energy‑Charts)

This project is an Arduino‑IDE‑friendly firmware for the **Seeed XIAO ESP32‑C3** that:

- Connects to Wi‑Fi.
- Fetches day‑ahead electricity prices for **Slovenia** from [Energy‑Charts.info](https://energy-charts.info).
- Computes final consumer prices (including configurable fees + VAT).
- Displays current and upcoming prices on a **20x4 I²C 2004 LCD**.
- Uses an LED and presence sensor to give quick visual feedback.

The code currently implements **Version 6.0** of the ticker, focusing on:

- Daily (not hourly) API fetching.
- Robust **NVS storage** of daily price data for resilience to reboots.
- Automatic CET/CEST handling.
- Preserved UI and button behavior from v5.5.

All available bidding zones (modify in the code):
- AT - Austria 
- BE - Belgium
- BG - Bulgaria
- CH - Switzerland
- CZ - Czech Republic
- DE-LU - Germany, Luxembourg
- DE-AT-LU - Germany, Austria, Luxembourg
- DK1 - Denmark 1
- DK2 - Denmark 2
- EE - Estionia
- ES - Spain
- FI - Finland
- FR - France
- GR - Greece
- HR - Croatia
- HU - Hungary
- IT-Calabria - Italy Calabria
- IT-Centre-North - Italy Centre North
- IT-Centre-South - Italy Centre South
- IT-North - Italy North
- IT-SACOAC - Italy Sardinia Corsica AC
- IT-SACODC - Italy Sardinia Corsica DC
- IT-Sardinia - Italy Sardinia
- IT-Sicily - Italy Sicily
- IT-South - Italy South
- LT - Lithuania
- LV - Latvia
- ME - Montenegro
- NL - Netherlands
- NO1 - Norway 1
- NO2 - Norway 2
- NO2NSL - Norway North Sea Link
- NO3 - Norway 3
- NO4 - Norway 4
- NO5 - Norway 5
- PL - Poland
- PT - Portugal
- RO - Romania
- RS - Serbia
- SE1 - Sweden 1
- SE2 - Sweden 2
- SE3 - Sweden 3
- SE4 - Sweden 4
- SI - Slovenia
- SK - Slovakia

---

## Features

### Core display & pricing

- Data source: [Energy‑Charts.info day‑ahead price API](https://api.energy-charts.info/price?bzn=SI) (bidding zone = `SI`).
- Resolution:
  - Prices in **15‑minute intervals** (4 per hour).
  - Display shows:
    - Current hour:
      - Hourly average.
      - Four 15‑minute prices in compact format (`XX XX XX XX`).
    - Next 2 hours’ averages.
- Price calculation:
  - Raw MWh prices from API are converted to EUR/kWh.
  - Optional surcharges:
    - `POWER_COMPANY_FEE_PERCENTAGE` (default 12%).
    - `VAT_PERCENTAGE` (default 22%).
- LCD:
  - **20x4 I²C (PCF8574 / 0x27)**.
  - Custom characters for local language and low/high price markers.

### LED behavior

- One **white LED** connected to GPIO 5.
- LED indicates **current 15‑minute segment** price:
  - Very cheap (`<= 0.05 EUR/kWh`): smooth breathing.
  - Cheap, normal, expensive, very expensive: different steady / blinking / double‑blink patterns.
  - Negative or no data: LED off.

### Presence sensor & backlight

- Optional **RCWL‑0516** presence sensor on GPIO 9:
  - Presence = backlight on, LED enabled.
  - No presence for a while = LCD backlight off, LED disabled.
- If no presence sensor is detected, the backlight is kept on.

### Button behavior

- One button (or capacitive touch) on GPIO 4.
- Uses internal pull‑up by default.
- Actions:
  - **Single short press**:
    - Scrolls through the hourly view on the **primary** screen.
  - **Double press**:
    - Toggles between:
      - Primary price view
      - Secondary info view
  - **Long press (≈3s in your current repo)**:
    - Forces a **manual data refresh** (API fetch), regardless of daily schedule.

---

## Version 6.0 – Daily Fetch + NVS Storage

Version 6.0 is focused on:

1. **Reducing API calls**  
2. **Persisting daily data in NVS**  
3. **Making the device robust to power outages**

### 1. Daily fetch instead of hourly

Previously (v5.5), the firmware fetched data **every hour** at the top of the hour.

In **v6.0**, the behavior is:

- **On boot (after Wi‑Fi + time sync):**
  - Try to load **today’s** data from NVS:
    - If found: use it directly, **no API call** at boot.
    - If not found or invalid: schedule an immediate API fetch.
- **After midnight (local time):**
  - Detect day rollover via localtime.
  - Immediately:
    - Invalidate yesterday’s data (`isTodayDataAvailable = false`).
    - Force display to show **“No data for today”**.
    - Turn off the white LED (no price indication).
    - Enter **midnight fetch phase**.

#### Midnight fetch phase

When the date changes:

1. First fetch is scheduled **immediately**.
2. If it fails:
   - The device **stays in “No data for today”**, LED off.
   - Retry schedule:
     - 10‑minute interval, up to 5 attempts.
     - After 5 failures, retry once at each **top of the hour** until it succeeds.

As soon as a fetch for **today** succeeds:

- `isTodayDataAvailable` is set to `true`.
- The enforced `NO_DATA_OFFSET` state is cleared back to `CURRENT_PRICES`.
- The LCD and LED resume showing today’s prices.

> Importantly, **yesterday’s data is never re‑used** for today, both at boot and after midnight.

### 2. NVS (non‑volatile) storage of daily data

The ESP32‑C3’s built‑in NVS is used via `Preferences`:

- Namespace: `"my-ticker"`.
- Keys (in addition to the existing Wi‑Fi credentials):
  - `data_day`, `data_mon`, `data_year` – calendar date of the stored data.
  - `data_prc` – full raw JSON response from the Energy‑Charts API.
  - `data_last_store` – UNIX timestamp when the data was last written.

#### On boot

After time sync:

- If NVS has data whose date **matches today**:
  - `data_prc` JSON is deserialized into the in‑RAM `doc` (`StaticJsonDocument`).
  - `processJsonData()` is run, just like after a live fetch.
  - `isTodayDataAvailable = true`.
  - No initial API call is needed.
- If the stored date does **not** match today (or parsing fails):
  - The stored data is **ignored** for display.
  - The system starts from “No data for today” and will fetch new data.

#### After each successful fetch for today

- The entire JSON payload is stored as `data_prc` with the corresponding date and timestamp.
- This means:
  - A reboot **later in the same day** can immediately restore the latest prices from NVS.
  - Reboots the next morning will detect the date mismatch and fetch new data instead of showing stale data.

### 3. Secondary info (status) screen

The secondary menu (double‑click to toggle) has **20 lines**, shown in 4‑line pages.

It typically includes:

- Current date and time.
- Last successful update timestamp.
- Daily average price in EUR/kWh (with company fee + VAT applied).
- Wi‑Fi RSSI and IP address.
- API success ratio and total calls.
- Device uptime.
- **NVS status section**:
  - `NVS status:`
  - `Data day: DD.MM.YYYY` or `Data day: none`
  - `Last save: DD.MM.YY` or `Last save: none`
  - `NVS: OK (today)` / `NVS: old data` / `NVS: empty`
- Credits + version line:
  - `energy-charts.info`
  - `dynamic electricity`
  - `price ticker v6.0`
  - `by Amir Toki^ 2025`

(Exact ordering and layout may differ slightly, based on your latest edits.)

---

## Hardware setup

### Microcontroller

- **Seeed XIAO ESP32‑C3**

### LCD (I²C 2004)

- 20x4 (2004) character LCD with I²C backpack (PCF8574).
- Default I²C address: `0x27` (configurable in code).
- Library: `LiquidCrystal_I2C`.

### Button / touch input

- GPIO 4, internal pull‑up enabled.

**Mechanical pushbutton (default)**

- One leg to GPIO 4, other leg to GND.

**TTP223 capacitive touch (alternative)**

- VCC → 3.3 V
- GND → GND
- OUT → GPIO 4

Code change required for TTP223:

```cpp
// In handleButton() or wherever the button is read:
int reading = !digitalRead(buttonPin);
// Change to:
int reading = digitalRead(buttonPin); // if TTP223 output is HIGH when touched
```

(Your current code uses `!digitalRead(buttonPin)` assuming an active‑LOW mechanical button.)

### Presence sensor (RCWL‑0516)

- VCC → 3.3 V
- GND → GND
- OUT → GPIO 9
- 10 kΩ pull‑down resistor between GPIO 9 and GND.

If no sensor is detected at boot, the firmware keeps the LCD backlight always on.

### White LED

- LED (with appropriate series resistor) on GPIO 5.

---

## Wi‑Fi provisioning

If stored Wi‑Fi credentials are invalid or missing:

1. Device starts in AP mode with SSID: `MyTicker_Setup`.
2. DNS server redirects to a simple captive “Wi‑Fi Setup” page.
3. You can enter SSID and password, which are stored in NVS (`Preferences`).
4. The device restarts and attempts to connect with the new credentials.

---

## Building and flashing

1. Open the `.ino` file in **Arduino IDE** (ensure ESP32 board support is installed).
2. Select:
   - Board: `Seeed XIAO ESP32C3`
   - Port: correct serial port
3. Install the required libraries if missing:
   - `LiquidCrystal_I2C`
   - `ArduinoJson`
4. Compile and upload.

---

## Versioning and files

Key documentation files:

- [`VERSION.md`](./VERSION.md) – current firmware version and compatibility notes.
- [`CHANGELOG.md`](./CHANGELOG.md) – detailed list of changes between versions.
- `*.ino` – main firmware source file (v6.0 and later).

---

## License

This project is licensed under the MIT License – see the [LICENSE](./LICENSE) file for details.
