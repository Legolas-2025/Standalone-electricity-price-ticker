# Electricity Price Ticker – Version Information

## Current firmware

- **Version:** 6.1.1  
- **Release date:** 2026-03-07  
- **Target MCU:** Seeed XIAO ESP32‑C3  
- **Display:** 20x4 I²C LCD (PCF8574, default address `0x27`)  
- **API endpoint:** `https://api.energy-charts.info/price?bzn=SI`  
- **Resolution:** 15‑minute intervals, hourly averages for overview  

## Highlights of v6.1.1

- Fix: daily **lowest/highest hourly price marker** now includes **negative** and **0.0** prices.
- Fix: daily average is computed over the number of valid hours (instead of always dividing by 24 even when hours were skipped).

For full details, see:

- [CHANGELOG.md](./CHANGELOG.md)
- [README.md](./README.md)

## Previous firmware

- **Version:** 6.0.0  
- **Release date:** 2026-01-27  
- **Target MCU:** Seeed XIAO ESP32‑C3  
- **Display:** 20x4 I²C LCD (PCF8574, default address `0x27`)  
- **API endpoint:** `https://api.energy-charts.info/price?bzn=SI`  
- **Resolution:** 15‑minute intervals, hourly averages for overview  

## Highlights of v6.0.0

- Single **daily fetch** (on boot / after midnight) instead of hourly.
- **NVS storage** of daily data for resilience to power outages.
- Robust midnight rollover and retry logic:
  - First immediate fetch.
  - Up to 5 × 10‑minute retries.
  - Then top‑of‑hour retries until successful.
- Prevents yesterday’s prices from ever being shown as today’s.
- Secondary info menu extended with **NVS status** and clear version label.
