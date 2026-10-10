# Electricity Price Ticker – Version Information

## Current firmware

- **Version:** 7.6
- **Release date:** 2026-10-10
- **Target MCU:** Seeed XIAO ESP32‑C3
- **Display:** 20x4 I²C LCD (PCF8574, default address `0x27`)
- **Input:** TTP223 capacitive touch pad on D2 (GPIO4); polarity is declared
  once as `BUTTON_ACTIVE_HIGH` — see Fix I
- **API endpoint:** `https://api.energy-charts.info/price?bzn=SI`
  (both the today and the tomorrow fetch add explicit `&start=&end=` date
  bounds — v7.5 Fix B and v7.6 Fix H)
- **Resolution:** 15‑minute intervals, hourly averages for overview

## Highlights of v7.6

### Gesture-Safe Button + Single-Market-Day Tomorrow Fetch

Two changes on top of v7.5: the button path is rebuilt so a touch gesture
survives a blocking HTTP fetch, and the tomorrow fetch asks for exactly one
market day instead of an open-ended window.

Fee/VAT math, NVS layout, 48-hour scrolling behaviour, Midnight Bridge logic
and all DST logic are unchanged. All v7.5 fixes (A–F2), all v7.4 scheduling
fixes (A, B, E, G) and all v7.2 button/screen fixes (1, 2, 4) are preserved.
v7.4's Fix C survives as a structural property rather than a defensive check:
the long-press decision is derived from a measured duration, so idle time can
no longer set a flag that hijacks a short click.

#### Fix H – Tomorrow fetch now sends an end date (moderate)

The today fetch gained explicit date bounds in v7.5 (Fix B), but the tomorrow
fetch still sent `&start=<tomorrow>` with no `&end=`. On the Energy-Charts API
`end` is inclusive and defaults to the end of the available window, so the
tomorrow request asked for several days of data. `processJsonData()` keeps only
entries whose local day matches the target, so the surplus was downloaded,
parsed and discarded — a payload several times larger than needed, on the
slowest path in the firmware, at the hours when the button is most likely to be
in use. The request now sends `start == end`, the same single-market-day
contract as the today fetch.

#### Fix I – Button polarity is declared once (`BUTTON_ACTIVE_HIGH`)

`HARDWARE_WIRING_DIAGRAM.md` documents two interchangeable inputs on D2 that are
inverted with respect to each other: a mechanical pushbutton wired to GND reads
LOW when pressed, while a TTP223 capacitive touch pad reads HIGH when touched.
v7.5 read the pin as `!digitalRead(buttonPin)` — correct only for the active-HIGH
TTP223 — while every surrounding comment described the pin as active-LOW and
called HIGH "released". That ambiguity is what the v7.2 `buttonEverReleased`
workaround and v7.4 Fix C were papering over.

From v7.6 the polarity lives in one constant, `BUTTON_ACTIVE_HIGH` (default
`true` = TTP223, matching the documented hardware; set `false` for a mechanical
pushbutton), and everything downstream speaks in terms of "pressed" / "not
pressed" through `buttonIsPressed()` and `buttonReadingToState()` instead of raw
pin levels.

#### Fix J – Gestures are captured by the ISR, not reconstructed by the loop (critical)

v7.4's Fix D recorded only "an edge happened" and `handleButton()` then rebuilt
the press from whatever the pin was doing when the loop finally got back to it.
That reconstruction is where gestures were lost: a press+release that completed
entirely inside a 10–15 s `http.GET()` leaves the pin back at its idle level, so
the state machine saw no press edge, no release edge and no duration — the click
vanished. v7.5's synthetic injection still dropped everything except one
fabricated click.

v7.6 records the gesture itself, in the interrupt, where the timing is real:

- `attachInterrupt(..., CHANGE)` sees both the press and the release edge.
- The press edge stores the touch-start timestamp, gated by `debounceDelay` so
  contact bounce cannot open a second gesture.
- The release edge closes a `(start, end)` millisecond pair and pushes it onto an
  8-slot FIFO. A touch shorter than `GESTURE_MIN_MS` (40 ms) is contact bounce:
  the gesture is left open and waits for the real release rather than being
  rejected outright, which would lose it.
