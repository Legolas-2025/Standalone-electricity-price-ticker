# Hardware Wiring Diagram (Seeed XIAO ESP32‑C3 Electricity Price Ticker)

This document depicts the wiring for the **Electricity Price Ticker** project based on the connection instructions in `README.md`.

It includes:
- Required peripherals (LCD + button)
- Supported optional peripherals (presence sensor + white LED + optional TTP223 touch button alternative)

> Further reference (official board documentation):
> - https://wiki.seeedstudio.com/XIAO_ESP32C3_Getting_Started/

> Notes:
> - Always connect **all grounds together** (ESP32 GND, LCD GND, sensor GND, LED GND).
> - Verify your **XIAO ESP32‑C3 pinout** in the Seeed documentation link above.
> - **Important:** The **10 kΩ pull-down resistor on D9 is mandatory at all times** (even if the presence sensor is not connected).
> - Button input is on **D2**. You may use either a **mechanical pushbutton** (default) *or* a **TTP223 touch module** (alternative), but not both in parallel unless you know what you’re doing.

---

## 1) Wiring overview (diagram)

```mermaid
flowchart TB
  MCU["Seeed XIAO ESP32-C3"]:::mcu

  LCD["20x4 I2C LCD 2004<br/>PCF8574 backpack<br/>I2C addr 0x27"]:::lcd

  %% Button input options (XIAO D2)
  D2NODE["D2 node<br/>(buttonPin input)"]:::io
  BTN["Mechanical pushbutton<br/>default option<br/>active LOW to GND"]:::btn
  TTP["TTP223 capacitive touch<br/>optional alternative<br/>OUT is HIGH when touched"]:::touch

  %% Presence-sensor input stage (XIAO D9) - always present
  D9NODE["D9 node<br/>(presencePin input)"]:::io
  RPD["10k pull-down resistor<br/>D9 to GND<br/>MANDATORY"]:::res
  PRES["RCWL-0516 presence sensor<br/>optional"]:::pres

  %% White LED output (XIAO D3)
  LED1["White indicator LED<br/>optional"]:::led

  PSU5V["5V supply<br/>USB-C or regulated 5V"]:::pwr
  V33["3.3V rail from XIAO"]:::pwr
  GND["Common GND"]:::gnd

  %% Power
  PSU5V -->|"5V"| MCU
  PSU5V -->|"5V or 3V3 if LCD supports"| LCD
  MCU -->|"3V3"| V33
  V33 -->|"3V3"| PRES
  V33 -->|"3V3"| TTP

  %% Grounds
  MCU --- GND
  LCD --- GND
  BTN --- GND
  TTP --- GND
  D2NODE --- GND

  D9NODE --- GND
  PRES --- GND

  LED1 --- GND
  PSU5V --- GND

  %% I2C
  MCU -->|"SDA D4"| LCD
  MCU -->|"SCL D5"| LCD

  %% Button input stage (always)
  MCU -->|"D2 (buttonPin)"| D2NODE
  BTN -->|"button to GND"| D2NODE
  TTP -->|"OUT to D2 node"| D2NODE

  %% Presence input stage (always)
  MCU -->|"D9 (presencePin)"| D9NODE
  D9NODE ---|"10k"| RPD
  RPD -->|"to GND"| GND
  PRES -->|"OUT to D9 node"| D9NODE

  %% White LED (single LED, directly from pin with series resistor)
  MCU -->|"D3 (whiteLedPin)"| LED1

classDef mcu fill:#e8f0ff,stroke:#2b5fd9,stroke-width:1px,color:#000;
classDef lcd fill:#fff4e5,stroke:#cc7a00,stroke-width:1px,color:#000;
classDef btn fill:#eaffea,stroke:#2d8a2d,stroke-width:1px,color:#000;
classDef touch fill:#e6fcff,stroke:#0077b6,stroke-width:1px,color:#000;
classDef pres fill:#f3e8ff,stroke:#7b2cbf,stroke-width:1px,color:#000;
classDef led fill:#ffe8ef,stroke:#c9184a,stroke-width:1px,color:#000;
classDef res fill:#f5f5f5,stroke:#444,stroke-width:1px,color:#000;
classDef pwr fill:#fff,stroke:#444,stroke-width:1px,color:#000;
classDef gnd fill:#fff,stroke:#000,stroke-width:1.5px,color:#000;
classDef io fill:#f5f5f5,stroke:#111,stroke-width:1px,color:#000;
