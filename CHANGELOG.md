# Changelog

All notable changes to this project are documented here.

## v7.6 – Gesture-Safe Button + Single-Market-Day Tomorrow Fetch (2026-10-10)

Two changes built on v7.5: the button path is rebuilt so a touch gesture survives
a blocking HTTP fetch, and the tomorrow fetch asks for exactly one market day
instead of an open-ended window.

Fee/VAT math, NVS layout, 48-hour scrolling behaviour, Midnight Bridge logic and
all DST logic are unchanged. All v7.5 fixes (A–F2), all v7.4 scheduling fixes
(A, B, E, G) and all v7.2 button/screen fixes (1, 2, 4) are preserved.

### Fix H – Tomorrow fetch now sends an end date

**Symptom**

The tomorrow fetch requested `&start=<tomorrow>` with no `&end=`. On the
Energy-Charts API `end` is inclusive and defaults to the end of the available
window, so the request returned several market days.

**Effect**

`processJsonData()` keeps only entries whose local day matches the target day, so
the surplus was downloaded, parsed and thrown away. The cost landed on the
slowest path in the firmware — the multi-second blocking `http.GET()` — at the
hours when the button is most likely to be in use, which is exactly the window in
which v7.5's known issue (a) freezes the UI.

**Fix**

`fetchAndProcessData()` now appends `&start=<date>&end=<date>` for the tomorrow
fetch as well, with `start == end`, the same single-market-day contract v7.5
Fix B established for the today fetch.

### Fix I – Button polarity declared once as `BUTTON_ACTIVE_HIGH`

**Problem**

`HARDWARE_WIRING_DIAGRAM.md` documents two interchangeable inputs on D2 (GPIO4)
that are inverted with respect to each other:

| Input | Idle level | Actuated level |
|---|---|---|
| Mechanical pushbutton wired to GND | HIGH | LOW (active LOW) |
| TTP223 capacitive touch pad | LOW | HIGH (active HIGH) |

v7.5 sampled the pin as `!digitalRead(buttonPin)`, which is correct only for the
active-HIGH TTP223, while the surrounding comments described the pin as
"active-LOW" and called HIGH "released". The code and its documentation disagreed
about which edge is a press — the ambiguity the v7.2 `buttonEverReleased`
workaround and v7.4 Fix C were compensating for.

**Fix**

```cpp
static const bool BUTTON_ACTIVE_HIGH = true;   // true = TTP223, false = mechanical

inline bool buttonIsPressed() {
    return digitalRead(buttonPin) == (BUTTON_ACTIVE_HIGH ? HIGH : LOW);
}

// Maps the physical polarity onto the Arduino debounce convention
// (LOW == pressed, HIGH == released) used inside handleButton().
inline int buttonReadingToState() {
    return buttonIsPressed() ? LOW : HIGH;
}
```

`buttonIsPressed()` is now the only place the raw pin level is interpreted; the
ISR and the debounce state machine both work in "pressed / not pressed" terms.
The default matches the documented TTP223 hardware. Set it to `false` for a
mechanical pushbutton.

### Fix J – Gestures are captured by the ISR, not reconstructed by the loop

**Root cause of the v7.5 known issue (b)**

v7.4 Fix D recorded `buttonInterruptFired` — a single `volatile bool` meaning
"an edge happened" — and `handleButton()` then rebuilt a press+release pair from
whatever the pin was doing when the loop finally got back to it. A gesture that
completes entirely inside a blocking `http.GET()` leaves the pin back at its idle
level, so the state machine saw no press edge, no release edge and no duration:
the synthetic event had a zero duration and the gesture was misclassified or
dropped. Two real presses during a freeze were reported as one.

**Fix**

`attachInterrupt(digitalPinToInterrupt(buttonPin), buttonISR, CHANGE)` now feeds
a gesture recorder:

```cpp
void IRAM_ATTR buttonISR() {
    unsigned long now = millis();
    btnEvtLastMs = now;                       // FIX G reads this, before any debounce test

    if (buttonIsPressed()) {
        if (now - btnEdgeLastMs >= debounceDelay) {
            btnEdgeLastMs   = now;
            btnTouchStartMs = now;            // touch opened
        }
    } else {
        if (btnTouchStartMs != 0) {
            if (now - btnTouchStartMs < GESTURE_MIN_MS) return;  // bounce: stay open
            btnEdgeLastMs = now;
            uint8_t next = (uint8_t)((btnGestureWrite + 1) % GESTURE_SLOTS);
            if (next != btnGestureRead) {     // FIFO not full
                btnGestureStart[btnGestureWrite] = btnTouchStartMs;
                btnGestureEnd[btnGestureWrite]    = now;
                btnGestureWrite = next;
            }
            btnTouchStartMs = 0;
        }
    }
    buttonInterruptFired = true;
}
```

Debounce rules, chosen so a real gesture is never lost:

- A touch may only **open** if `debounceDelay` (50 ms) has passed since the last
  accepted edge, so contact bounce cannot open a second gesture.
- A release only **closes** a gesture if the touch lasted at least
  `GESTURE_MIN_MS` (40 ms). A shorter release is treated as bounce and the
  gesture is left open for the real release — rejecting the edge outright would
  leave the gesture open forever and lose it.
- If the FIFO is full the newest gesture is dropped rather than overwriting an
  unread one, so the queue never duplicates or reorders a gesture. 8 slots hold
  7 gestures, which is more than a user can produce during one fetch.

`handleButton()` drains the FIFO, and the classifier is unchanged apart from
being fed measured durations:

```cpp
processButtonPress(pressDuration, endMs, pressDuration >= longPressThreshold);
```

Both `pressDuration` and `clickTime` come from the ISR's own timestamps. That is
what keeps the 500 ms `doubleClickWindow` anchored to real gesture timing:
measuring the gap with `millis()` would compare the moment the loop got round to
draining, so two clicks four seconds apart during a long fetch would look like a
double-click.

Before each gesture is judged, a pending single click whose gap to the next
gesture exceeds `doubleClickWindow` is confirmed first, so two clicks made seconds
apart during a blocked fetch produce two advances instead of collapsing into one
pending click.

**v7.4 Fix C is now structural rather than defensive**

`longHeld` is derived from the measured duration itself, so the flag can never be
set by idle time and can never hijack a short click. The old "duration ≥ threshold
but `longPressDetected` was not set" fall-through case cannot occur.

**What the debounce state machine is still for**

`buttonPressStartTime`, `longPressDetected` and `buttonEverReleased` now drive
only the live LCD "Long press detected! / Release to refresh" feedback while a
finger is still on the pad. They no longer classify gestures, so they can no
longer lose them. The v7.2 boot-noise guard is retained: a pad held from reset
produces no `CHANGE` edge, so it yields neither feedback nor a gesture.

### Verification

`http.GET()` remains synchronous, so the freeze itself cannot be reproduced away
from a board — but the gesture path no longer depends on the loop being free, so
it can be exercised by driving `buttonISR()`, `processButtonPress()` and
`handleButton()` directly. `_v76_sim.py` mirrors those three functions and runs
them against a virtual `millis()` timeline, using the firmware's own constants
(`longPressThreshold` 3000 ms, `doubleClickWindow` 500 ms, `debounceDelay` 50 ms,
`GESTURE_MIN_MS` 40 ms, `GESTURE_SLOTS` 8). "Blocked fetch" means the loop calls
`handleButton()` only at 1000 ms and 16000 ms while every press and release
happens in between.