- `handleButton()` drains the FIFO once per gesture and classifies each one from
  its measured duration.

Because both timestamps are taken by the ISR, the measured duration is exact and
independent of how long the loop was blocked, and the 500 ms double-click window
is anchored to the real gesture times rather than to the loop's drain times. The
debounce state machine is kept, but only for what it is genuinely useful for
now: tracking "is the user still holding?" so the LCD can show the live
"Long press detected! / Release to refresh" feedback. It no longer classifies
gestures, so it can no longer lose them.

### v7.5 known issues (a) and (b) — resolved for the button path

**(b) "A double-click can collapse to a single click during a freeze" is fixed.**
`buttonInterruptFired` no longer reconstructs a gesture; the FIFO carries every
gesture the ISR saw. Two clicks made four seconds apart during a blocked fetch
now produce two separate advances, and a genuine double-click performed during a
fetch is still recognised as a double.

**(a) "The UI can freeze for a few seconds during a double-click" is unchanged
as a freeze.** `http.GET()` is still synchronous, so the display cannot redraw
and the LED holds its last PWM value while a request is in flight. What changes
is the cost: the freeze no longer swallows the gesture. Fix G still prevents a
fetch from *starting* within 800 ms of a button edge; Fix J makes a fetch that
is already running survivable.

Eliminating the freeze itself still requires porting the fetch to the
asynchronous `esp_http_client` API — deliberately not undertaken here.

### Verification status

Verified by a behavioural simulation of `buttonISR()`, `processButtonPress()`
and `handleButton()` (`_v76_sim.py`) driven with the firmware's own constants
(`longPressThreshold` 3000 ms, `doubleClickWindow` 500 ms, `debounceDelay`
50 ms, `GESTURE_MIN_MS` 40 ms, `GESTURE_SLOTS` 8). All 14 scenarios produced the
expected outcome: single click, double click, 3.5 s long press, 2.5 s press
below threshold, click and long press during a 12 s blocked fetch, two clicks
4 s apart during a fetch, double click during a fetch, 20 ms bounce rejection,
bouncy press, 100 s idle with no phantom long press, pad held from boot (never
released and released later), and 7 / 10 rapid clicks during a fetch.

**Compiled and flashed on hardware:** the sketch builds for the Seeed XIAO
ESP32C3 (`esp32:esp32:XIAO_ESP32C3`, ESP32 Arduino core 3.3.12) and has been
uploaded to the board and run. On the device, four double-clicks each toggled
the list, two pairs 4.4 s and 5.2 s apart stayed four separate gestures, and a
long press reported `measured 3347 ms` after the live threshold feedback. A
click made during a blocked fetch has not yet been re-timed on the touch pad.

### Cosmetic / non-behavioural changes

- New firmware file: `ESP32_standalone_electricity_ticker_7_6.ino`
- Version strings bumped from v7.5 to v7.6:
  - `connectToWiFi()` splash: `"Elec. Rate SI v7.5"` → `"Elec. Rate SI v7.6"`
  - `displaySecondaryList()` credit line: `"price ticker v7.5"` → `"price ticker v7.6"`
  - `setup()` debug banner: `"v7.5 (DST + API URL Hardening)"` → `"v7.6 (Gesture-Safe Button + API URL Fix)"`
- Each captured gesture is logged with its measured duration.
- `buttonInterruptFired` is retained for serial-log clarity only; v7.6 no longer
  uses it to reconstruct a gesture.

See `CHANGELOG.md` for full implementation details and verification data.

---

## Highlights of v7.5

### DST Hardening + API URL Date Bounds + Fall-Back Average Fix

Hardening and correctness release built on v7.4: `mktime()` DST handling,
explicit API URL date bounds, a complete sweep of the remaining non-reentrant
`localtime()` call-sites, a real fall-back-day bug in the daily average /
min-max calculation, and two small additions to the fetch path.
Button handling, fee/VAT math, NVS layout, 48-hour scrolling behaviour and
Midnight Bridge logic are unchanged; the v7.3 DST fixes and the v7.4 scheduling
fixes are preserved verbatim.

> Every fix was verified before implementation. Where the original issue
description did not survive verification, the measured result is recorded
in `CHANGELOG.md` instead of the claim.

