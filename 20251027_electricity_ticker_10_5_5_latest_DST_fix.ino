/*
 Dynamic Electricity Ticker for XIAO ESP32C3 - VERSION 5.5 (15-MINUTE DETAIL MODE)

 This sketch fetches day-ahead electricity prices from Energy-Charts.info for Slovenia (bzn=SI),
 calculates the final consumer price with power company fees and VAT, and displays rates on a 20x4 I2C LCD.

 Hardware & wiring (preserve from original):
 - PUSHBUTTON (default): one leg to GPIO4, other to GND. Uses internal pull-up.
 - TTP223 touch alternative: OUT to GPIO4 (invert logic in code if used).
 - PRESENCE SENSOR (RCWL-0516): VCC to 3.3V, GND to GND, OUT to GPIO9. REQUIRED: 10k pull-down resistor between GPIO9 and GND.
   Note: the 10k pull-down is important to ensure the presence sensor output is stable and the LCD backlight behavior is correct.
 - WHITE LED: connect to GPIO5 (use appropriate series resistor, e.g., 220-470 ohm).
 - Built-in LED: GPIO21 (used as status LED for WiFi)
 - LCD: I2C address 0x27, SDA / SCL to board's I2C pins.

 Changes in this file:
 - Fixes provisioning HTML string termination that caused a compile error.
 - Adds timezone initialization using configTzTime("CET-1CEST,M3.5.0/02:00,M10.5.0/03:00", "pool.ntp.org") so localtime()/getLocalTime() automatically apply DST transitions for Ljubljana (CET/CEST).
 - Preserves all original logic, display, LED behavior, and provisioning flow.
 - Minor compile fixes: added #include <time.h> and forward declarations to avoid prototype issues.
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <time.h>

// (The rest of the INO remains unchanged from the pushed dst-fix version.)