| Scenario | Result |
|---|---|
| Single click, idle loop | `SINGLE` |
| Double click | `DOUBLE` |
| Long press (3.5 s hold) | live LCD feedback at 3100 ms, then `LONG` (3500 ms) |
| 2.5 s press — below threshold | `SINGLE`, never `LONG` |
| Click **during** a 12 s blocked fetch | `SINGLE` (lost in v7.5) |
| Long press **during** a blocked fetch | `LONG` with the true 3500 ms duration |
| Two clicks 4 s apart during a blocked fetch | `SINGLE` + `SINGLE` |
| Double click during a blocked fetch | `DOUBLE` |
| 20 ms bounce tap | rejected — no gesture |
| Bouncy press (on/off/on within 20 ms, real release at 200 ms) | exactly one `SINGLE` |
| 100 s idle, no press | nothing — no phantom long press |
| Pad held from boot, never released | nothing |
| Pad held from boot, released at 5000 ms | nothing |
| 7 rapid clicks during a blocked fetch | `DOUBLE`, `DOUBLE`, `DOUBLE`, `SINGLE` |
| 10 rapid clicks during a blocked fetch | same — FIFO caps at 7 gestures, 3 dropped |

Static checks on the `.ino`: balanced `{}`/`()`/`[]`, no leftover v7.5 button
machinery (`btnEvtPending`, `btnEvtPressed`, `btnPressStartMs`, `btnReleaseMs`,
synthetic-event injection, `buttonInterruptFired &&` gating), and exactly one
raw `digitalRead(buttonPin)` — inside `buttonIsPressed()`.

**Compiled and flashed:** the sketch builds for the Seeed XIAO ESP32C3
(`esp32:esp32:XIAO_ESP32C3`, ESP32 Arduino core 3.3.12) and has been uploaded to
the board and run. First run on the device:

```
[DEBUG] Starting Dynamic Electricity Ticker v7.6 (Gesture-Safe Button + API URL Fix) - DST-SAFE
[DEBUG] Button interrupt attached on GPIO 4 (active-HIGH: TTP223 touch pad)
[DEBUG] Double-click detected - toggling list        (x4)
[DEBUG] Long press threshold reached - waiting for release
[DEBUG] Long press detected - Forcing manual data refresh (measured 3347 ms)
```

Two double-clicks 4.4 s apart and two more 5.2 s apart each registered as their
own double-click, so the 500 ms window is anchored to the gesture times recorded
by the ISR and not to the moment the loop got round to draining them. The long
press showed the live feedback at the 3000 ms threshold and was then classified
from the measured 3347 ms duration.

**Still to be timed on the touch pad:** a click made during a blocked fetch.
`DEBUG_LEVEL` is 2, which compiles out the level-3 `Gesture captured (<n> ms)`
and `Single click confirmed` lines; raise it to 3 to watch the ISR-recorded
durations directly.

### v7.5 known issues — status after v7.6

- **(b) "A double-click can collapse to a single click during a freeze" — fixed**
  by Fix J. The FIFO carries every gesture; nothing is reconstructed.
- **(a) "The UI can freeze for a few seconds during a double-click" — unchanged
  as a freeze.** `http.GET()` is still synchronous and the display still cannot
  redraw while a request is in flight. What changed is the cost: the freeze no
  longer swallows the gesture. Fix G still prevents a fetch from *starting*
  within 800 ms of a button edge. Removing the freeze outright still requires
  porting the fetch to the asynchronous `esp_http_client` API.

### Cosmetic / non-behavioural changes

- New firmware file: `ESP32_standalone_electricity_ticker_7_6.ino`
- Version strings bumped from v7.5 to v7.6:
  - `connectToWiFi()` splash: `"Elec. Rate SI v7.5"` → `"Elec. Rate SI v7.6"`
  - `displaySecondaryList()` credit line: `"price ticker v7.5"` → `"price ticker v7.6"`
  - `setup()` debug banner: `"v7.5 (DST + API URL Hardening)"` → `"v7.6 (Gesture-Safe Button + API URL Fix)"`
- Each captured gesture is logged with its measured duration, so a borderline
  gesture is diagnosable from the serial log alone.
- `buttonInterruptFired` is retained for serial-log clarity only; v7.6 no longer
  uses it to reconstruct a gesture.

### Files changed

| File | Change |
|---|---|
| `ESP32_standalone_electricity_ticker_7_6.ino` | v7.5 base with Fixes H, I and J |
| `VERSION.md` | Current firmware updated to v7.6; v7.6 highlights section added |
| `CHANGELOG.md` | v7.6 section added at top |

---

## v7.5 – DST Hardening + API URL Date Bounds + Fall-Back Average Fix (2026-10-10)

Hardening and correctness release built on v7.4. Button handling, fee/VAT
math, NVS layout, 48-hour scrolling behaviour and Midnight Bridge logic are
unchanged; all v7.4 scheduling fixes and all v7.3 DST fixes are preserved
verbatim.

Every fix below was verified before implementation rather than applied on
faith; where the original issue description did not survive verification, the
measured result is stated instead of the claim.

### Fix A – Set `tm_isdst = -1` before `mktime()` (2 sites)

`fetchAndProcessData()` and `processJsonData()` advance the calendar day with
`tm_mday += 1; tm_hour = 12` and then call `mktime()` without resetting
`tm_isdst`, so the source day's DST state leaks into the normalisation. Both
sites now set `tm_isdst = -1` first, letting the C library determine the
correct offset for the target day.

*Measured impact:* the ±1 h offset is real, but the midday anchor keeps the
calendar day — the only part this firmware consumes — correct either way. This
is defensive hardening rather than a reachable mis-display in v7.4.

### Fix B – Explicit `&start=&end=` on today's fetch URL

Today's fetch no longer uses the bare `api_url`; it requests
`&start=YYYY-MM-DD&end=YYYY-MM-DD` for the current local date, removing the
dependence on server-side caching of the bare endpoint.

*Verified against the live API:* `end` is **inclusive**, so `start == end`
returns exactly one market day (96 entries normally, 92 on a spring-forward
day, 100 on a fall-back day) and the window is interpreted in local exchange
time. The reported stale two-month-old window did not reproduce at the time of
writing, so this is hardening against a non-deterministic server default rather
than a fix for an observed bug.

### Fix C – `localtime()` → `localtime_r()` (8 call-sites, 7 functions)

`localtime()` returns a pointer to a single shared static `struct tm`, so every
call invalidates the previous result. All eight remaining call-sites now use
`localtime_r()` with a caller-owned buffer: `findPriceIndexForHour()`,
`getHourFromPriceIndex()`, `displaySecondaryList()` (×2),
`scheduleAfterMidnightFailure()`, `handleDataFetching()`, and `loop()` (×2).
`getDstFlagFromPriceIndex()`, added by Fix D, also uses `localtime_r()`.

`handleDataFetching()` is the one that mattered — it dereferenced the
`localtime()` result with **no NULL check**, so a failed conversion would have
been a NULL dereference during the busiest part of the day.