#### Fix A – `tm_isdst = -1` before `mktime()`

The calendar-day increment (`tm_mday += 1; tm_hour = 12`) was followed by
`mktime()` without resetting `tm_isdst`, so the source day's DST state
leaked into the normalisation. Both sites now set `tm_isdst = -1` first.

*Measured impact:* the ±1 h offset on transition days is real, but the
midday anchor keeps the calendar day — the only part this firmware
consumes — correct either way. So this is defensive hardening rather than
a reachable mis-display in v7.4.

#### Fix B – Explicit `&start=&end=` on today's fetch URL

Today's fetch no longer uses the bare `api_url`; it now requests
`&start=YYYY-MM-DD&end=YYYY-MM-DD` for the current local date, removing
the dependence on server-side caching of the bare endpoint.

*Verified against the live API:* `end` is inclusive, so `start == end`
returns exactly one market day (96 entries normally, 92 on a spring-forward
day). The reported "stale ~2-month-old window" was not reproducible at the
time of writing — the bare endpoint already returned the correct current
day — so this is hardening against a non-deterministic server default.

#### Fix C – `localtime()` → `localtime_r()` (8 call-sites, 7 functions)

`localtime()` returns a pointer to a single shared static `struct tm`, so
every call invalidates the previous result. All eight remaining call-sites
now use `localtime_r()` with a caller-owned buffer:
`findPriceIndexForHour()`, `getHourFromPriceIndex()`, `displaySecondaryList()`
(×2), `scheduleAfterMidnightFailure()`, `handleDataFetching()`, and `loop()`
(×2).

`handleDataFetching()` is the one that mattered: it dereferenced the
`localtime()` result with **no NULL check**, so a failed conversion would
have meant a NULL dereference during the busiest part of the day. It now
tests the `localtime_r()` return value like every other site.

#### Fix D – DST-aware hour-block detection in the daily average

On the DST fall-back day local 02:00 occurs **twice**, so the day holds 25
hour-blocks and 100 quarter-hour entries. The average / min-max loop in
`processJsonData()` detected each new block by comparing `tm_hour` alone, so
the second 02:xx block was skipped entirely: the daily average ran over 24
blocks instead of 25, and if that block held the day's extreme price the
lowest/highest marker landed on the wrong hour and the true extreme was never
flagged. Block detection now compares `tm_hour` **and** `tm_isdst` (via the new
`getDstFlagFromPriceIndex()` helper).

*Verified:* on a simulated 25-hour fall-back day the loop went from 24 blocks
averaged to 25, and a −30.00 minimum that was previously invisible is now
correctly located. The 24-row LCD display still shows the first 02:xx block,
which is correct for a 24-row layout — only the aggregate statistics changed.

#### Fix E – today-fetch no longer spins the loop in a blocking `http.GET()`

Reported as a slow, freezing double-click screen transition. Ruled out first:
the button handler and both display renderers are byte-for-byte identical to
v7.4, and the display hot path benchmarked **22 % faster** in v7.5
(23.9 µs vs 30.6 µs per redraw).

Actual cause: in `fetchAndProcessData()`, the "wrong day" and "JSON parse
failure" branches advanced `nextScheduledFetchTime` only when `fetchTomorrow`
was true — despite the adjacent comment already saying *"Must advance the
schedule to avoid a tight retry loop"*. A today-fetch rejected by the date gate
therefore left the schedule in the past, and `handleDataFetching()` re-entered
on the next loop iteration with another blocking `http.GET()`, indefinitely.

Simulated (today-fetch permanently rejected, 500 ms per GET): 12 fetches and
6000 ms blocked across 12 iterations before; 1 fetch and 500 ms after, with the
next fetch scheduled 588 s out. Fixed with one forward-progress guard in
`handleDataFetching()`, mirroring the v7.4 Fix B guard on the tomorrow path.

The gap is identical in v7.4 and is therefore **not** a v7.5 regression. Fix B
reduces how often it triggers: measured on 2026-10-10, the bare endpoint
returned the previous market day while `&start=&end=` returned the requested one.

#### Fix F2 – the HTTP request is measured rather than guarded

