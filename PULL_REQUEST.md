Summary:
- Adds DST/timezone initialization (CET/CEST) using configTzTime so localtime() and conversions of API unix timestamps respect DST transitions for Ljubljana.
- Fixes provisioning HTML string termination bug that caused compilation error.
- Adds README with detailed pinout and wiring instructions, including the required 10k pull-down for the RCWL-0516 presence sensor, and instructions for using a TTP223 capacitive touch button.
Testing:
- Compiled successfully with esp32 by Espressif Systems v3.3.2 in Arduino IDE.
Notes:
- All original application logic, display behavior, and LED patterns are preserved.