### Fix D – DST-aware hour-block detection in the daily average (real bug)

On the DST fall-back day local 02:00 occurs **twice**, so the day holds 25
hour-blocks and 100 quarter-hour entries. The average / min-max loop in
`processJsonData()` detected each new block by comparing `tm_hour` alone, so
the second 02:xx block was skipped entirely: the daily average ran over 24
blocks instead of 25, and if that block held the day's extreme price the
low/high marker landed on the wrong hour and the true extreme was never
flagged. Block detection now compares `tm_hour` **and** `tm_isdst` via the new
`getDstFlagFromPriceIndex()` helper.

*Verified:* on a simulated 25-hour fall-back day the loop went from 24 blocks
averaged to 25, and a −30.00 minimum that was previously invisible is
correctly located. The 24-row LCD display still shows the first 02:xx block,
which is correct for a 24-row layout — only the aggregate statistics changed.

### Fix E – today-fetch no longer spins the loop in a blocking `http.GET()`

The "API returned the wrong day" and "JSON parse failure" branches in
`fetchAndProcessData()` advanced `nextScheduledFetchTime` only when
`fetchTomorrow` was true. When today's fetch was rejected by the date gate the
schedule stayed in the past, so `handleDataFetching()` re-entered on the next
iteration and issued another blocking HTTP request — forever.

*Measured:* 12 loop iterations → 12 fetches, 6000 ms blocked before; 1 fetch,
500 ms blocked after, with the next fetch scheduled 588 s out. Fixed with a
single forward-progress guard in `handleDataFetching()`, mirroring the v7.4 Fix
B guard on the tomorrow path.

*Note:* the gap is identical in v7.4 and is therefore **not** a v7.5
regression. Fix B reduces how often it triggers, because the bare endpoint was
measured serving the previous market day while `&start=&end=` returns the
requested one.

### Fix F2 – the HTTP request is measured rather than guarded

A wall-clock stall guard was considered and **not adopted**. It would have had
to measure `millis()` *after* `http.GET()` had already returned, so it could
not prevent a single millisecond of blocking — the loop was already stuck by
the time it ran — while adding a real risk of discarding a large but valid
response past an arbitrary 16 s cutoff. Instead, every `http.GET()` now
prints its actual duration, so a future freeze is measurable on the serial
monitor instead of a matter of inference.

### Fix G – no fetch is *started* while the user is pressing the button

The ISR records the wall-clock time of the last physical edge in
`btnEvtLastMs`. `handleDataFetching()` returns early if that edge happened less
than `BUTTON_INTERACTION_GUARD_MS` (800 ms) ago, which covers the full
double-click window plus the 500 ms the firmware waits to confirm it.

The fetch is only *deferred*, never cancelled: `nextScheduledFetchTime` is
untouched, so the data still arrives, just once the user's hands are off the
button.

### Cosmetic / non-behavioural changes

- Version strings bumped from v7.4 to v7.5:
  - `connectToWiFi()` splash: `"Elec. Rate SI v7.4"` → `"v7.5"`
  - `displaySecondaryList()` credit line: `"price ticker v7.4"` → `"v7.5"`
  - `setup()` debug banner: `"v7.4 (Fetch Scheduling Fix)"` → `"v7.5 (DST + API URL Hardening)"`
- The long-press log line now carries the measured press duration, so a
  borderline gesture is diagnosable from the serial log alone.

### Known limitation (not fixed — architectural)

`http.GET()` remains synchronous. On this single-core ESP32-C3 it cannot be
moved to another core, so while a request is in flight the display cannot
redraw and the LED animation is paused. Fix G makes a fetch/click collision
much less likely; it does not make an in-progress fetch non-blocking.
Eliminating the freeze entirely requires porting the fetch to the asynchronous
`esp_http_client` API with callbacks — a substantial rewrite of
`fetchAndProcessData()` that was deliberately **not** undertaken here.

### Files changed

| File | Change |
|---|---|
| `ESP32_standalone_electricity_ticker_7_5.ino` | v7.4 base with Fixes A–E, F2 and G |
| `README.md` | Version references updated to v7.5; v7.5 highlights section added |
| `CHANGELOG.md` | v7.5 section added at top |
| `VERSION.md` | Current firmware updated to v7.5; v7.5 highlights section added |

---

## v7.4 – Fetch Scheduling Fix: Button Responsiveness (2026-10-04)

**Summary**

Bug-fix release that eliminates the afternoon/evening button freeze.
The root cause was a tight retry loop in the tomorrow-data fetch: after
14:00 local time, if the Energy-Charts API had not yet published next-day
prices (typical until ~01:00–02:00 UTC = 03:00–04:00 CEST), the main loop
called fetchAndProcessData(true) every iteration because
nextScheduledFetchTime was never advanced after a failed or rejected
fetch. Each iteration blocked the loop for 5–15 s inside http.GET(),
starving handleButton(). The display still updated (it runs after the
HTTP block), but the button was only sampled for ~1 ms per iteration.

No fee/VAT math, NVS layout, API URL construction, DST logic, 48-hour
scrolling behaviour, or Midnight Bridge logic was changed. All four v7.3
DST fixes and all four v7.2 button/screen fixes are preserved verbatim.

### Fix A – Advance nextScheduledFetchTime after every failed tomorrow fetch

**Symptom**

Button unresponsive from ~14:00 until the API publishes tomorrow's data
(typically 03:00–04:00 CEST). Display continues to show correct prices.
The freeze "reverts to normal" once isTomorrowDataAvailable flips true.

**Root cause**

In fetchAndProcessData(), the failure paths (HTTP error, JSON parse
error, data rejected by processJsonData()) only advanced
nextScheduledFetchTime when fetchTomorrow == false. For tomorrow
fetches the schedule was never advanced, so the next loop iteration
immediately retried, creating a tight loop that blocked the main loop
for 10–15 s per attempt.

**Fix**

Every failure/rejection path in fetchAndProcessData() now advances
nextScheduledFetchTime by 1800 s (30 min) when fetchTomorrow == true:

No-WiFi bail-out: nextScheduledFetchTime = now + 1800
HTTP error (response code ≤ 0): nextScheduledFetchTime = now + 1800
JSON parse error: nextScheduledFetchTime = now + 1800
Data returned but rejected by processJsonData(): nextScheduledFetchTime = now + 1800

### Fix B – Belt-and-suspenders guard in `handleDataFetching()`

After calling fetchAndProcessData(true), if isTomorrowDataAvailable
is still false, nextScheduledFetchTime is forced to at least
now + 1800. This protects against any future refactor that removes
the advance from the helper.

The tomorrow-fetch window is now capped at 23:00 local time: the condition 
in handleDataFetching() is ti->tm_hour >= 14 && ti->tm_hour <= 23. After 
23:00 no further tomorrow HTTP calls are issued; the Midnight Bridge 
in loop() handles the 00:00 rollover instead. This caps the worst-case retry count 
at 19 attempts (14:00 → 23:00, every 30 min) within a single calendar day.

### Fix C – Long-press detector: only honour genuine long presses

**Symptom**

After 3 s of idle (no button press), the long-press detector fired
because buttonPressStartTime records the time of the last release,
not the start of a press. The "3-second hold" check
(millis() - buttonPressStartTime >= 3000) actually measured idle time.
After it fired, longPressDetected = true persisted, and the next
press (even a normal short click) triggered a forced manual refresh
(nextScheduledFetchTime = now), blocking the loop for another 10–15 s.