A wall-clock stall guard was considered and **not adopted**: it would have had
to measure `millis()` after `http.GET()` had already returned, so it could not
prevent any blocking while risking the discard of a large but valid response
past an arbitrary cutoff. Instead, every request now prints its actual
duration, so a future freeze is measurable on the serial monitor.

#### Fix G – no fetch is started while the user is pressing the button

The ISR records the wall-clock time of the last physical edge in
`btnEvtLastMs`. `handleDataFetching()` returns early if that edge happened less
than `BUTTON_INTERACTION_GUARD_MS` (800 ms) ago, which covers the full
double-click window plus the 500 ms the firmware waits to confirm it.

The fetch is only deferred, never cancelled — `nextScheduledFetchTime` is
untouched, so the data still arrives, just once the user's hands are off the
button.

### Known issues / not yet fixed (v7.5)

Recorded from post-release testing. **No code change** — neither fix is in this
release. Both issues are intermittent and pre-existing; v7.4 behaved the same
way. Fix G reduces the exposure but does not eliminate it.

**(a) The UI can freeze for a few seconds during a double-click.** The button
path is not at fault: `toggleList()` only flips `currentList` and redraws the
LCD. The freeze is the blocking `http.GET()` in `handleDataFetching()` — while
the loop is inside it, `handleButton()` and `updateLeds()` never run, so the
LED holds its last PWM value. `Config::HTTP_TIMEOUT` (10 s) and
`HTTP_CONNECT_TIMEOUT` (5 s) bound TCP connect and socket reads but not DNS
resolution, which lwIP performs synchronously inside `http.GET()`, so a
multi-second stall is consistent with DNS and would not be addressed by raising
those constants. Fix G prevents a fetch from *starting* within 800 ms of a
button edge but cannot shorten one that is already running.

Intermittent by construction: a fetch is due roughly every 30 minutes in normal
operation, so the exposed window is only the request's own duration. It becomes
more likely in a degraded state, where the retry cadence drops to 10 minutes.
The 10-second auto-return to the primary screen (`resetDisplayToTop()`) does no
network work and therefore never freezes.

Every request already prints its duration, so a freeze is measurable rather than
a matter of inference — a freeze is preceded by:

```text
HTTP GET TODAY rc=200 took 4000 ms
  ^ slow request: loop was blocked for 4000 ms (button edges queued in ISR, applied after)
```

**(b) A double-click can collapse to a single click during a freeze.**
`buttonInterruptFired` is a `volatile bool`, recording *that* an edge occurred
rather than *how many*, so `handleButton()` synthesises exactly one
press+release pair when the loop resumes and two real presses are reported as
one — the screen scrolls instead of switching. A double-click straddling the
end of a freeze fails the same way, as the 500 ms `doubleClickWindow` has by
then expired.

Fixing both means counting edges in the ISR and making the fetch non-blocking.
Neither is a small patch, so both are deferred rather than rushed into this
release.

### Cosmetic / non-behavioural changes

- New firmware file: `ESP32_standalone_electricity_ticker_7_5.ino`
- Version strings bumped from v7.4 to v7.5 in splash/secondary/debug banner:
  - `connectToWiFi()` splash: `"Elec. Rate SI v7.4"` → `"Elec. Rate SI v7.5"`
  - `displaySecondaryList()` credit line: `"price ticker v7.4"` → `"price ticker v7.5"`
  - `setup()` debug banner: `"v7.4 (Fetch Scheduling Fix)"` → `"v7.5 (DST + API URL Hardening)"`
- Inline comments added at each fix site referencing the fix letter.
- The long-press log line now carries the measured press duration.

See `CHANGELOG.md` for full implementation details and verification data.

---

## Highlights of v7.4

### Fetch Scheduling Fix: Button Responsiveness

Bug-fix release that eliminates the afternoon/evening button freeze.
After 14:00 local time, if the Energy-Charts API had not yet published
next-day prices (typical until ~01:00–02:00 UTC = 03:00–04:00 CEST), the
main loop called `fetchAndProcessData(true)` every iteration because
`nextScheduledFetchTime` was never advanced after a failed or rejected
fetch. Each iteration blocked the loop for 5–15 s inside `http.GET()`,
starving `handleButton()`. The display still updated, but the button was
only sampled for ~1 ms per iteration.

