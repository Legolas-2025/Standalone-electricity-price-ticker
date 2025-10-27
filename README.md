```markdown
# Dynamic Electricity Price Ticker (XIAO ESP32C3)

This project displays day-ahead electricity prices for Slovenia (Energy-Charts API) on a 20x4 I2C LCD using a XIAO ESP32C3.
It shows hourly averages and 15-minute detail for the current hour, and uses a white LED as a visual price indicator.

## Features
- Fetches day-ahead prices from Energy-Charts API (bzn=SI)
- Displays current hour 15-minute detail on row 0 and hourly averages on remaining rows
- LED patterns indicate price ranges (breathing, steady, blink, double-blink, triple-blink)
- Wi‑Fi provisioning mode (AP + simple web UI)
- Automatic NTP sync and DST-aware local time (CET/CEST for Ljubljana)

## Important: DST / Timezone
The firmware initializes timezone with the CET/CEST POSIX TZ string so `localtime()` and conversions of API unix timestamps respect DST transitions for Ljubljana:

`configTzTime("CET-1CEST,M3.5.0/02:00,M10.5.0/03:00", "pool.ntp.org");`

## Hardware and Pinout
- Board: Seeed XIAO ESP32C3 (or compatible ESP32-C3 board)

Pin usage (GPIO - function):
- GPIO4  - Button (pushbutton to GND, internal pull-up enabled in code)
- GPIO5  - White LED (use series resistor ~220-470Ω)
- GPIO9  - Presence sensor (RCWL-0516) OUT
- GPIO21 - Built-in status LED (WiFi indicator)
- I2C (SDA / SCL) - LCD (LiquidCrystal_I2C), default address 0x27

### Presence sensor (RCWL-0516) wiring
- VCC -> 3.3V
- GND -> GND
- OUT -> GPIO9
- Crucial: add a 10kΩ pull-down resistor between GPIO9 (OUT) and GND.
  - Why: The RCWL-0516 output may float or remain high on boot; the pull-down ensures a defined LOW when idle and prevents the LCD backlight from unintentionally turning off or on.
  - If you do not use a presence sensor, the firmware keeps the backlight ON by default.

### Button wiring (mechanical pushbutton)
- One leg of the pushbutton to GPIO4, the other to GND. The code uses INPUT_PULLUP, so no external resistor is required.

### ALTERNATIVE: TTP223 CAPACITIVE TOUCH BUTTON
- If you prefer a capacitive touch button instead of mechanical pushbutton:
  WIRING:
  - VCC to 3.3V power rail
  - GND to GND
  - OUT to GPIO 4 (same pin as pushbutton)
  CODE CHANGES REQUIRED:
  - Find the line in the sketch that reads: `int reading = digitalRead(buttonPin);`
  - Change it to: `int reading = !digitalRead(buttonPin);`
  - This inverts the logic since the TTP223 outputs HIGH when touched, while the pushbutton pulls LOW when pressed.
  - No other changes are needed — all timing and debounce logic remains the same.
  BENEFITS:
  - No mechanical wear, sealed operation
  - Can be mounted behind thin non-metallic panels
  - More modern, sleek appearance

### White LED wiring
- GPIO5 -> 220Ω resistor -> LED anode
- LED cathode -> GND

### I2C LCD wiring
- SDA -> board SDA pin
- SCL -> board SCL pin
- VCC -> 5V or 3.3V depending on module
- GND -> GND

## Software
- Espressif ESP32 Arduino core (tested with esp32 by Espressif Systems v3.3.2 in Arduino IDE)
- ArduinoJson library

## Notes
- The project stores Wi‑Fi credentials in non-volatile storage (Preferences) and falls back to an AP provisioning portal if no credentials are saved.
- The code applies power company fees and VAT to convert wholesale EUR/MWh prices to final consumer EUR/kWh by default. You can disable fees by setting APPLY_FEES_AND_VAT to false.

## How to use
1. Upload the firmware to your XIAO ESP32C3.
2. If no Wi‑Fi credentials are stored, the board will start an AP `MyTicker_Setup` — connect and open the provisioning web UI.
3. After Wi‑Fi and NTP sync, the sketch will fetch prices and start displaying them.

## License
- Add license information here if you want to publish this repository.
```