**Fix**

The press handler now checks pressDuration >= longPressThreshold before
honouring longPressDetected:

```cpp
if (longPressDetected && pressDuration >= longPressThreshold) {
    // genuine long press → manual refresh
} else if (pressDuration < longPressThreshold) {
    // normal short press → single/double click
}
```

A spurious idle-time flag no longer hijacks a normal click.

### Fix D – Button edge interrupt (non-blocking press capture)

A CHANGE interrupt on buttonPin sets a volatile flag. At the top of
handleButton(), if the flag is set and the pin is now released, a
synthetic press+release event is injected into the state machine. This
guarantees that a press occurring during a legitimate 10–15 s HTTP block
is not silently lost.

```cpp
volatile bool buttonInterruptFired = false;

void IRAM_ATTR buttonISR() {
    buttonInterruptFired = true;
}

// In setup():
attachInterrupt(digitalPinToInterrupt(buttonPin), buttonISR, CHANGE);
```

### Other changes (cosmetic / non-behavioural)

- Filename bumped to v7.4: ESP32_standalone_electricity_ticker_7_4.ino
- Three user-visible version strings bumped from v7.3 to v7.4:
- connectToWiFi() splash: "Elec. Rate SI v7.3" → "v7.4"
- displaySecondaryList() credit line: "price ticker v7.3" → "v7.4"
- setup() debug banner: "v7.3 (DST Hardening)" → "v7.4 (Fetch Scheduling Fix)"
- Inline comments added at each fix site referencing Fix A/B/C/D.

### Files changed
| File	| Change |
|---|---|
| `ESP32_standalone_electricity_ticker_7_4.ino`	| New file — v7.3 base with Fixes A, B, C, D applied |
| `README.md`	| Version references updated to v7.4; v7.4 highlights section added |
| `CHANGELOG.md`	| v7.4 section added at top |
| `VERSION.md` |	Current firmware updated to v7.4; v7.4 highlights section added |

---

## v7.3 - DST Edge-Case Hardening (2026-10-03)

**Summary**

Bug-fix release that corrects three DST-related edge cases identified by
static analysis of the v7.2 firmware. No fee/VAT math, NVS layout, button
logic, API scheduling or 48-hour scrolling behaviour was changed. All four
v7.2 button/screen-control fixes are preserved verbatim.

### Fix A – Date-validation gate in `processJsonData()` was a no-op (critical)

**Symptom**

Stale price data from a previous day, or an API payload for the wrong day,
could be silently accepted and displayed as if it were today's data.

**Root cause**

C's `localtime()` returns a pointer to one shared static `struct tm`. In
`processJsonData()` two consecutive calls were made:

```cpp
struct tm* lastDataTm  = localtime(&lastDataTime);   // ← static buffer
struct tm* targetDayTm = localtime(&targetTime);     // ← same static buffer, overwritten
bool sameDate = (lastDataTm->tm_mday == targetDayTm->tm_mday && ...
```

The second call overwrites the buffer that `lastDataTm` also points to.
Both pointers then point to the same data, so the comparison is always
`X == X` (unconditionally true). The date-validation gate was a no-op.

**Fix**

Replaced both calls with `localtime_r()` into two separate `struct tm`
value variables:

```cpp
struct tm lastDataTm;
localtime_r(&lastDataTime, &lastDataTm);
// ...
struct tm targetDayTm;
localtime_r(&targetTime, &targetDayTm);
bool sameDate = (lastDataTm.tm_mday == targetDayTm.tm_mday && ...
```

### Fix B – "Tomorrow" URL date wrong on spring-forward Saturday evening (moderate)

**Symptom**

On the spring-forward Saturday after ~23:00 CET, the tomorrow-fetch URL
contained the date of the day after tomorrow (Monday) instead of tomorrow
(Sunday), causing the API to return no data or the wrong day's data.

**Root cause**

Both the URL construction in `fetchAndProcessData()` and the target-date
calculation in `processJsonData()` computed "tomorrow" by adding 86 400
UTC seconds:

```cpp
now += 24 * 3600;
struct tm* tmr = localtime(&now);
```

On spring-forward night the clocks skip one hour, so 86 400 UTC seconds
span 25 local hours (23:00 CET → 00:00 CEST the day after tomorrow).

**Fix**

Advance the calendar day directly and re-normalise with `mktime()`, using
a midday anchor to stay well away from the DST boundary:

```cpp
struct tm tmr;
localtime_r(&now, &tmr);
tmr.tm_mday += 1;
tmr.tm_hour = 12; // midday anchor — safely away from any DST boundary
mktime(&tmr);     // re-normalises month/year rollover and re-applies DST rules
```

The same pattern is applied in both `fetchAndProcessData()` and
`processJsonData()` (Fix A already covers the latter via the combined
`isTomorrow` branch).

### Fix C – Fall-back day (25-hour) daily average missed the repeated 02:xx block (minor)

**Symptom**

On the DST fall-back day (last Sunday of October, 25 local hours), the
daily average price and the min/max hour markers were computed over only 24
hour blocks instead of 25, causing a ~4% error in the displayed average and
possibly misidentifying the cheapest or most expensive hour.

**Root cause**

The averaging loop in `processJsonData()` iterated `for (int hour = 0; hour < 24; hour++)`.
`findPriceIndexForHour(2)` returns the index of the **first** 02:xx block.
The second 02:xx block (the repeated CET hour after the clocks fall back)
was never visited.

**Fix**

The loop now scans the `unix_seconds` array by entry index rather than by
hour number. It detects each new hour block by comparing consecutive entries
and handles the second 02:xx block explicitly:

```cpp
for (size_t i = 0; i < unixSeconds.size(); i++) {
    // Only process the first entry of each hour block
    int entryHour = getHourFromPriceIndex(unixSeconds, (int)i);
    if (i > 0) {
        int prevHour = getHourFromPriceIndex(unixSeconds, (int)i - 1);
        if (prevHour == entryHour) continue;
    }
    // ... compute hourlyAvg for this block, including repeated blocks
}
```

### Other changes (cosmetic / non-behavioural)

- Filename and three user-visible version strings bumped to v7.3:
  - `connectToWiFi()` splash: `"Elec. Rate SI v7.2"` → `"v7.3"`
  - `displaySecondaryList()` credit line: `"price ticker v7.2"` → `"v7.3"`
  - `setup()` debug banner: `"v7.2 (Button Robustness)"` → `"v7.3 (DST Hardening)"`
- Inline comments added at each fix site referencing the fix letter (A/B/C).

### Files changed

| File | Change |
|---|---|
| `ESP32_standalone_electricity_ticker_7_3.ino` | New file — v7.2 base with Fixes A, B, C applied |
| `README.md` | Version references updated to v7.3; v7.3 highlights section added |
| `CHANGELOG.md` | v7.3 section added at top |
| `VERSION.md` | Current firmware updated to v7.3; v7.3 highlights section added |

---

## v7.2 - Button Robustness & Screen-Control Fixes (2026-08-04)

**Summary**