No fee/VAT math, NVS layout, API URL construction, DST logic, 48-hour
scrolling behaviour, or Midnight Bridge logic was changed. All four v7.3
DST fixes (A/B/C) and all four v7.2 button/screen fixes (1/2/3/4) are
preserved verbatim.

#### Fix A – Advance `nextScheduledFetchTime` after every failed tomorrow fetch

Every failure/rejection path in `fetchAndProcessData()` now advances
`nextScheduledFetchTime` by 1800 s (30 min) when `fetchTomorrow == true`:

- No-WiFi bail-out: `nextScheduledFetchTime = now + 1800`
- HTTP error (response code ≤ 0): `nextScheduledFetchTime = now + 1800`
- JSON parse error: `nextScheduledFetchTime = now + 1800`
- Data returned but rejected by `processJsonData()`: `nextScheduledFetchTime = now + 1800`

This breaks the tight retry loop that starved `handleButton()` from 14:00
onward.

#### Fix B – Belt-and-suspenders guard in `handleDataFetching()`

After calling `fetchAndProcessData(true)`, if `isTomorrowDataAvailable` is
still false, `nextScheduledFetchTime` is forced to at least `now + 1800`.
This protects against any future refactor that removes the advance from the
helper.

The tomorrow-fetch window is bounded to 14:00–23:00 (`ti->tm_hour >= 14 && ti->tm_hour <= 23`). 
After 23:00 the Midnight Bridge takes over; at most 19 retry attempts are made per day.

#### Fix C – Long-press detector: only honour genuine long presses

The long-press detector in `handleButton()` recorded `buttonPressStartTime`
at RELEASE time, so the "3-second hold" check (`millis() -
buttonPressStartTime >= 3000`) actually measured idle time since the last
release, not press duration. After 3 s of idle the detector fired, set
`longPressDetected = true`, and the NEXT press (even a normal short click)
triggered a forced manual refresh (`nextScheduledFetchTime = now`), blocking
the loop for another 10–15 s.

Fix: the press handler now checks `pressDuration >= longPressThreshold`
before honouring `longPressDetected`. A spurious idle-time flag no longer
hijacks a normal click.

#### Fix D – Button edge interrupt (non-blocking press capture)

A `CHANGE` interrupt on `buttonPin` sets a volatile flag. At the top of
`handleButton()`, if the flag is set and the pin is now released, a synthetic
press+release event is injected into the state machine. This guarantees that
a press occurring during a legitimate 10–15 s HTTP block is not silently
lost.

```cpp
volatile bool buttonInterruptFired = false;

void IRAM_ATTR buttonISR() {
    buttonInterruptFired = true;
}

// In setup():
attachInterrupt(digitalPinToInterrupt(buttonPin), buttonISR, CHANGE);
```
### Cosmetic / non-behavioural changes

- New firmware file: `ESP32_standalone_electricity_ticker_7_4.ino`
- Version strings bumped from v7.3 to v7.4 in splash/secondary/debug banner:
- connectToWiFi() splash: `"Elec. Rate SI v7.3"` → `"Elec. Rate SI v7.4"`
- `displaySecondaryList()` credit line: `"price ticker v7.3"` → `"price ticker v7.4"`
- `setup()` debug banner: `"v7.3 (DST Hardening)"` → `"v7.4 (Fetch Scheduling Fix)"`
- Inline comments added at each fix site referencing Fix A/B/C/D.
- New global flag `volatile bool buttonInterruptFired` (see Fix D).
- See `CHANGELOG.md` for full implementation details.

## Highlights of v7.3

### DST Edge-Case Hardening

Bug-fix release that corrects three DST-related edge cases identified by
static analysis of the v7.2 firmware. No fee/VAT math, NVS layout, button
logic, API scheduling or 48-hour scrolling behaviour was changed.

#### Fix A – Date-validation gate in `processJsonData()` was a no-op

Two consecutive `localtime()` calls were using the same static `struct tm`
buffer. The second call overwrote the first result, so date comparison became
`X == X` (always true). The gate now uses `localtime_r()` with two separate
`struct tm` value variables.

#### Fix B – "Tomorrow" date arithmetic now advances calendar day safely

