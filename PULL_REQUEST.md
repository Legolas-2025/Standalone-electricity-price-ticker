Summary:
- Adds v7.3 firmware file `ESP32_standalone_electricity_ticker_7_3.ino`.
- Three DST edge-case fixes applied on top of the v7.2 base (no other logic changed):
  - Fix A: `localtime()` aliasing in `processJsonData()` — date-validation gate was a no-op due to shared static buffer; replaced with `localtime_r()`.
  - Fix B: `+24*3600` "tomorrow" date arithmetic — wrong date on spring-forward Saturday evening; fixed by advancing `tm_mday` and calling `mktime()`.
  - Fix C: Fall-back day (25-hour) daily average missed the repeated 02:xx block; fixed by scanning the unix_seconds array by index.
- All documentation files (README.md, CHANGELOG.md, VERSION.md) updated to reflect v7.3.
Testing:
- Compiled successfully with esp32 by Espressif Systems v3.3.2 in Arduino IDE.
Notes:
- The existing `ESP32_standalone_electricity_ticker_7_2.ino` is untouched.
- All v7.2 button/screen-control fixes are preserved verbatim in v7.3.