Bug-fix release that resolves the three control glitches reported for the v7.1
firmware on the Seeed XIAO ESP32‑C3:

- "It is 20:35 and I cannot scroll the values of the primary screen."
- "Double click does not switch to the secondary screen."
- "Strange behaviour at the end of the day, if there is no tomorrow's data
  available yet and the time is let's say 22:15."

No fee/VAT math, NVS layout, API scheduling or 48-hour scrolling behaviour
was changed. The v7.1 negative-price provider fee logic is preserved
verbatim.

### Fix 1 – Primary-screen scroll works at any hour of the day

**Symptom**

Clicking the button on the primary screen appeared to do nothing — the
display stayed on the current hour and refused to advance.

**Root cause (two compounding bugs)**

1. `displayPriceRow()` only blanked past hours while `currentHour < 22`:

   ```cpp
   if (localHourIndex < currentHour && currentHour < 22) {
       lcd.print("                    "); return;
   }
   ```

   After 22:00 the guard fell through, so the screen was allowed to repaint
   already-finished morning hours (00:00 – 21:59). The moment the user
   scrolled forward, the new "top" hour was visually overwritten by the
   previous morning's data, making the screen look frozen.

2. `displayPrimaryList()` contained an override:

   ```cpp
   if (currentHour >= 21 && timeOffsetHours > 0) {
       displayStartHourOffset = 21 + timeOffsetHours;
   }
   ```

   From 21:00 onward this pinned the top row at `21 + offset`. At 22:15
   every click computed start = 22+offset, was then clamped to 21+offset,
   and the user saw no movement.

**Fix**

- `displayPriceRow()`: simplified the past-hour guard to
  `if (localHourIndex < currentHour) blank();` so past hours of today are
  hidden at every hour of the day, not just before 22:00.
- `displayPrimaryList()`: removed the `currentHour >= 21` override
  entirely. Past-hour blanking is now handled correctly by Fix 1a, so the
  override is no longer needed.

### Fix 2 – Double-click reliably toggles to the secondary screen

**Symptom**

A quick double-click did nothing — the display stayed on the primary
price view. In some cases the screen showed a stuck "Long press detected!
Release to refresh" message right after the device booted or was reset.

**Root cause**

The double-click path itself was correct; it was being starved by a false
"Long press detected!" that fired immediately after every reset. On the
ESP32-C3 the button pin (configured as `INPUT_PULLUP`) floats HIGH for a
few seconds during boot while the internal pull-up is settling and
power-rail noise is ringing. Because `buttonPressStartTime` is
initialised to `0`, the long-press detector's predicate
`millis() - buttonPressStartTime >= 3000` evaluated to
`millis() >= 3000` a few seconds after boot — the firmware interpreted
the floating-pin noise as a genuine 3-second hold, cleared the LCD to:

```
Long press detected!
Release to refresh
```

…and from then on the user could not see any prices to click on
(single- and double-click recognisers both still ran, but their visible
effect was hidden behind the long-press splash).

**Fix**

Added a single new global flag and gated the long-press detector on it:

```cpp
// New flag, set true the first time the pin is observed LOW after boot
bool buttonEverReleased = false;

// In the "reading went LOW" branch:
if (reading == LOW) {
    buttonPressStartTime = millis();
    longPressDetected    = false;
    buttonEverReleased   = true;   // v7.2
}

// In the long-press trip block:
if (buttonState == LOW && !longPressDetected && buttonEverReleased) {  // v7.2
    if (millis() - buttonPressStartTime >= longPressThreshold) {
        longPressDetected = true;
        // ... show "Long press detected! Release to refresh"
    }
}
```

The detector now refuses to fire until the user (or the power-rail noise)
has released the button at least once. The v7.1 button logic (50 ms
debounce, 3 s long-press threshold, 500 ms double-click window, TTP223
timing) is otherwise preserved verbatim.

### Fix 3 – End-of-day scroll is stable with no tomorrow data

**Symptom**

Around 22:00 – 23:59 with no tomorrow data in the buffer, scrolling
through the primary screen produced a screen full of blank past-hour
rows, or wrapped the start hour back to 00:00 in a confusing way.

**Root cause**

`advanceDisplayOffset()` contained a hack:

```cpp
if (allowedAhead < 2 && currentHour >= 21 && !isTomorrowDataAvailable)
    allowedAhead = 2;
```

At 22:00 (with no tomorrow data) this let the user click past hour 23
into "24:00 / 25:00". `displayStartHourOffset` then exceeded
`maxOffsetLimit = 23` and the wrap logic:

```cpp
if (displayStartHourOffset > maxOffsetLimit)
    displayStartHourOffset %= (maxOffsetLimit + 1);
```

…wrapped it back to 0. Combined with the buggy past-hour blanking in
Fix 1a, the result was a screen full of stale blank rows from 00:00 to
21:59.

**Fix**

Removed the `allowedAhead = 2` hack. The natural cap is now sufficient:

- 22:00 → can step 22 → 23, then wraps back to current
- 23:00 → cannot step forward at all

No wrap to 00:00 of the previous day is reachable any more, and Fix 1a
ensures any past hour that does briefly land on the screen is blanked
correctly.

### Fix 4 (bonus) – Auto-return timer now resets on every click

**Symptom**

Scrolling through the 20-line secondary status page (4 lines at a time)
did not push the 10 s auto-return-to-top timeout forward — the display
could jump back to the primary price view mid-read.

**Root cause**

`lastButtonActivity` and `autoScrollExecuted` were only updated inside the
primary-list branches of `advanceDisplayOffset()`. The secondary-list
branch scrolled the offset but did not touch the timer.

**Fix**

Moved the two resets to the very top of `advanceDisplayOffset()`:

```cpp
void advanceDisplayOffset() {
    // Any successful click (single, double, or long-press-release) ends up
    // here, so this is the single place that resets the auto-return timer.
    lastButtonActivity = millis();
    autoScrollExecuted = false;
    // ... rest of the function unchanged
}
```

### Other changes (cosmetic / non-behavioural)

- Filename and three user-visible version strings bumped to v7.2:
  - `connectToWiFi()` splash: `"Elec. Rate SI v7.1"` → `"v7.2"`
  - `displaySecondaryList()` credit line (line 18): `"price ticker v7.1"` → `"v7.2"`
  - `setup()` debug banner: `"Starting Dynamic Electricity Ticker v7.1 (Neg Price Fee) - DST-SAFE"`
    → `"Starting Dynamic Electricity Ticker v7.2 (Button Robustness) - DST-SAFE"`
- Inline comments added at each fix site explaining what v7.1 did wrong,
  so future maintainers do not re-introduce the overrides.
- New global flag `bool buttonEverReleased` (see Fix 2).

### Files changed

| File | Change |
|---|---|
| `ESP32_standalone_electricity_ticker_7_2.ino` | Filename, header comment, `buttonEverReleased` flag, `handleButton()` long-press gate, `displayPriceRow()` past-hour guard, `displayPrimaryList()` removed override, `advanceDisplayOffset()` timer reset, three version strings |

---

## v7.1 - Negative Price Provider Fee (2026-04-06)

**Summary**

Added a separate configurable provider fee for negative spot prices, correctly
modelling contracts where the provider's fee structure differs between positive
and negative market prices.

### What changed