Computing tomorrow with `+24*3600` could produce the wrong local date on
spring-forward Saturday evening near DST transition. The code now advances
`tm_mday += 1`, anchors at midday (`tm_hour = 12`), and re-normalises using
`mktime()` before formatting the API `start=YYYY-MM-DD` date.

#### Fix C – Fall-back day (25-hour) averaging includes repeated 02:xx block

The old `hour = 0..23` scan missed the second 02:xx block on DST fall-back
days (100 quarter-hour entries), slightly skewing average and min/max
identification. The scan now iterates the `unix_seconds` array by index and
processes each hour-block start, including repeated local-hour blocks.

#### Cosmetic / non-behavioural changes

- New firmware file: `ESP32_standalone_electricity_ticker_7_3.ino`
- Version strings bumped from v7.2 to v7.3 in splash/secondary/debug banner
- Inline comments added at each fix site referencing Fix A/B/C

See `CHANGELOG.md` for full implementation details.

---

## Highlights of v7.2

### Button Robustness & Screen-Control Fixes

Bug-fix release that resolves the three control glitches reported for the v7.1
firmware on the Seeed XIAO ESP32‑C3: the primary screen looked unscrollable,
double-clicks failed to switch to the secondary status screen, and the end-of-day
behaviour (no tomorrow data yet) was unstable around 22:00–23:59. No fee/VAT
math, NVS layout, API scheduling or 48-hour scrolling behaviour was changed.

#### Fix 1 – Primary-screen scroll now works at any hour of the day

Two compounding bugs in `displayPrimaryList()` and `displayPriceRow()` were
cancelling each other out and made single-click scrolling look dead:

- `displayPriceRow()` only blanked past hours while `currentHour < 22`. After
  22:00 the screen could repaint already-finished morning hours, so as soon
  as the user scrolled forward the new "top" hour was visually over-written
  by the previous morning's data. The guard is now
  `if (localHourIndex < currentHour) blank();`, so past hours of today are
  hidden at every hour of the day.
- `displayPrimaryList()` contained an override
  `if (currentHour >= 21 && timeOffsetHours > 0) displayStartHourOffset = 21 + timeOffsetHours;`
  which pinned the top row at 21:00 + offset from 21:00 onward. At 22:15
  every click computed start = 22+offset, was then clamped to 21+offset, and
  the user saw no movement. The override is no longer needed and has been
  removed.

#### Fix 2 – Double-click on the secondary screen now fires reliably

The double-click path itself was correct; it was being starved by a false
"Long press detected!" message that fired immediately after every reset.
On the ESP32-C3 the button pin (`INPUT_PULLUP`) floats HIGH for a few
seconds during boot, while `buttonPressStartTime` is initialised to `0`.
As soon as `millis()` crossed the 3 s threshold, the long-press detector
tripped on a phantom 3-second hold, cleared the LCD to
"Long press detected! / Release to refresh", and from then on the user
could not see any prices to click on (single- and double-click recognisers
both still ran, but their visible effect was hidden behind the long-press
splash).

Fix: a new `bool buttonEverReleased` is set to `true` the first time the
pin is observed LOW after boot, and the long-press detector is gated on
it: `if (buttonState == LOW && !longPressDetected && buttonEverReleased)`.
The detector refuses to fire until the user (or the power-rail noise) has
released the button at least once. The v7.1 button logic (debounce, 3 s
long-press threshold, 500 ms double-click window, TTP223 timing) is
otherwise preserved verbatim.

#### Fix 3 – End-of-day scroll is now stable when no tomorrow data is available

`advanceDisplayOffset()` contained a hack
`if (allowedAhead < 2 && currentHour >= 21 && !isTomorrowDataAvailable) allowedAhead = 2;`
which at 22:00 (with no tomorrow data) let the user click past hour 23 into
"24:00 / 25:00", where `displayStartHourOffset` wrapped back to 0 and filled
the LCD with the now-unblanked past-hour rows. The hack has been removed;
the natural cap is now sufficient:

- **22:00** → can step 22 → 23, then wraps back to current
- **23:00** → cannot step forward at all

No wrap to 00:00 of the previous day is reachable any more.

#### Fix 4 (bonus) – Auto-return timer now resets on every click

