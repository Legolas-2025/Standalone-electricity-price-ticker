# Hardware Wiring Diagram (Seeed XIAO ESP32‑C3 Electricity Price Ticker)

This document depicts the wiring for the **Electricity Price Ticker** project based on the connection instructions in `README.md`.

It includes:
- Required peripherals (LCD + button)
- Supported optional peripherals (presence sensor + white LED / LED strip driver)

> Notes:
> - Always connect **all grounds together** (ESP32 GND, LCD GND, sensor GND, LED driver GND, external PSU GND).
> - Verify your **XIAO ESP32‑C3 SDA/SCL pins** in the Seeed pinout; the sketch uses the board default I²C pins.

---

## 1) Wiring overview (diagram)

```mermaid
flowchart TB
  MCU[Seeed XIAO ESP32‑C3]:::mcu

  LCD[20x4 I²C LCD (2004)\nPCF8574 backpack\nAddr: 0x27]:::lcd
  BTN[Momentary pushbutton\n(active LOW via INPUT_PULLUP)]:::btn
  PRES[RCWL‑0516 presence sensor (optional)]:::pres
  LED[White LED / LED strip control (optional)]:::led
  RPD[10 kΩ pull‑down resistor\n(required for presence sensor)]:::res
  MOSFET[MOSFET / transistor driver\n(recommended for LED strips)]:::drv

  PSU5V[5V supply\n(USB‑C or regulated 5V)]:::pwr
  PSU3V3[3.3V rail\n(from XIAO)]:::pwr
  GND[(Common GND)]:::gnd

  %% Power
  PSU5V -->|5V| MCU
  PSU5V -->|5V (or 3V3 if LCD supports it)| LCD
  MCU -->|3V3| PSU3V3
  PSU3V3 -->|3.3V| PRES

  %% Grounds
  MCU --- GND
  LCD --- GND
  PRES --- GND
  LED --- GND
  MOSFET --- GND
  PSU5V --- GND

  %% I2C
  MCU -->|SDA| LCD
  MCU -->|SCL| LCD

  %% Button
  MCU -->|GPIO 4 (buttonPin)| BTN
  BTN -->|to GND| GND

  %% Presence sensor
  MCU -->|GPIO 9 (presencePin)| PRES
  RPD -->|10 kΩ| GND
  MCU -->|GPIO 9 node| RPD

  %% White LED / strip driver
  MCU -->|GPIO 5 (whiteLedPin)| LED
  MCU -->|GPIO 5 (better)| MOSFET
  MOSFET -->|drives| LED

classDef mcu fill:#e8f0ff,stroke:#2b5fd9,stroke-width:1px,color:#000;
classDef lcd fill:#fff4e5,stroke:#cc7a00,stroke-width:1px,color:#000;
classDef btn fill:#eaffea,stroke:#2d8a2d,stroke-width:1px,color:#000;
classDef pres fill:#f3e8ff,stroke:#7b2cbf,stroke-width:1px,color:#000;
classDef led fill:#ffe8ef,stroke:#c9184a,stroke-width:1px,color:#000;
classDef res fill:#f5f5f5,stroke:#444,stroke-width:1px,color:#000;
classDef drv fill:#f5f5f5,stroke:#444,stroke-width:1px,color:#000;
classDef pwr fill:#fff,stroke:#444,stroke-width:1px,color:#000;
classDef gnd fill:#fff,stroke:#000,stroke-width:1.5px,color:#000;
```

---

## 2) Pin mapping (as used by the sketch)

| Peripheral | Signal | XIAO ESP32‑C3 pin | Notes |
|---|---:|---:|---|
| LCD (I²C, PCF8574) | SDA | **Board I²C SDA** | Check Seeed pinout for your board variant |
| LCD (I²C, PCF8574) | SCL | **Board I²C SCL** | Check Seeed pinout for your board variant |
| LCD (I²C, PCF8574) | VCC | **5V** (or 3V3) | Many LCD backpacks expect 5V; use 3V3 only if your module supports it |
| LCD (I²C, PCF8574) | GND | GND | Common ground |
| Button | input | **GPIO 4** (`buttonPin`) | Sketch uses `INPUT_PULLUP`; wiring is button → GPIO4 and button → GND |
| Presence sensor (RCWL‑0516, optional) | OUT | **GPIO 9** (`presencePin`) | Requires pull‑down resistor |
| Presence sensor (RCWL‑0516, optional) | VCC | **3.3V** | Recommended 3.3V |
| Presence sensor (RCWL‑0516, optional) | GND | GND | Common ground |
| Pull‑down resistor (required if presence sensor used) | 10 kΩ | **GPIO 9 → GND** | Stabilizes the OUT line |
| White LED (optional) | control | **GPIO 5** (`whiteLedPin`) | Use series resistor for a single LED |
| LED strip driver (recommended for strips) | gate/base | **GPIO 5** | Use a MOSFET/transistor + external supply; do not power strips from GPIO |

---

## 3) Practical wiring notes

### LCD (I²C)
- The sketch instantiates: `LiquidCrystal_I2C lcd(0x27, 20, 4);`
- If your LCD uses a different I²C address, you must change `0x27`.

### Button (default)
- Recommended wiring:
  - One leg of the button → **GPIO 4**
  - Other leg → **GND**
- The code reads it as active-low using:
  - `pinMode(buttonPin, INPUT_PULLUP);`
  - `int reading = !digitalRead(buttonPin);`

### Presence sensor (RCWL‑0516, optional)
- Wiring:
  - `VCC` → **3.3V**
  - `GND` → **GND**
  - `OUT` → **GPIO 9**
  - **10 kΩ pull‑down** between **GPIO 9** and **GND** (required)

### White LED / LED strip output (optional)
- Single LED wiring:
  - **GPIO 5 → series resistor (220–470 Ω) → LED anode**
  - LED cathode → **GND**
- LED strip / higher current:
  - Use a logic-level MOSFET (or suitable transistor) and an external PSU.
  - Ensure **common ground** between PSU and ESP32.

---

## 4) Included peripherals checklist

- [x] Seeed XIAO ESP32‑C3
- [x] 20x4 I²C LCD (PCF8574 backpack)
- [x] Pushbutton (or touch alternative using same GPIO)
- [x] Presence sensor RCWL‑0516 (optional)
- [x] 10 kΩ pull‑down resistor for presence sensor (required when used)
- [x] White LED output / LED strip driver (optional)