**New constant (in `// Price computation` globals block):**
```cpp
const float NEG_PRICE_COMPANY_FEE_PERCENTAGE = 30.0;
```

**Price calculation is now:**

| Market price | Formula |
|---|---|
| Positive (`raw >= 0`) | `raw × (1 + POWER_COMPANY_FEE_PERCENTAGE/100) × (1 + VAT_PERCENTAGE/100)` |
| Negative (`raw < 0`) | `raw × (1 - NEG_PRICE_COMPANY_FEE_PERCENTAGE/100) × (1 + VAT_PERCENTAGE/100)` |

The switch happens on the **raw API price** before any multiplier is applied.
VAT is applied to both cases, consistent with net billing contracts where VAT
is calculated on the monthly net sum (mathematically equivalent due to VAT
being a linear multiplier).

**Key values for `NEG_PRICE_COMPANY_FEE_PERCENTAGE`:**

| Value | Meaning |
|---|---|
| `30.0` | Provider keeps 30%, pays you 70% of the negative market price |
| `0.0` | Provider passes the full negative price to you (no fee deducted) |

**All 5 fee calculation sites updated:**

| Function | Purpose |
|---|---|
| `updateLeds()` | LED brightness reflects correct negative price |
| `format15MinPrice()` | 15-min row values on primary display |
| `displayPriceRow()` | Hourly price rows on primary display |
| `displaySecondaryList()` | Daily average on secondary info screen |
| Version strings | `connectToWiFi()` LCD and `displaySecondaryList()` credit line |

---

## v7.0 - Rolling 48-Hour Logic & Midnight Bridge (2026-04-03)

**Summary**

This is the **"Golden Build"** for this hardware platform. It combines all the hardware stability fixes from v6.2.4 with a revolutionary new 48-hour price prediction system that eliminates the "1 AM fetch gap" problem that plagues most electricity tickers.

### New Features

#### 1. Dual-Buffer NVS System

The ticker now stores "Today" and "Tomorrow" data independently in NVS, allowing seamless display of up to 47 hours of price data.

**New NVS Keys:**
- `data_prc_t` – Raw JSON payload for tomorrow's prices
- `data_store_t` – Unix timestamp when tomorrow's data was stored

**New Global Variables:**
- `StaticJsonDocument<Config::JSON_BUFFER_SIZE> docTomorrow` – Tomorrow's price data buffer
- `bool isTomorrowDataAvailable` – Flag indicating tomorrow's data availability
- `float averagePriceTomorrow` – Tomorrow's daily average price
- `int lowestPriceIndexTomorrow` – Index of tomorrow's lowest price hour
- `int highestPriceIndexTomorrow` – Index of tomorrow's highest price hour

#### 2. The Midnight Bridge (Rollover Logic)

**Problem:**
Most electricity tickers fail at midnight because they rely on slow API calls to fetch new data. The Energy-Charts API typically doesn't publish next-day data until 1-2 AM, leaving users with a "No Data" screen for hours.

**Solution:**
The Midnight Bridge detects the moment the local clock moves from 23:59:59 to 00:00:00 and instantly promotes the pre-fetched "Tomorrow" data to become "Today" data.

**Implementation (in `loop()`):**
```cpp
if (daycheck->tm_mday != trackedDay) {
    // Midnight rollover detected
    if (isTomorrowDataAvailable) {
        // Swap tomorrow to today instantly
        doc = docTomorrow;
        docTomorrow.clear();

        // Update all statistics
        averagePrice = averagePriceTomorrow;
        lowestPriceIndex = lowestPriceIndexTomorrow;
        highestPriceIndex = highestPriceIndexTomorrow;

        // Save to NVS and clear tomorrow slot
        serializeJson(doc, payload);
        saveDataToNVS(payload, false);
        clearTomorrowNVS();

        timeOffsetHours = 0;
        displayPrices();
    }
}
```

**NVS Power-Failure Protection:**
Immediately after the swap, the new "Today" data is serialized and saved to NVS. If power is cut at 00:05 AM, the device reboots with correct data already loaded.

#### 3. Smart Fetching & Tomorrow's Data

**Automatic Tomorrow Fetch:**
After 14:00 (2 PM) local time, the ticker automatically fetches tomorrow's data using the `&start=YYYY-MM-DD` parameter:

```cpp
void fetchAndProcessData(bool fetchTomorrow) {
    String url = api_url;
    if (fetchTomorrow) {
        time_t now = time(nullptr);
        now += 24 * 3600; // Add 24 hours
        struct tm* tmr = localtime(&now);
        char dateStr[20];
        snprintf(dateStr, sizeof(dateStr), "%04d-%02d-%02d",
                 tmr->tm_year + 1900, tmr->tm_mon + 1, tmr->tm_mday);
        url += "&start=";
        url += dateStr;
    }
    // ... HTTP request follows
}
```

**Smart Scheduling (`handleDataFetching()`):**
```cpp
struct tm* ti = localtime(&now);
// Priority 1: If it's after 14:00 and we don't have tomorrow's data yet
if (ti->tm_hour >= 14 && !isTomorrowDataAvailable) {
    fetchAndProcessData(true); // Fetch tomorrow
}
```

#### 4. Seamless 48H Scrolling

**Extended Display Range:**
When tomorrow's data is available, users can scroll up to **47 hours ahead**:

```cpp
int maxOffsetLimit = isTomorrowDataAvailable ? 47 : 23;
if (displayStartHourOffset > maxOffsetLimit)
    displayStartHourOffset %= (maxOffsetLimit + 1);
```

**Visual Distinction for Tomorrow:**
Future hours are marked with `HH:>>` to clearly distinguish tomorrow's prices from today's:

```cpp
if (showTomorrow) {
    snprintf(buffer, sizeof(buffer), "%02d:>>", localHourIndex);
} else {
    snprintf(buffer, sizeof(buffer), "%02d:00", localHourIndex);
}
```

**Correct Min/Max Indicators:**
The code correctly uses tomorrow's statistics when displaying future hours:

```cpp
int lowIdx = showTomorrow ? lowestPriceIndexTomorrow : lowestPriceIndex;
int highIdx = showTomorrow ? highestPriceIndexTomorrow : highestPriceIndex;
```

#### 5. Hardware Stability (Preserved from v6.2.4)

All v6.2.4 stability fixes remain intact:

- **State-Based Refresh**: Display updates exactly at 00, 15, 30, and 45 minutes past the hour, even if the CPU is busy
- **LED Indicators Pinned to Current**: White LED and built-in LED reflect actual current prices, regardless of what the user is viewing on screen

### Technical Implementation Details

#### Dual-Buffer Display Helpers

**`display15MinuteDetails(int row, int totalHourOffset)`:**
- Now accepts `totalHourOffset` (0-47) instead of just hour index
- Automatically selects correct buffer (`doc` or `docTomorrow`) based on offset
- Shows past segments ("> ") for current hour

**`displayPriceRow(int row, int totalHourOffset, bool isCurrentHourRow)`:**
- Extended to handle tomorrow's data with visual indicators
- Correctly applies hour suppression logic only to today's hours

#### NVS Persistence Updates