`lastButtonActivity` / `autoScrollExecuted` were only updated in the
primary-list branches of `advanceDisplayOffset()`. Scrolling the secondary
status page therefore did not push the 10 s auto-return-to-top timeout
forward. The two resets are now at the top of `advanceDisplayOffset()` so
every successful click (single, double, or long-press-release) refreshes
the timer regardless of which list is showing.

#### Cosmetic / non-behavioural changes

- Filename and three user-visible version strings bumped to v7.2:
  - `connectToWiFi()` splash: `"Elec. Rate SI v7.1"` → `"v7.2"`
  - `displaySecondaryList()` credit line: `"price ticker v7.1"` → `"v7.2"`
  - `setup()` debug banner: `"v7.1 (Neg Price Fee)"` → `"v7.2 (Button Robustness)"`
- Inline comments added at each fix site explaining what v7.1 did wrong,
  so future maintainers do not re-introduce the overrides.
- New global flag `bool buttonEverReleased` (see Fix 2).

See `CHANGELOG.md` for full implementation details.

---

## Highlights of v7.1

### Negative Price Provider Fee

Added a separate provider fee for negative spot prices via a new constant
`NEG_PRICE_COMPANY_FEE_PERCENTAGE`. Positive and negative market prices now
use independent fee multipliers, correctly modelling contracts where the
provider's fee structure differs between the two cases.

**Price calculation:**

| Market price | Formula |
|---|---|
| Positive (`raw >= 0`) | `raw × (1 + POWER_COMPANY_FEE_PERCENTAGE/100) × (1 + VAT_PERCENTAGE/100)` |
| Negative (`raw < 0`) | `raw × (1 - NEG_PRICE_COMPANY_FEE_PERCENTAGE/100) × (1 + VAT_PERCENTAGE/100)` |

VAT is applied to both, consistent with net billing where VAT is calculated
on the monthly net sum (linear equivalence applies).

All 5 fee calculation sites updated: `updateLeds()`, `format15MinPrice()`,
`displayPriceRow()`, `displaySecondaryList()` (daily average), and version strings.

See `CHANGELOG.md` for full implementation details.

---

## Highlights of v7.0

### MAJOR UPGRADE: Rolling 48-Hour Logic & Midnight Bridge

This version is the **"Golden Build"** for this hardware. It represents the culmination of hardware stability fixes from v6.2.4 combined with revolutionary new 48-hour price prediction capabilities.

#### 1. The Midnight Bridge (Rollover Logic)

The most complex part of electricity tickers is handling the midnight transition. This code now correctly detects the moment the local clock moves from 23:59:59 to 00:00:00.