**`saveDataToNVS(const String& rawJson, bool isTomorrow)`:**
```cpp
if (isTomorrow) {
    preferences.putString("data_prc_t", rawJson);
    preferences.putULong("data_store_t", (unsigned long)now);
} else {
    // Original "today" save logic
    preferences.putInt("data_day", timeinfo.tm_mday);
    // ...
}
```

**`loadDataFromNVS()`:**
- Now loads both today and tomorrow data from NVS
- Validates and processes both buffers independently

#### API Date Validation

The `processJsonData()` function now validates data against the correct target date:

```cpp
time_t targetTime = time(nullptr);
if (isTomorrow) targetTime += 24 * 3600;
struct tm* targetDayTm = localtime(&targetTime);

bool sameDate = (lastDataTm->tm_mday == targetDayTm->tm_mday &&
                 lastDataTm->tm_mon == targetDayTm->tm_mon &&
                 lastDataTm->tm_year == targetDayTm->tm_year);
```

### Why This Is the "Golden Build"

| Feature | v6.2.4 | v7.0 |
|---------|--------|------|
| Display hours ahead | 23 hours (today only) | 47 hours (today + tomorrow) |
| Midnight transition | "No Data" until API updates | Seamless swap from buffer |
| Power failure resilience | Relies on API availability | NVS contains valid data |
| Visual tomorrow indication | None | `HH:>>` format |
| Tomorrow min/max markers | N/A | Correct indices |
| Fetch strategy | Once per day | Smart: today + tomorrow after 14:00 |

### User Experience: Behavior & Display States

The display changes based on which data buffer is being used and the status of the fetch:

| **State** | **Display Output** | **LED Behavior** |
|----------|-------------------|-----------------|
| **Normal (Today)** | Shows current prices and 15-min details. Hours are marked as HH:00. | White LED reflects current price status (Breathe, Solid, or Blink). |
| **Scrolling (Tomorrow)** | Future prices are displayed. Hours are marked with HH:>> to indicate "Tomorrow". | **Pinned to Today:** The LEDs continue showing the _actual current_ price status even while you scroll through tomorrow. |
| **No Data** | Displays: "No data for today, Press & hold to, refresh manually." | White LED is turned **OFF** to avoid misleading price signals. |
| **Connecting** | "Elec. Rate SI v7.0" followed by "Connecting..." and progress dots. | Built-in LED is **OFF** until connection is established. |

### API Call Intervals & Retry Strategy (v7.0)

The exact API call intervals in version 7.0 vary depending on the device's state, data availability, and time of day:

#### Primary Scheduling (Daily Fetch)

The device aims to maintain a rolling 48-hour data window by fetching today's and tomorrow's data at specific times:

- **Initial Boot:** An API call is attempted immediately upon startup and time synchronization.
- **Tomorrow's Data (Smart Fetching):** Starting at **14:00 (2 PM) local time**, the device begins checking for the next day's prices. It will attempt to fetch this data periodically until successful.
- **Midnight Rollover:** At exactly **00:00:00**, the device "promotes" tomorrow's data to the today buffer. If tomorrow's data was already successfully fetched and stored, **no API call is needed at midnight**.

#### Retry Logic (Exponential Backoff)

If a scheduled API call fails (e.g., due to a temporary server error or WiFi glitch), the device uses a safety-oriented retry interval:

- **Max Retries:** 5 attempts (`HTTP_GET_RETRY_MAX`)
- **Interval Formula:** Uses a backoff factor of **2** (`HTTP_GET_BACKOFF_FACTOR`)
- **Typical Progression:** After a failure, it waits a short period, then doubles that wait time for each subsequent failure until the maximum retry count is reached

#### "Midnight Phase" Recovery

If the device reaches midnight but **does not** have tomorrow's data ready (meaning the afternoon fetches failed), it enters a high-priority state called `midnightPhaseActive`:

- **Interval:** It bypasses the standard daily schedule and retries the API **more aggressively** (initially every minute).
- **Goal:** To clear the "No Data" screen and restore the price display as quickly as possible once the energy provider's server updates.

#### Background Monitoring

While not a full API call, the device performs these checks constantly:

- **Loop Pacing:** The main system loop runs every **100ms** to check if it's time for a scheduled fetch.
- **Display Refresh:** The screen logic checks the time every loop but only refreshes the UI every **15 minutes** (at :00, :15, :30, :45) to match the price data intervals.

---

## v6.2.4 - Exact-boundary display refresh bug (2026-04-01):

**Summary**

Top of the hour auto display refresh glitch fix where display automatically refreshed but showed the PREVIOUS hour's data.

### Problem:
- At the exact top of the hour (e.g., 20:00:00), the display automatically refreshed but showed the PREVIOUS hour's data (19:00). This happened because the "next-boundary" rounding logic in findCurrentPriceIndex() incorrectly excluded the current interval if the time was exactly on the boundary.

### Solution:
- Simplified findCurrentPriceIndex() to use a robust "last entry <= now" comparison. This ensures the display transitions to the new hour instantaneously at XX:00:00.

---

## v6.2.3 - State-based display refresh logic fix (2026-04-01):

**Summary**

The refresh logic should be "State-Based" rather than "Event-Based." Instead of checking if the minute is zero, it should check if the current hour is different from the last recorded hour.

### Problem: Screen would occasionally fail to update if the ESP32 was busy
   - fetching data or reconnecting WiFi) during the exact 00/15/30/45 minute mark.

### Solution: Switched from "Event-Based" (refresh only AT minute X) to "State-Based"

    (refresh IF current time != last refresh time).
  - This ensures the screen updates immediately even if the device was busy during the transition.

---

## v6.2.2 - Display blank lines issue fix (2026-03-31):

**Summary**

Fixed a bug where the display was showing blank lines

### Problem: Sometimes rows 0 and 1 (current 15-min prices and current hour) were blank

**Cause:** The "hour suppression" logic was hiding the current hour unexpectedly

### Solution:

  - Row 1 (current hour) now ALWAYS shows - suppression logic only applies to rows 2-3
  - Row 0 (15-min details) also always shows for the current hour

---

## v6.2.1 – Current Interval Fix (2026‑03‑29)

**Summary**

Fixed a bug where the display was showing prices one hour ahead of the current time.

### Problem: Display Showing Next Hour Instead of Current

**Root Cause:**