**The Swap:** Instead of waiting for a slow API call at midnight (which usually fails because the server hasn't updated yet), the code instantly promotes the "Tomorrow" buffer to become "Today" data.

**The NVS Update:** The code correctly serializes the new "Today" data and saves it to NVS immediately after the swap. This ensures that if power cuts at 00:05 AM, the device reboots with the correct data already loaded.

#### 2. Dual-Buffer NVS System

The ticker now stores "Today" and "Tomorrow" data independently in NVS:

- **Today buffer (`doc`)**: Contains the current day's price data
- **Tomorrow buffer (`docTomorrow`)**: Contains the next day's price data
- **NVS keys**: `data_prc`/`data_day`/`data_mon`/`data_year` for today, `data_prc_t`/`data_store_t` for tomorrow

#### 3. Smart Fetching & API URL

The logic for fetching tomorrow's data is implemented correctly:

- **URL Construction**: Adding `&start=YYYY-MM-DD` dynamically after 14:00 (2 PM) queries the Energy-Charts API for the next day
- **Validation**: In `processJsonData()`, the code compares the timestamp in the JSON against the target date, preventing the "Tomorrow" buffer from being filled with "Today's" data if the API is lagging

#### 4. Seamless 48H Scrolling

If next-day data is available, the button allows scrolling up to **47 hours ahead**:

- **Visual Distinction**: Using `HH:>>` for tomorrow's hours prevents the user from confusing a cheap price "tomorrow" with a cheap price "today"
- **Index Safety**: The code correctly uses `lowestPriceIndexTomorrow` and `highestPriceIndexTomorrow` when the display is in the "tomorrow" range, ensuring the Min/Max icons appear on the correct 15-minute segments

#### 5. Hardware Stability (Inherited from v6.2.4)

All v6.2.4 hardware stability fixes are preserved:

- **Refresh Logic**: "State-Based" refresh ensures the display updates exactly at 00, 15, 30, and 45 minutes past the hour, even if the CPU is busy with a background fetch
- **LED Indicators**: White LED for low price and Built-in LED for connectivity remain pinned to the actual current price, even when the user is scrolling through future data on the screen

#### Final "Sanity Check" Verdict

**Status:** Verified. The code is safe to deploy. The transition from 15-minute intervals to the midnight rollover is now seamless. The "1 AM fetch gap" that plagues most electricity tickers has been successfully bypassed.

---

## Highlights of v6.2.4

- **BUG FIX:** Exact-boundary display refresh bug
- Problem: At the exact top of the hour (e.g., 20:00:00), the display automatically refreshed but showed the PREVIOUS hour's data (19:00). This happened because the "next-boundary" rounding logic in findCurrentPriceIndex() incorrectly excluded the current interval if the time was exactly on the boundary.
- Fix: Simplified findCurrentPriceIndex() to use a robust "last entry <= now" comparison. This ensures the display transitions to the new hour instantaneously at XX:00:00.

## Highlights of v6.2.3

- **BUG FIX**: State-based display refresh logic
- Problem: Screen would occasionally fail to update if the ESP32 was busy (fetching data or reconnecting WiFi) during the exact 00/15/30/45 minute mark.
- Fix: Switched from "Event-Based" (refresh only AT minute X) to "State-Based" (refresh IF current time != last refresh time). This ensures the screen updates immediately even if the device was busy during the transition.

## Highlights of v6.2.2

- **BUG FIX**: Display blank lines issue
- Problem: Sometimes rows 0 and 1 (current 15-min prices and current hour) were blank
- Cause: The "hour suppression" logic was hiding the current hour unexpectedly
- Fix:
  - Row 1 (current hour) now ALWAYS shows - suppression logic only applies to rows 2-3
  - Row 0 (15-min details) also always shows for the current hour

## Highlights of v6.2.1

- **BUG FIX**: Fixed `findCurrentPriceIndex()` to return the correct current interval.
- Problem: At 17:57, it returned index for 18:00 instead of 17:45, causing display to show hour 18 instead of hour 17.
- Fix: Now calculates next 15-minute boundary and finds the last entry before that boundary.

## Highlights of v6.2.0

- **CRITICAL FIX**: DST (Daylight Saving Time) handling is now fully fixed for all days.
- Previously, the code assumed every day has exactly 96 price entries (24h × 4). This caused incorrect price display on DST switch days:
  - Spring forward (March): Only 92 entries → wrong prices displayed
  - Fall back (October): 100 entries → wrong prices displayed
- **Solution**: All price lookups now use timestamp-based searching through the `unix_seconds` array instead of arithmetic calculation (`hourIndex * 4`).
- New functions: `findPriceIndexForHour()`, `findCurrentPriceIndex()`, `getHourFromPriceIndex()`
- Updated functions: `getHourlyAverage()`, `display15MinuteDetails()`, `displayPriceRow()`, `displayPrimaryList()`, `updateLeds()`
- The ticker now works correctly on all days, including DST switch days, with no manual intervention.
- **Future-proof**: If EU cancels DST, only the `TZ_CET_CEST` string needs updating (one line of code).

## Previous firmware

- **Version:** 6.1.2
- **Release date:** 2026-03-11
- **Target MCU:** Seeed XIAO ESP32‑C3

## Highlights of v6.1.2

- Fix: restore proper white LED price indicator behavior on ESP32 by avoiding mixing PWM (`analogWrite`) and `digitalWrite` on the same pin.
- Fix: LED is now truly off when backlight/LED gating turns it off (no more "dim glow").

## Earlier firmware

- **Version:** 6.1.1 (2026-03-07) – Daily low/high marker includes negative and zero prices
- **Version:** 6.1.0 (2026-01-30) – Midnight fetch and market day detection fixes
- **Version:** 6.0.0 (2026-01-27) – NVS storage and daily fetch

For full details, see:

- [CHANGELOG.md](./CHANGELOG.md)
- [README.md](./README.md)
---