The `findCurrentPriceIndex()` function was finding the **next** 15-minute interval (first entry with timestamp >= now), but it should find the **current** interval (the one we're currently IN).

For example, at 17:57:
- The current 15-minute interval is **17:45-18:00** (price indexed at 17:45)
- The **next** interval is 18:00-18:15 (price indexed at 18:00)
- The buggy function returned the index for **18:00** instead of **17:45**
- Result: Display showed hour **18** instead of hour **17**

### Solution

The fix calculates the **next 15-minute boundary** and finds the last entry **strictly before** that boundary:

```cpp
// Calculate the next 15-minute boundary
const int QUARTER_SECONDS = 15 * 60; // 900 seconds
time_t nextQuarter = ((now + QUARTER_SECONDS - 1) / QUARTER_SECONDS) * QUARTER_SECONDS;

// Find the last entry strictly before nextQuarter
for (size_t i = unixSeconds.size(); i > 0; i--) {
    if ((time_t)unixTime < nextQuarter) {
        return (int)(i - 1);
    }
}
```

**Example:**
- At 17:57: nextQuarter = 18:00, finds last entry < 18:00 = 17:45
- At 18:00: nextQuarter = 18:15, finds last entry < 18:15 = 18:00
- At 18:46: nextQuarter = 19:00, finds last entry < 19:00 = 18:45

---

## v6.2.0 – DST (Daylight Saving Time) Handling Fixed (2026‑03‑29)

**Summary**

This release fixes a critical bug that caused incorrect price display on DST switch days. On March 29, 2026 (the spring forward day), the ticker at 12:41 showed prices for hours 13:00, 14:00, and 15:00 instead of the correct 12:00, 13:00, and 14:00.

### Problem: Arithmetic-Based Index Calculation

**Root Cause (v6.0.0 – v6.1.2):**

The code assumed every day has exactly 96 price entries:

```cpp
int startIndex = hourIndex * 4;  // e.g., hour 12 → index 48
```

This assumption breaks on DST switch days:

| Day Type | Hours | Price Entries | Example |
|----------|-------|---------------|---------|
| Normal day | 24 | 96 | Array indices 0-95 |
| Spring forward (March) | 23 | 92 | Index 48 points to wrong time |
| Fall back (October) | 25 | 100 | Index 48 points to wrong time |

At 12:41 on March 29, 2026:
- ESP32 correctly reported `timeinfo.tm_hour = 12`
- Old code calculated `12 × 4 = 48`
- But array only had 92 entries (no index 48 that maps to local hour 12)
- Result: Displayed prices for 13:00, 14:00, 15:00 instead of 12:00, 13:00, 14:00

### Solution: Timestamp-Based Lookups

**New approach (v6.2.0):**

All price lookups now search the `unix_seconds` array using actual timestamps:

```cpp
// Find index by searching for matching hour in timestamps
int findPriceIndexForHour(const JsonArray& unixSeconds, int targetHour) {
    for (size_t i = 0; i < unixSeconds.size(); i++) {
        unsigned long unixTime = unixSeconds[i].as<unsigned long>();
        time_t t = (time_t)unixTime;
        struct tm* ptm = localtime(&t);
        if (ptm != NULL && ptm->tm_hour == targetHour) {
            return (int)i;  // Found correct index for this hour
        }
    }
    return -1;
}
```

**New functions added:**
- `findPriceIndexForHour()` – Finds first price index for a given hour
- `findCurrentPriceIndex()` – Finds current 15-minute slot using Unix timestamp
- `getHourFromPriceIndex()` – Gets hour from price array index

**Updated functions:**
- `getHourlyAverage()` – Now uses timestamp lookup instead of `hourIndex * 4`
- `display15MinuteDetails()` – Timestamp-based with hour verification in loop
- `displayPriceRow()` – Timestamp-based data index lookup
- `displayPrimaryList()` – Timestamp-based current hour detection
- `updateLeds()` – Timestamp-based current interval lookup

### Why This Is Future-Proof

| Scenario | Code Behavior |
|----------|---------------|
| Normal days (96 entries) | Works as before |
| DST spring forward (92 entries) | Timestamp lookup finds correct indices |
| DST fall back (100 entries) | Timestamp lookup finds correct indices |
| EU cancels DST | Only update `TZ_CET_CEST` string; code works unchanged |

If EU parliament ever cancels DST switching, you only need to update the `TZ_CET_CEST` line (one line of code). The price lookup logic requires no changes.

---

## v6.1.2 – LED Indicator Restored (ESP32 PWM Fix) (2026‑03‑11)

**Summary**

This release fixes a regression introduced in **v6.1.1** where the **white LED price indicator** could remain **dimly lit** even when the LCD backlight turned off, and the intended **blink/breathe patterns** no longer behaved correctly.

### Fixed: White LED Stuck Dim / Patterns Broken (PWM vs Digital)

**Problem (v6.1.1):**

- The sketch uses `analogWrite()` to drive the LED with PWM for breathe/blink patterns.
- However, in some "LED OFF" branches the code used `digitalWrite(LOW)` on the same `whiteLedPin`.
- On ESP32 (LEDC), once PWM is attached to a pin, `digitalWrite(LOW)` may **not fully disable** PWM output.
- Result:
  - LED could remain **faintly on** (dim glow) when LEDs were supposed to be off.
  - Some patterns could appear "stuck" or inconsistent.

**Solution (v6.1.2):**

- LED control is now **PWM‑only** inside `updateLeds()`:
  - Use `analogWrite(whiteLedPin, 0)` instead of `digitalWrite(whiteLedPin, LOW)`.
  - Use `analogWrite(whiteLedPin, 255)` instead of `digitalWrite(whiteLedPin, HIGH)`.
  - Blink/double‑blink toggles now switch between PWM **0** and **255**.
- This ensures the LED is **truly off** whenever LED output is gated off.

---

## v6.1.1 – Daily Min/Max Includes Negative & Zero Prices (2026‑03‑07)

**Summary**

This release fixes a bug where the **daily lowest / highest hourly price marker** ignored negative prices (and also ignored 0.0), which could cause the ticker to incorrectly mark the **lowest positive** price as the daily minimum.

### Fixed: Daily Low/High Marker Ignored Negative & Zero Prices

**Problem (v6.1.0):**

- In `processJsonData()` the daily min/max scan used:
  ```cpp
  if (hourlyAvg > 0) { ... }
  ```
- This had two side effects:
  1. **Negative** hourly averages were completely skipped.
  2. A true price of **0.0** was also skipped (even though 0 can be a valid market price).

**Solution (v6.1.1):**

- The min/max and average scan now:
  - Treats an hour as valid based on **data availability** (having all 4×15‑minute entries), not based on value sign.
  - Includes **all values** (negative, zero, positive).

---

## v6.1.0 – Midnight Fetch & Market Day Fix (2026‑01‑30)

**Summary**

This release fixes a bug where the ticker could remain indefinitely on the **"No data for today"** screen after midnight, even though the API was already returning fresh data.

### Fixed: Stuck on NO_DATA_OFFSET After Midnight

**Problem (v6.0.0):**

- The API can continue to serve **yesterday's market day** for some time after local midnight.
- HTTP + JSON success always incremented `apiSuccessCount`, even if the data was "not for today".
- The scheduler treated such fetches as **successful** and never retried.

**Solution (v6.1.0):**

- `processJsonData()` now determines the "market day" using the **last** `unix_seconds` timestamp.
- A scheduled fetch is only successful if `lastProcessJsonAcceptedToday == true`.
- Midnight retry logic keeps trying until valid "today" data is received.

---

## v6.0.0 – NVS Storage & Daily Fetch (2026‑01‑27)

**Summary**

First major redesign focused on reducing API traffic and improving resilience using non‑volatile storage.

### New

- NVS namespace `"my-ticker"` with Wi‑Fi credentials and daily price data caching.
- Boot behavior: Try to load and validate NVS data; if date matches today → reuse it and **skip** initial API call.
- After‑midnight: NVS is overwritten with each successful new‑day dataset.

---

## v5.x – Earlier Versions

Earlier versions (v5.x and below) had:

- No NVS‑based caching of daily API data.
- More frequent API calls (e.g., hourly refresh pattern).
- Less robust handling of DST and daily boundaries.

For exact details, see older `.ino` files and their header comments in this repository.
---
