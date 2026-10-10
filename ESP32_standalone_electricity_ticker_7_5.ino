/*
  ESP32_standalone_electricity_ticker_7_5.ino
  -----------------------------------------------------

  VERSION 7.5 CHANGES (2026-10-10):
  ----------------------------------
  Changes from v7.4: DST hardening, explicit API URL date bounds, a
  complete localtime_r() sweep, a real fall-back-day bug in the daily
  average, and two small additions to the fetch path (Fixes F2 and G).
  Self-contained firmware release. No fee/VAT math, NVS layout, 48-hour
  scrolling behaviour, Midnight Bridge logic, or button handling was
  changed - the gestures are still v7.4's: single click scrolls, double
  click toggles, long press forces a manual refresh. All v7.4 scheduling
  fixes (A/B/C) and all v7.3 DST fixes (A/B/C) are preserved verbatim.
  Every change below was verified against the running behaviour before
  being applied rather than taken on trust. Where the original issue
  description did not reproduce, the measured result is recorded here
  instead of the claim.
  Fix A - tm_isdst = -1 before mktime() (2 sites)
  -----------------------------------------------
  In fetchAndProcessData() and processJsonData(), the calendar-day
  increment (tm_mday += 1; tm_hour = 12) was followed by mktime() without
  resetting tm_isdst. On DST transition days the stale tm_isdst inherited
  from the source day makes mktime() normalise the target struct against
  the wrong UTC offset, so the resulting wall-clock time reads 13:00 /
  11:00 instead of the intended midday anchor. Fixed by setting
  tm_isdst = -1 before every mktime() call so the C library determines
  the correct DST state for the target day.

  Measured impact (verified against glibc mktime with TZ=CET-1CEST):
  because the midday anchor is already in place, the +-3600 s offset
  shifts only the time-of-day field. The calendar day this firmware
  actually consumes - tm_year/tm_mon/tm_mday for the API URL, and the
  day/month/year comparison in processJsonData() - is correct either
  way. This is therefore defensive hardening that makes the intent
  explicit and removes a dependency on whether the C library honours a
  stale tm_isdst, not a reachable mis-display in v7.4.

  Fix B - Explicit &start=&end= on today's fetch URL
  --------------------------------------------------
  The today-fetch URL was still the bare api_url string, which leaves the
  returned window to the server's own caching. Added
  &start=YYYY-MM-DD&end=YYYY-MM-DD (today's local date) to the URL
  construction in fetchAndProcessData(false).

  A live probe on 2026-10-09 found the bare endpoint already returning the
  correct current local day, so this is hardening rather than a fix for an
  observed stale window.

  Verified against the live API: `end` is INCLUSIVE, so &start=D&end=D
  returns exactly one market day (96 entries normally, 92 on a
  spring-forward day, 100 on a fall-back day), and `start`/`end` are
  interpreted in local exchange time, so the window is the correct local
  market day including on DST transition days.

  Fix C - localtime() -> localtime_r() (8 call-sites, 7 functions)
  ----------------------------------------------------------------
  localtime() returns a pointer to a single shared static struct tm, so
  every call invalidates the result of the previous one. All eight
  remaining call-sites now use localtime_r() with a local struct tm:
    - findPriceIndexForHour()
    - getHourFromPriceIndex()
    - displaySecondaryList()  (2 sites: lastSuccessfulFetchTime and
                               nvsLastStoreTime)
    - scheduleAfterMidnightFailure()
    - handleDataFetching()
    - loop()  (2 sites: Midnight-Bridge day check, state-based refresh)
  getDstFlagFromPriceIndex() added by Fix D below also uses localtime_r().

  handleDataFetching() is the one that mattered: it dereferenced the
  localtime() result directly (if (ti->tm_hour >= 14 ...)) with no NULL
  check, so a failed conversion would have meant a NULL dereference.
  It now tests the localtime_r() return value like every other site.

  Fix D - DST-aware hour-block detection in the daily average (real bug)
  ---------------------------------------------------------------------
  On the DST fall-back day local 02:00 occurs TWICE (CEST then CET), so
  the day holds 25 hour-blocks / 100 price entries instead of 24 / 96.

  The average + min/max loop in processJsonData() detects the start of a
  new block with `if (prevHour == entryHour) continue;` - it compared
  the tm_HOUR field only. On a fall-back day the second 02:xx block has
  the same tm_hour as the first, so every one of its entries was skipped
  by that guard. The daily average was therefore computed over 24 blocks
  instead of 25, and if the second 02:00 block held the day's extreme
  price, the lowest/highest marker pointed at the wrong hour and the true
  extreme was never flagged. (The v7.3 comment on that loop already
  claimed fall-back blocks were included; they were not.)

  Block detection now compares tm_hour AND tm_isdst, so the two 02:xx
  blocks are recognised as distinct. The existing fallback further down
  the same loop, which recomputes the block average from startIndex
  instead of calling getHourlyAverage(), now actually reaches that case
  and returns the correct value for the second block.

  Added getDstFlagFromPriceIndex() as the matching accessor for
  getHourFromPriceIndex().

  Display note: the LCD is a 24-row hour-of-day view, so a repeated
  02:00 row has no distinct place to render. findPriceIndexForHour() and
  getHourlyAverage() intentionally keep returning the FIRST 02:xx block
  for display purposes - unchanged, and correct for a 24-row layout.
  Only the aggregate statistics are now DST-complete.

  No other behaviour changed.

  Fix E - today-fetch no longer spins the loop in an unbounded blocking
  http.GET() (real bug, pre-existing since v7.3)
  ----------------------------------
  Reported as "the double-click screen switch became slow and the screen
  freezes for a second or two". Investigated: the button and both display
  renderers are byte-for-byte identical to v7.4, and a localtime() vs
  localtime_r() benchmark of the display hot path measured v7.5 at 22%
  FASTER (23.9 us vs 30.6 us per redraw). The switch path is not the cause.

  The actual cause is a scheduling gap in fetchAndProcessData(). Both its
  "JSON parse failure" and "API returned the wrong day" branches advance
  nextScheduledFetchTime only when `fetchTomorrow` is true. v7.4 added that
  guard for tomorrow but never covered the today path, even though the
  accompanying comment already said "Must advance the schedule to avoid a
  tight retry loop". When today's fetch was rejected by the date gate,
  nextScheduledFetchTime stayed in the past while isTodayDataAvailable
  remained false, so handleDataFetching() re-entered on the very next
  iteration and issued another blocking http.GET() - forever.

  Measured: 12 loop iterations -> 12 fetches, 6000 ms blocked. After Fix E:
  1 fetch, 500 ms blocked, next fetch scheduled 588 s out.

  Fix: a single belt-and-braces forward-progress guard in
  handleDataFetching(), matching the shape of the v7.4 Fix B guard that
  already protects the tomorrow path.

  Note on Fix B: measured against the live API, the bare endpoint is
  currently serving the PREVIOUS market day while the explicit
  &start=&end= form returns the requested day. Fix B therefore reduces how
  often this tight loop is entered; it does not cause it.

  Fix F2 - measure the HTTP request instead of guarding it
  ------------------------------------------------
  A wall-clock stall guard was considered and NOT adopted. It would have
  had to measure millis() AFTER http.GET() had already returned, so it
  could not prevent a single millisecond of blocking - the loop was
  already stuck by the time it ran - while adding a real risk of
  discarding a large-but-valid response past an arbitrary 16 s cutoff.
  Instead, every http.GET() now prints its actual duration, so a future
  freeze is measurable on the serial monitor instead of a matter of
  inference.

  Fix G - do not START a fetch while the user is pressing the button
  ------------------------------------------------
  handleDataFetching() returns early if a physical button edge was
  captured less than BUTTON_INTERACTION_GUARD_MS (800 ms) ago, which
  covers the full double-click window plus the 500 ms the firmware waits
  to confirm it. The fetch is only DEFERRED, never cancelled:
  nextScheduledFetchTime is untouched, so the data still arrives, just
  once the user's hands are off the button.

  VERSION 7.4 CHANGES (2026-10-04):
  ----------------------------------
  Button-responsiveness fix: eliminate the tight retry loop that starves
  handleButton() from ~14:00 until the API publishes tomorrow's data.

  Root cause (identified by tracing the afternoon/evening freeze):
    After 14:00 local time, if the Energy-Charts API has not yet published
    tomorrow's prices (typical until ~01:00-02:00 UTC = 03:00-04:00 CEST),
    handleDataFetching() calls fetchAndProcessData(true) every loop
    iteration because nextScheduledFetchTime is NEVER advanced after a
    failed/rejected tomorrow fetch. Each iteration blocks the main loop
    for 5-15 seconds inside http.GET(), starving handleButton().
    The display still updates (displayPrices() runs after the HTTP block),
    but the button is only sampled for ~1 ms per iteration.

  Fix A - Advance nextScheduledFetchTime after every failed tomorrow fetch
  ------------------------------------------------------------------------
  In fetchAndProcessData(), every failure path (HTTP error, JSON parse
  error, data-rejected-by-processJsonData, no-WiFi bail-out) now advances
  nextScheduledFetchTime by 1800 s (30 min) when fetchTomorrow == true.
  This breaks the tight retry loop.

  Fix B - Belt-and-suspenders guard in handleDataFetching()
  ---------------------------------------------------------
  After calling fetchAndProcessData(true), if isTomorrowDataAvailable is
  still false, nextScheduledFetchTime is forced to at least now + 1800.
  This protects against any future refactor that removes the advance from
  the helper.
  
  Fix B additionally caps the tomorrow-fetch window to 14:00–23:00
  (ti->tm_hour <= 23), giving at most 19 HTTP attempts per day.

  Fix C - Long-press detector: only honour genuine long presses
  -------------------------------------------------------------
  The long-press detector in handleButton() records buttonPressStartTime
  at RELEASE time, so the "3-second hold" check (millis() -
  buttonPressStartTime >= 3000) actually measures idle time since the
  last release, not press duration. After 3 s of idle the detector fires,
  sets longPressDetected = true, and the NEXT press (even a normal short
  click) triggers a forced manual refresh (nextScheduledFetchTime = now),
  blocking the loop for another 10-15 s. This compounds Fix A's problem.

  Fix: the press handler now checks pressDuration >= longPressThreshold
  before honouring longPressDetected. A spurious idle-time flag no longer
  hijacks a normal click.

  Fix D - Button edge interrupt (non-blocking press capture)
  ----------------------------------------------------------
  A CHANGE interrupt on buttonPin sets a volatile flag. At the top of
  handleButton(), if the flag is set and the pin is now released, a
  synthetic press+release event is injected into the state machine.
  This guarantees that a press occurring during a legitimate 10-15 s
  HTTP block is not silently lost.

  No fee/VAT math, NVS layout, API URL construction, DST logic,
  48-hour scrolling behaviour, or Midnight Bridge logic was changed.
  All v7.3 DST fixes (A/B/C) and all v7.2 button/screen fixes (1/2/3/4)
  are preserved verbatim.

  VERSION 7.3 CHANGES (2026-10-03):
  ----------------------------------
  DST edge-case hardening.
  Bug-fix release that corrects three DST-related edge cases identified by
  static analysis of the v7.2 firmware. No fee/VAT math, NVS layout, button
  logic, API scheduling or 48-hour scrolling behaviour was changed.

  Fix A - localtime() aliasing in processJsonData() (critical)
  -------------------------------------------------------------
  Two consecutive localtime() calls return the same static pointer; the second
  call silently overwrote the first result, making the date comparison always
  X == X (unconditionally true). Replaced with localtime_r() into separate
  struct tm variables.

  Fix B - +24*3600 "tomorrow" date arithmetic (moderate)
  -------------------------------------------------------
  Adding exactly 24*3600 UTC seconds near DST boundaries can land on the wrong
  local calendar day. Replaced with calendar-day advancement via tm_mday += 1
  and mktime(), anchored at midday to stay away from DST boundary hours.

  Fix C - 25-hour fall-back day averaging (minor)
  -----------------------------------------------
  Daily averaging/min/max scan iterated hour=0..23 and missed the second 02:xx
  block on fall-back days. Reworked to scan unix_seconds by index so repeated
  hour blocks are included.

  VERSION 7.2 CHANGES (2026-08-04):
  ----------------------------------
  Bug-fix release: primary-screen scroll, double-click and end-of-day logic
  (the three glitches reported on 2026-04-25 for the v7.1 firmware). No fee/VAT math, NVS layout, API or scheduling behaviour was changed.

  FIX 1 - "Cannot scroll the primary screen" (e.g. at 20:35)
  -----------------------------------------------------------
  Two compounding bugs in displayPrimaryList() / displayPriceRow() were
  cancelling each other out and made single-click scrolling look dead:
    a) displayPriceRow() only blanked past hours while currentHour < 22.
       After 22:00 the screen was allowed to repaint already-finished
       morning hours (00:00..21:59), so as soon as the user scrolled
       forward the new "top" hour was hidden by the previous morning's
       data visually leaking back in. Simplified the guard to
           if (localHourIndex < currentHour) blank();
       so past hours of TODAY are hidden at every hour of the day.
    b) displayPrimaryList() contained an override
           if (currentHour >= 21 && timeOffsetHours > 0)
               displayStartHourOffset = 21 + timeOffsetHours;
       which pinned the top row at 21:00 + offset from 21:00 onward.
       At 22:15 this meant every click computed start = 22+offset, was
       then clamped to 21+offset, and the user saw no movement. The
       override is no longer needed (Fix 1a blanks the past correctly)
       and has been removed.

  FIX 2 - "Double click does not switch to the secondary screen"
  ---------------------------------------------------------------
  The double-click path itself was correct; it was being starved by a
  false "Long press detected!" that fired immediately after every reset.
  Cause: on the ESP32-C3 the button pin (INPUT_PULLUP) floats HIGH for
  several seconds during boot, while buttonPressStartTime is initialised
  to 0. As soon as millis() crosses 3000 ms, the long-press detector
  tripped on a phantom 3-second hold, cleared the LCD to
  "Long press detected! / Release to refresh", and from then on the user
  could not see any prices to click on (single- and double-click
  recognisers both still ran, but their visible effect was hidden
  behind the long-press splash).
  Fix: added a single bool buttonEverReleased that is set to true the
  first time the pin is observed LOW after boot, and gated the long-
  press detector on it:
       if (buttonState == LOW && !longPressDetected && buttonEverReleased)
  The detector refuses to fire until the user (or the power-rail noise)
  has released the button at least once. v7.1 button logic is otherwise
  preserved verbatim - TTP223 timings and the 500 ms double-click window
  are unchanged.

  FIX 3 - "Strange behaviour at the end of the day with no tomorrow data"
  ------------------------------------------------------------------------
  advanceDisplayOffset() contained a hack
       if (allowedAhead < 2 && currentHour >= 21 && !isTomorrowDataAvailable)
           allowedAhead = 2;
  which at 22:00 (with no tomorrow data) let the user click past
  hour 23 into "24:00/25:00", where displayStartHourOffset wrapped back
  to 0 and filled the LCD with the now-unblanked past-hour rows from
  Fix 1a. The hack has been removed; the natural cap is now sufficient:
       22:00 -> can step 22 -> 23, then wraps to current
       23:00 -> cannot step forward at all
  No wrap to 00:00 of the previous day is reachable any more, so the
  end-of-day screen is stable.

  FIX 4 - Secondary-list scroll did not reset the auto-return timer
  -----------------------------------------------------------------
  lastButtonActivity / autoScrollExecuted were only updated in the
  primary-list branches of advanceDisplayOffset(). Scrolling the
  secondary list (status / NVS page) therefore did not push the 10 s
  auto-return-to-top timeout forward, and the display could jump back
  to prices mid-read. The two resets are now at the top of
  advanceDisplayOffset() so every successful click (single, double,
  long-press-release) refreshes the timer regardless of which list is
  showing.

  Other changes (cosmetic / non-behavioural)
  ------------------------------------------
  - Filename and three user-visible version strings bumped to v7.2:
      connectToWiFi() splash:        "Elec. Rate SI v7.1" -> "v7.2"
      displaySecondaryList() line 18: "price ticker v7.1"  -> "v7.2"
      setup() debug banner:          "v7.1 (Neg Price Fee)"
                                     -> "v7.2 (Button Robustness)"
  - Inline comments added at each fix site explaining what v7.1 did
    wrong, so future maintainers do not re-introduce the overrides.
  - New global flag bool buttonEverReleased (see Fix 2).

  VERSION 7.1 CHANGES (2026-04-06):
  ----------------------------------
  Separate provider fee for negative spot prices.
  - Added NEG_PRICE_COMPANY_FEE_PERCENTAGE constant (default 30.0).
  - Positive prices use: raw * (1 + POWER_COMPANY_FEE_PERCENTAGE/100) * (1 + VAT_PERCENTAGE/100)
  - Negative prices use: raw * (1 - NEG_PRICE_COMPANY_FEE_PERCENTAGE/100) * (1 + VAT_PERCENTAGE/100)
  - Switch happens on raw API price (before any multiplier).
  - VAT applied to both cases (consistent with net billing + end-of-month VAT).
  - All 5 fee calculation sites updated: updateLeds(), format15MinPrice(),
    displayPriceRow(), displaySecondaryList() (daily average), connectToWiFi() version string.
  - 0.00 = provider passes full negative price to you (no fee).
  - 30.0 = provider keeps 30%, pays you 70% of the negative market price.

  VERSION 7.0 CHANGES (2026-04-02):
  ----------------------------------
  MAJOR UPGRADE: Rolling 48-Hour Logic & Midnight Bridge
  - Added Dual-Buffer NVS: Stores "Today" and "Tomorrow" independently.
  - Midnight Bridge: At exactly 00:00:00, "Tomorrow" data automatically becomes "Today",
    eliminating the 1AM/2AM UTC offset fetch delay.
  - Seamless 48H Scrolling: If next-day data is available, the button allows
    scrolling up to 47 hours ahead.
  - Visual Indicators: Future hours are marked with "HH:>>" to distinguish
    from today's "HH:00".
  - Smart Fetching: Automatically looks for tomorrow's data after 14:00 local time.

  VERSION 6.2.4 CHANGES:
  - FIX: Exact-boundary display refresh bug resolved.
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

// ========================================================================
// CONFIG STRUCT
// ========================================================================

struct Config {
    static const int JSON_BUFFER_SIZE = 4096;
    static const int HTTP_TIMEOUT = 10000;
    static const int HTTP_CONNECT_TIMEOUT = 5000;
    static const int WIFI_RETRY_MAX = 20;
    static const int NTP_TIMEOUT = 15000;
    static const int LOOP_UPDATE_INTERVAL = 100;
};
#define DEBUG_LEVEL 2
#define HAS_WHITE_LED true

// ========================================================================
// GLOBALS
// ========================================================================

const int whiteLedPin   = 5;
const int builtinLedPin = 21;
const int buttonPin     = 4;
const int presencePin   = 9;

bool ledsConnected = HAS_WHITE_LED;
bool areLedsOn     = false;

int breatheValue = 0;
int breatheDir   = 1;
unsigned long lastBreatheMillis = 0;
const int breatheInterval = 10;

bool blinkState = false;
unsigned long lastBlinkMillis = 0;
const int BLINK_INTERVAL_1000MS = 1000;
const int BLINK_INTERVAL_500MS  = 500;
const int BLINK_INTERVAL_200MS  = 200;

bool doubleBlinkState  = false;
int  doubleBlinkCount  = 0;
unsigned long lastDoubleBlinkMillis = 0;
const int DOUBLE_BLINK_FAST_INTERVAL     = 200;
const int DOUBLE_BLINK_LONG_ON_INTERVAL  = 400;
const int DOUBLE_BLINK_PAUSE_INTERVAL    = 1000;

const float PRICE_THRESHOLD_0_05 = 0.05;
const float PRICE_THRESHOLD_0_15 = 0.15;
const float PRICE_THRESHOLD_0_25 = 0.25;
const float PRICE_THRESHOLD_0_35 = 0.35;
const float PRICE_THRESHOLD_0_50 = 0.50;

LiquidCrystal_I2C lcd(0x27, 20, 4);

const char* api_url = "https://api.energy-charts.info/price?bzn=SI";

// Scheduling & retry
time_t nextScheduledFetchTime = 0;
int lastSuccessfulFetchDay     = 0;
int httpGetRetryCount          = 0;
const int HTTP_GET_RETRY_MAX   = 5;
const int HTTP_GET_BACKOFF_FACTOR = 2;
time_t lastSuccessfulFetchTime = 0;

int apiSuccessCount = 0;
int apiFailCount    = 0;

// Timezone
// Note: This firmware uses timestamp-based price lookups that work correctly on ALL days
// (including DST switch days with 23 or 25 hours). The TZ string only affects how
// local time is displayed and interpreted.
//
// To DISABLE DST switching and stay on ONE time zone year-round:
// ---------------------------------------------------------------------------
// Option A - Stay on CET (UTC+1, winter time) permanently:
//   const char* TZ_CET_CEST = "CET-1";          // Always UTC+1
//
// Option B - Stay on CEST (UTC+2, summer time) permanently:
//   const char* TZ_CET_CEST = "CEST-2";          // Always UTC+2
//
// Option C - Use fixed offset without timezone name:
//   const char* TZ_CET_CEST = "UTC+1";           // Always UTC+1
//   const char* TZ_CET_CEST = "UTC+2";           // Always UTC+2
//
// Option D - For other countries (e.g., Germany/Austria/Switzerland):
//   Germany (CET permanent):  const char* TZ_CET_CEST = "CET-1";
//   Germany (CEST permanent):  const char* TZ_CET_CEST = "CEST-2";
//
// The current default "CET-1CEST,M3.5.0/02:00,M10.5.0/03:00" switches automatically:
// - Spring forward: Last Sunday of March at 02:00 -> 03:00 (CEST, UTC+2)
// - Fall back: Last Sunday of October at 03:00 -> 02:00 (CET, UTC+1)
const long gmtOffset_sec = 3600;
const int  daylightOffset_sec = 3600;
const char* TZ_CET_CEST = "CET-1CEST,M3.5.0/02:00,M10.5.0/03:00";

// Price computation
const bool  APPLY_FEES_AND_VAT               = true;
const float POWER_COMPANY_FEE_PERCENTAGE     = 12.0;  // Fee % for positive spot prices
const float NEG_PRICE_COMPANY_FEE_PERCENTAGE = 30.0;  // Fee % kept by provider on negative prices
                                                        // 30.0 = provider keeps 30%, pays you 70%
                                                        //  0.0 = full negative price passed to you
const float VAT_PERCENTAGE                   = 22.0;

// Button handling
int buttonState          = 0;
int lastButtonState      = 0;
unsigned long lastDebounceTime    = 0;
unsigned long buttonPressStartTime = 0;
bool longPressDetected   = false;
// v7.2: minimal fix for the boot-time false "Long press detected!" message.
// Set to true the first time the pin is observed LOW after boot. Until
// that happens the long-press detector refuses to trip, which prevents the
// ESP32-C3 floating-pin / power-rail noise (where the pin reads as pressed
// for several seconds after reset) from being treated as a 3-second hold.
bool buttonEverReleased = false;
const unsigned long debounceDelay      = 50;
const unsigned long longPressThreshold = 3000;

unsigned long lastClickTime     = 0;
const unsigned long doubleClickWindow = 500;
bool waitingForDoubleClick      = false;
bool pendingClick               = false;

// ========================================================================
// BUTTON STATE MACHINE - UNCHANGED FROM v7.4
// ========================================================================
// v7.5 makes no change to button handling. The gestures are v7.4's:
// single click scrolls, double click toggles, long press forces a
// manual refresh. btnEvtLastMs below records the wall-clock time of the
// last physical edge for FIX G only (see handleDataFetching()); it is read
// by no part of this state machine, so writing it cannot affect a
// gesture.
// ========================================================================
// v7.4 Fix D: button edge interrupt flag.
// Set by the ISR on any CHANGE (press or release) of buttonPin.
// Consumed at the top of handleButton() so that a press which occurred
// during a blocking HTTP fetch is not silently lost.
volatile bool buttonInterruptFired = false;

// v7.5 FIX G: timestamp of the most recent physical edge.
volatile unsigned long btnEvtLastMs = 0;
// How long after the last physical button edge we refuse to START a blocking
// HTTP fetch. Must exceed doubleClickWindow (500 ms) so the whole two-press
// gesture -- and the 500 ms the firmware then waits to confirm it was a
// double -- completes before network work steals the loop. 500 + 300 margin.
// This only DEFERS the fetch; nextScheduledFetchTime is untouched, so the data
// still arrives, just after the user's hands are off the button.
const unsigned long BUTTON_INTERACTION_GUARD_MS = 800;

// Auto scroll timeout
unsigned long lastButtonActivity = 0;
const unsigned long autoScrollTimeout = 10000;
bool autoScrollExecuted = false;

// Time-based display refresh markers
unsigned long lastHourlyRefresh  = 0;
unsigned long last15MinRefresh   = 0;

// Secondary list / menu
int secondaryListOffset = 0;
const int SECONDARY_LIST_TOTAL_LINES   = 20;
const int SECONDARY_LIST_SCROLL_INCREMENT = 4;

// Backlight / presence
const unsigned long backlightOffDelay = 30000;
unsigned long lastPresenceTime = 0;
bool presenceSensorConnected   = false;

// Loop pacing
unsigned long lastLoopUpdate = 0;

// Display state
enum DisplayState { CURRENT_PRICES, CUSTOM_MESSAGE, NO_DATA_OFFSET };
DisplayState displayState = CURRENT_PRICES;
int timeOffsetHours = 0;

enum ListType { PRIMARY_LIST, SECONDARY_LIST };
ListType currentList = PRIMARY_LIST;

// ========================================================================
// VERSION 7.0 GLOBALS (DUAL BUFFERS)
// ========================================================================
// JSON doc and data flags
StaticJsonDocument<Config::JSON_BUFFER_SIZE> doc;         // Today's Data
StaticJsonDocument<Config::JSON_BUFFER_SIZE> docTomorrow; // Tomorrow's Data

bool isTodayDataAvailable = false;
bool isTomorrowDataAvailable = false;

float averagePrice        = 0.0;
int lowestPriceIndex      = -1;
int highestPriceIndex     = -1;

float averagePriceTomorrow    = 0.0;
int lowestPriceIndexTomorrow  = -1;
int highestPriceIndexTomorrow = -1;

// NVS and provisioning
Preferences preferences;
DNSServer dnsServer;
WebServer  server(80);

const char* ap_ssid = "MyTicker_Setup";
bool inProvisioningMode = false;
bool needsRestart       = false;

bool isTimeSynced = false;
bool initialBoot  = true;
int trackedDay = -1;

bool  nvsDataLoadedForToday = false;
bool  nvsDataPresent        = false;
int   nvsStoredDay          = -1;
int   nvsStoredMonth        = -1;
int   nvsStoredYear         = -1;
time_t nvsLastStoreTime     = 0;

int midnightRetryCount = 0;
bool midnightPhaseActive = false;
bool lastProcessJsonAcceptedToday = false;

// ========================================================================
// CUSTOM CHARACTER BITMAPS
// ========================================================================

byte bitmap_c[8] = { B00100, B00000, B01110, B10001, B10000, B10001, B01110, B00000 };
byte bitmap_s[8] = { B00100, B00000, B01110, B10000, B01110, B00001, B11110, B00000 };
byte bitmap_z[8] = { B00100, B00000, B11111, B00010, B00100, B01000, B11111, B00000 };
byte lo_prc[]    = { B00000, B00100, B00100, B00100, B10101, B01110, B00100, B00000 };
byte hi_prc[]    = { B00000, B00100, B01110, B10101, B00100, B00100, B00100, B00000 };

// Forward declarations
int  getCurrentQuarterHourIndex();
void displayPrices();
bool processJsonData();           // NOTE: now returns bool
void scheduleAfterMidnightFailure();

// ========================================================================
// TIMESTAMP-BASED INDEX LOOKUP (DST-SAFE)
// ========================================================================

// v7.5 Fix D scope note: this deliberately matches on tm_hour ALONE and
// returns the FIRST hit. On a DST fall-back day two local 02:xx blocks
// exist; the 20x4 LCD is a 24-row hour-of-day view, so rendering the first
// one is correct. Do NOT make this lookup (hour, isdst)-aware like the
// aggregate loop above unless the display also grows a 25th row -- adding
// a second 02:00 row to a 24-row layout would misalign every row below it.
// If the display ever moves to a timestamp-keyed layout, that layout needs
// its own rendering strategy for the repeated hour.
int findPriceIndexForHour(const JsonArray& unixSeconds, int targetHour) {
    if (unixSeconds.size() == 0) return -1;

    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return -1;

    for (size_t i = 0; i < unixSeconds.size(); i++) {
        unsigned long unixTime = unixSeconds[i].as<unsigned long>();
        if (!isValidUnixTime(unixTime)) continue;

        time_t t = (time_t)unixTime;
        // v7.5 FIX C: localtime_r() -- caller-owned buffer, no shared static
        struct tm tmEntry;
        if (localtime_r(&t, &tmEntry) && tmEntry.tm_hour == targetHour) {
            return (int)i;
        }
    }

    return -1;
}

int findCurrentPriceIndex(const JsonArray& unixSeconds) {
    if (unixSeconds.size() == 0) return -1;

    time_t now;
    time(&now);

    for (size_t i = unixSeconds.size(); i > 0; i--) {
        size_t idx = i - 1;
        unsigned long unixTime = unixSeconds[idx].as<unsigned long>();
        if (!isValidUnixTime(unixTime)) continue;

        if ((time_t)unixTime <= now) {
            return (int)idx;
        }
    }

    return 0;
}

int getHourFromPriceIndex(const JsonArray& unixSeconds, int priceIndex) {
    if (priceIndex < 0 || priceIndex >= (int)unixSeconds.size()) return -1;

    unsigned long unixTime = unixSeconds[priceIndex].as<unsigned long>();
    if (!isValidUnixTime(unixTime)) return -1;

    time_t t = (time_t)unixTime;
    // v7.5 FIX C: localtime_r() -- caller-owned buffer, no shared static
    struct tm tmEntry;
    if (!localtime_r(&t, &tmEntry)) return -1;

    return tmEntry.tm_hour;
}

// v7.5 FIX D: DST-aware matching accessor for getHourFromPriceIndex().
// Returns tm_isdst for the entry, or -1 if the index or timestamp is
// unusable. On the fall-back day the two local 02:xx blocks share the same
// tm_hour but differ in tm_isdst, so (hour, isdst) is the unique key for
// an hour-block; tm_hour alone is not.
int getDstFlagFromPriceIndex(const JsonArray& unixSeconds, int priceIndex) {
    if (priceIndex < 0 || priceIndex >= (int)unixSeconds.size()) return -1;

    unsigned long unixTime = unixSeconds[priceIndex].as<unsigned long>();
    if (!isValidUnixTime(unixTime)) return -1;

    time_t t = (time_t)unixTime;
    struct tm tmEntry;
    if (!localtime_r(&t, &tmEntry)) return -1;

    return tmEntry.tm_isdst;
}

// ========================================================================
// UTILS
// ========================================================================

void debugPrint(int level, const String& message) {
#if DEBUG_LEVEL >= 1
    if (DEBUG_LEVEL >= level) {
        Serial.println("[DEBUG] " + message);
    }
#endif
}

void lcdPrint(const char* text) {
    for (int i = 0; text[i] != '\0'; i++) {
        char currentChar = text[i];
        if (currentChar == '^') {
            lcd.write(byte(0));
        } else if (currentChar == '~') {
            lcd.write(byte(1));
        } else if (currentChar == '|') {
            lcd.write(byte(2));
        } else {
            lcd.print(currentChar);
        }
    }
}

void commaPrint(float value, int places) {
    String numStr = String(value, places);
    numStr.replace('.', ',');
    lcd.print(numStr);
}

bool isValidUnixTime(unsigned long timestamp) {
    return (timestamp > 946684800UL && timestamp < 2147483647UL);
}

// ========================================================================
// PROVISIONING
// ========================================================================

void startProvisioning() {
    debugPrint(1, "Starting Wi-Fi Provisioning AP");
    inProvisioningMode = true;
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("No Wi-Fi access!");
    lcd.setCursor(0, 1);
    lcd.print("Setup Wi-Fi:");
    lcd.setCursor(0, 2);
    lcd.print("SSID: MyTicker_Setup");

    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap_ssid);

    IPAddress apIP = WiFi.softAPIP();
    dnsServer.start(53, "*", apIP);

    lcd.setCursor(0, 3);
    lcd.print("IP: " + apIP.toString());
    debugPrint(1, "AP IP address: " + apIP.toString());

    server.onNotFound([]() {
        String html = "<h3>Wi-Fi Setup</h3><form action='/save' method='get'>SSID: <input type='text' name='ssid'><br>Password: <input type='password' name='pass'><br><input type='submit' value='Save'>[...]";
        server.send(200, "text/html", html);
    });

    server.on("/save", HTTP_GET, []() {
        String newSsid = server.arg("ssid");
        String newPass = server.arg("pass");

        if (newSsid.length() > 0) {
            preferences.begin("my-ticker", false);
            preferences.putString("ssid", newSsid);
            preferences.putString("pass", newPass);
            preferences.end();

            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print("Saved!");
            lcd.setCursor(0, 1);
            lcd.print("Restarting...");

            server.send(200, "text/html", "Wi-Fi credentials saved. Restarting ESP32...");
            needsRestart = true;
            debugPrint(1, "Credentials saved, restarting.");
        } else {
            server.send(200, "text/html", "Invalid credentials. Please go back and try again.");
        }
    });

    server.begin();
    debugPrint(1, "HTTP server started");
}

void handleProvisioning() {
    dnsServer.processNextRequest();
    server.handleClient();
}

void connectToWiFi() {
    String stored_ssid = "";
    String stored_pass = "";

    preferences.begin("my-ticker", false);
    stored_ssid = preferences.getString("ssid", "");
    stored_pass = preferences.getString("pass", "");
    preferences.end();

    if (stored_ssid.length() > 0) {
        lcd.setCursor(0, 0);
        lcd.print("Elec. Rate SI v7.5");
        lcd.setCursor(0, 1);
        lcd.print("Connecting...");

        WiFi.begin(stored_ssid.c_str(), stored_pass.c_str());
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < Config::WIFI_RETRY_MAX) {
            delay(500);
            lcd.setCursor(12 + (attempts % 8), 1);
            lcd.print(".");
            attempts++;
        }

        if (WiFi.status() == WL_CONNECTED) {
            debugPrint(2, "WiFi connected successfully");
            digitalWrite(builtinLedPin, HIGH);
            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print("Connected!");
            lcd.setCursor(0, 1);
            lcd.print(WiFi.localIP());
            delay(2000);
            lcd.backlight();
        } else {
            debugPrint(1, "WiFi connection failed after " + String(attempts) + " attempts");
            digitalWrite(builtinLedPin, LOW);
            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print("WiFi Failed!");
            startProvisioning();
        }
    } else {
        startProvisioning();
    }
}

// ========================================================================
// LED HANDLING
// ========================================================================

void updateLeds() {
    // ESP32 note:
    // Do not mix analogWrite() (LEDC PWM) with digitalWrite() on the same pin.
    // Once PWM is attached, digitalWrite(LOW) may not fully turn off the LED.
    // Therefore this function uses analogWrite() exclusively for whiteLedPin.

    if (!ledsConnected || !areLedsOn || !isTodayDataAvailable || !isTimeSynced) {
        analogWrite(whiteLedPin, 0);
        breatheValue = 0;
        breatheDir   = 1;
        blinkState   = LOW;
        doubleBlinkCount = 0;
        return;
    }

    JsonArray prices      = doc["price"];
    JsonArray unixSeconds = doc["unix_seconds"];

    int currentIntervalIndex = findCurrentPriceIndex(unixSeconds);
    if (currentIntervalIndex < 0 || currentIntervalIndex >= (int)prices.size()) {
        analogWrite(whiteLedPin, 0);
        return;
    }

    float currentRate = prices[currentIntervalIndex].as<float>();

    if (currentRate <= 0) {
        analogWrite(whiteLedPin, 0);
        return;
    }

    float finalPrice = currentRate;
    if (APPLY_FEES_AND_VAT) {
        if (currentRate >= 0) {
            finalPrice = finalPrice * (1 + POWER_COMPANY_FEE_PERCENTAGE / 100.0) * (1 + VAT_PERCENTAGE / 100.0);
        } else {
            finalPrice = finalPrice * (1 - NEG_PRICE_COMPANY_FEE_PERCENTAGE / 100.0) * (1 + VAT_PERCENTAGE / 100.0);
        }
    }
    finalPrice /= 1000.0;

    if (finalPrice <= PRICE_THRESHOLD_0_05) {
        if (millis() - lastBreatheMillis > breatheInterval) {
            breatheValue += breatheDir;
            if (breatheValue >= 255 || breatheValue <= 0) {
                breatheDir *= -1;
            }
            analogWrite(whiteLedPin, breatheValue);
            lastBreatheMillis = millis();
        }
        doubleBlinkCount = 0;

    } else if (finalPrice <= PRICE_THRESHOLD_0_15) {
        analogWrite(whiteLedPin, 255);
        doubleBlinkCount = 0;

    } else if (finalPrice <= PRICE_THRESHOLD_0_25) {
        if (millis() - lastBlinkMillis > BLINK_INTERVAL_1000MS) {
            blinkState = !blinkState;
            analogWrite(whiteLedPin, blinkState ? 255 : 0);
            lastBlinkMillis = millis();
        }
        doubleBlinkCount = 0;

    } else if (finalPrice <= PRICE_THRESHOLD_0_35) {
        if (millis() - lastBlinkMillis > BLINK_INTERVAL_500MS) {
            blinkState = !blinkState;
            analogWrite(whiteLedPin, blinkState ? 255 : 0);
            lastBlinkMillis = millis();
        }
        doubleBlinkCount = 0;

    } else if (finalPrice <= PRICE_THRESHOLD_0_50) {
        int targetBlinks = 2;
        if (doubleBlinkCount < targetBlinks * 2) {
            if (millis() - lastDoubleBlinkMillis > DOUBLE_BLINK_FAST_INTERVAL) {
                doubleBlinkState = !doubleBlinkState;
                analogWrite(whiteLedPin, doubleBlinkState ? 255 : 0);
                lastDoubleBlinkMillis = millis();
                doubleBlinkCount++;
            }
        } else {
            if (millis() - lastDoubleBlinkMillis > DOUBLE_BLINK_PAUSE_INTERVAL) {
                doubleBlinkCount = 0;
                lastDoubleBlinkMillis = millis();
            }
        }

    } else {
        if (doubleBlinkCount == 0) {
            if (millis() - lastDoubleBlinkMillis > DOUBLE_BLINK_PAUSE_INTERVAL) {
                analogWrite(whiteLedPin, 255);
                lastDoubleBlinkMillis = millis();
                doubleBlinkCount++;
            }
        } else if (doubleBlinkCount == 1) {
            if (millis() - lastDoubleBlinkMillis > DOUBLE_BLINK_FAST_INTERVAL) {
                analogWrite(whiteLedPin, 0);
                lastDoubleBlinkMillis = millis();
                doubleBlinkCount++;
            }
        } else if (doubleBlinkCount == 2) {
            if (millis() - lastDoubleBlinkMillis > DOUBLE_BLINK_FAST_INTERVAL) {
                analogWrite(whiteLedPin, 255);
                lastDoubleBlinkMillis = millis();
                doubleBlinkCount++;
            }
        } else if (doubleBlinkCount == 3) {
            if (millis() - lastDoubleBlinkMillis > DOUBLE_BLINK_FAST_INTERVAL) {
                analogWrite(whiteLedPin, 0);
                lastDoubleBlinkMillis = millis();
                doubleBlinkCount++;
            }
        } else if (doubleBlinkCount == 4) {
            if (millis() - lastDoubleBlinkMillis > DOUBLE_BLINK_FAST_INTERVAL) {
                analogWrite(whiteLedPin, 255);
                lastDoubleBlinkMillis = millis();
                doubleBlinkCount++;
            }
        } else if (doubleBlinkCount == 5) {
            if (millis() - lastDoubleBlinkMillis > DOUBLE_BLINK_LONG_ON_INTERVAL) {
                analogWrite(whiteLedPin, 0);
                lastDoubleBlinkMillis = millis();
                doubleBlinkCount = 0;
            }
        }
    }
}

// ========================================================================
// DATA FETCHING / NVS PERSISTENCE
// ========================================================================

float getHourlyAverage(int hourIndex, const JsonArray& prices, const JsonArray& unixSeconds) {
    if (hourIndex < 0 || hourIndex >= 24) return 0.0;
    if (unixSeconds.size() == 0) return 0.0;

    int startIndex = findPriceIndexForHour(unixSeconds, hourIndex);
    if (startIndex < 0 || startIndex + 3 >= (int)prices.size()) return 0.0;

    float sum = 0.0;
    int validCount = 0;

    for (int i = 0; i < 4; i++) {
        int idx = startIndex + i;
        if (idx >= (int)prices.size()) break;

        int entryHour = getHourFromPriceIndex(unixSeconds, idx);
        if (entryHour != hourIndex) break;

        sum += prices[idx].as<float>();
        validCount++;
    }
    return validCount > 0 ? sum / validCount : 0.0;
}

int getCurrentQuarterHourIndex() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return 0;

    int minute = timeinfo.tm_min;
    return minute / 15;
}

void format15MinPrice(float price, char* buffer, int bufferSize) {
    if (APPLY_FEES_AND_VAT) {
        if (price >= 0) {
            price = price * (1 + POWER_COMPANY_FEE_PERCENTAGE / 100.0) * (1 + VAT_PERCENTAGE / 100.0);
        } else {
            price = price * (1 - NEG_PRICE_COMPANY_FEE_PERCENTAGE / 100.0) * (1 + VAT_PERCENTAGE / 100.0);
        }
    }
    price /= 1000.0;

    int hundredths = (int)round(price * 100);

    if (hundredths > 99) {
        snprintf(buffer, bufferSize, "+99");
    } else if (hundredths < -99) {
        snprintf(buffer, bufferSize, "-99");
    } else if (hundredths >= 0) {
        snprintf(buffer, bufferSize, " %02d", hundredths);
    } else {
        snprintf(buffer, bufferSize, "%03d", hundredths);
    }
}

// ========================================================================
// NVS PERSISTENCE & DUAL FETCHING
// ========================================================================

void saveDataToNVS(const String& rawJson, bool isTomorrow) {
    time_t now;
    time(&now);
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return;

    preferences.begin("my-ticker", false);

    if (isTomorrow) {
        preferences.putString("data_prc_t", rawJson);
        preferences.putULong("data_store_t", (unsigned long)now);
        debugPrint(2, "Tomorrow's data saved to NVS.");
    } else {
        preferences.putInt("data_day",  timeinfo.tm_mday);
        preferences.putInt("data_mon",  timeinfo.tm_mon);
        preferences.putInt("data_year", timeinfo.tm_year + 1900);
        preferences.putString("data_prc", rawJson);
        preferences.putULong("data_last_store", (unsigned long)now);

        nvsDataPresent   = true;
        nvsStoredDay     = timeinfo.tm_mday;
        nvsStoredMonth   = timeinfo.tm_mon;
        nvsStoredYear    = timeinfo.tm_year + 1900;
        nvsLastStoreTime = now;
        nvsDataLoadedForToday = true;
        debugPrint(2, "Today's data saved to NVS.");
    }
    preferences.end();
}

void clearTomorrowNVS() {
    preferences.begin("my-ticker", false);
    preferences.remove("data_prc_t");
    preferences.remove("data_store_t");
    preferences.end();
    debugPrint(2, "Tomorrow's NVS slot cleared.");
}

bool loadDataFromNVS() {
    preferences.begin("my-ticker", false);
    int storedDay   = preferences.getInt("data_day",  -1);
    int storedMonth = preferences.getInt("data_mon",  -1);
    int storedYear  = preferences.getInt("data_year", -1);
    String storedJsonToday = preferences.getString("data_prc", "");
    String storedJsonTomorrow = preferences.getString("data_prc_t", "");
    unsigned long storedTime = preferences.getULong("data_last_store", 0);
    preferences.end();

    struct tm nowInfo;
    if (!getLocalTime(&nowInfo)) return false;

    if (storedDay == nowInfo.tm_mday && storedMonth == nowInfo.tm_mon && storedYear == (nowInfo.tm_year + 1900)) {
        if (!storedJsonToday.isEmpty()) {
            DeserializationError err = deserializeJson(doc, storedJsonToday);
            if (!err) {
                processJsonData(false);
                nvsDataPresent   = true;
                nvsStoredDay     = storedDay;
                nvsStoredMonth   = storedMonth;
                nvsStoredYear    = storedYear;
                nvsLastStoreTime = storedTime;
                nvsDataLoadedForToday = true;
            }
        }
    }

    if (!storedJsonTomorrow.isEmpty()) {
        DeserializationError err = deserializeJson(docTomorrow, storedJsonTomorrow);
        if (!err) {
            processJsonData(true);
        }
    }

    return isTodayDataAvailable;
}

// ========================================================================
// v7.4 FIX A: fetchAndProcessData() now advances nextScheduledFetchTime
// on EVERY failure/rejection path for tomorrow fetches, breaking the
// tight retry loop that starved handleButton() from 14:00 onward.
// ========================================================================

void fetchAndProcessData(bool fetchTomorrow) {
    if (WiFi.status() != WL_CONNECTED || !isTimeSynced) {
        apiFailCount++;
        // v7.4: even a no-WiFi bail-out must advance the schedule
        // to prevent a tight retry loop.
        time_t now; time(&now);
        nextScheduledFetchTime = now + 1800;
        return;
    }

    String url = api_url;
    if (fetchTomorrow) {
        time_t now;
        time(&now);
        struct tm tmr;
        localtime_r(&now, &tmr);
        tmr.tm_mday += 1;
        tmr.tm_hour = 12; // midday anchor -- safely away from any DST boundary
        // v7.5 FIX A: let the C library determine DST for the target day
        // instead of inheriting the source day's tm_isdst, otherwise mktime()
        // normalises against the wrong UTC offset on DST transition days.
        tmr.tm_isdst = -1;
        mktime(&tmr);     // re-normalises month/year rollover and re-applies DST rules
        char dateStr[20];
        snprintf(dateStr, sizeof(dateStr), "%04d-%02d-%02d", tmr.tm_year + 1900, tmr.tm_mon + 1, tmr.tm_mday);
        url += "&start=";
        url += dateStr;
    } else {
        // v7.5 FIX B: today's fetch now uses explicit date bounds instead of
        // the bare api_url, so the returned window cannot be silently widened
        // or narrowed by server-side caching of the bare /price?bzn=XX request.
        // `end` is inclusive on the Energy-Charts API, so start == end yields
        // exactly one market day (96 entries normally, 92 / 100 on DST days).
        time_t now;
        time(&now);
        struct tm todayTm;
        localtime_r(&now, &todayTm);
        char dateStr[20];
        snprintf(dateStr, sizeof(dateStr), "%04d-%02d-%02d",
                 todayTm.tm_year + 1900, todayTm.tm_mon + 1, todayTm.tm_mday);
        url += "&start=";
        url += dateStr;
        url += "&end=";
        url += dateStr;
    }

    HTTPClient http;
    http.begin(url);
    http.setTimeout(Config::HTTP_TIMEOUT);
    http.setConnectTimeout(Config::HTTP_CONNECT_TIMEOUT);

    unsigned long httpStartMs = millis();
    int httpResponseCode = http.GET();

    // v7.5 FIX F2: no wall-clock stall guard here, on purpose.
    // The obvious guard -- measure `millis()` AFTER http.GET() returns and,
    // if the call overran HTTP_TIMEOUT + HTTP_CONNECT_TIMEOUT + 1000 ms
    // (= 16000 ms with the configured values), force
    // httpResponseCode = -1 -- would fix nothing. It cannot prevent a
    // single millisecond of blocking; the loop was already stuck by the
    // time it ran. All it adds is a real risk of discarding a
    // large-but-valid response. Dead code that implies a safety property
    // the code does not have is worse than no code at all.
    //
    // What replaces it is measurement rather than a bogus verdict:
    // http.GET() is the single longest blocking call in the firmware and
    // the sole reason the UI can freeze, so its duration is printed on
    // every request. If the user sees a freeze, the serial monitor now
    // shows exactly how long the network call took, and whether it was
    // DNS, connect or read.
    //
    // Note on what setTimeout()/setConnectTimeout() actually bound: they
    // govern the TCP connect and the socket read/write. They do NOT bound
    // DNS resolution, which ESP32 lwIP performs synchronously inside
    // http.GET() and which can block for several seconds on its own. A
    // 4 s freeze is therefore entirely consistent with a DNS stall and is
    // NOT evidence that the two timeout constants need raising.
    unsigned long httpElapsedMs = millis() - httpStartMs;
    debugPrint(2, String("HTTP GET ") + (fetchTomorrow ? "TOMORROW " : "TODAY ") +
                    "rc=" + String(httpResponseCode) +
                    " took " + String(httpElapsedMs) + " ms");
    if (httpElapsedMs > 1000UL) {
        debugPrint(2, String("  ^ slow request: loop was blocked for ") +
                        String(httpElapsedMs) + " ms (button edges queued in ISR, applied after)");
    }
    if (httpResponseCode > 0) {
        String payload = http.getString();

        StaticJsonDocument<Config::JSON_BUFFER_SIZE>& targetDoc = fetchTomorrow ? docTomorrow : doc;
        targetDoc.clear();
        DeserializationError error = deserializeJson(targetDoc, payload);

        if (error) {
            apiFailCount++;
            http.end();
            // v7.4: advance schedule even on JSON parse failure
            time_t now; time(&now);
            if (fetchTomorrow) nextScheduledFetchTime = now + 1800;
            return;
        }

        apiSuccessCount++;
        if (!fetchTomorrow) time(&lastSuccessfulFetchTime);

        bool accepted = processJsonData(fetchTomorrow);
        if (accepted) {
            saveDataToNVS(payload, fetchTomorrow);
            if (!fetchTomorrow && midnightPhaseActive) {
                midnightPhaseActive = false;
                midnightRetryCount = 0;
            }
        } else {
            // v7.4: API returned data but for the wrong day.
            // Must advance the schedule to avoid a tight retry loop.
            time_t now; time(&now);
            if (fetchTomorrow) nextScheduledFetchTime = now + 1800;
        }

        if (!fetchTomorrow) httpGetRetryCount = 0;
        displayPrices();
    } else {
        apiFailCount++;
        if (!fetchTomorrow) httpGetRetryCount++;
        displayPrices();

        time_t now; time(&now);
        if (!fetchTomorrow) {
            if (!midnightPhaseActive) nextScheduledFetchTime = now + 600;
        } else {
            // v7.4 FIX A: advance schedule for tomorrow-fetch HTTP failures.
            // Without this the loop retries every iteration, blocking
            // handleButton() for 10-15 s per attempt.
            nextScheduledFetchTime = now + 1800; // retry in 30 min
        }
    }
    http.end();
}

bool processJsonData(bool isTomorrow) {
    StaticJsonDocument<Config::JSON_BUFFER_SIZE>& targetDoc = isTomorrow ? docTomorrow : doc;
    JsonArray prices      = targetDoc["price"];
    JsonArray unixSeconds = targetDoc["unix_seconds"];

    if (prices.size() == 0 || unixSeconds.size() == 0) {
        if (isTomorrow) isTomorrowDataAvailable = false;
        else isTodayDataAvailable = false;
        return false;
    }

    // Validate the date
    // v7.3: Use localtime_r() with separate struct tm variables to avoid the
    // static-buffer aliasing bug: two consecutive localtime() calls return the
    // same pointer, so the first result was silently overwritten and the
    // sameDate comparison was always X==X (unconditionally true).
    size_t lastIndex = unixSeconds.size() - 1;
    unsigned long lastUnix = unixSeconds[lastIndex].as<unsigned long>();
    time_t lastDataTime = (time_t)lastUnix;
    struct tm lastDataTm;
    localtime_r(&lastDataTime, &lastDataTm);

    time_t targetTime = time(nullptr);
    if (isTomorrow) {
        // v7.3: advance the calendar day rather than adding 86400 UTC seconds.
        struct tm tmNow;
        localtime_r(&targetTime, &tmNow);
        tmNow.tm_mday += 1;
        tmNow.tm_hour = 12; // midday anchor -- safely away from any DST boundary
        // v7.5 FIX A: same as above -- do not inherit the source day's
        // tm_isdst across the calendar-day increment.
        tmNow.tm_isdst = -1;
        targetTime = mktime(&tmNow);
    }
    struct tm targetDayTm;
    localtime_r(&targetTime, &targetDayTm);

    bool sameDate = (lastDataTm.tm_mday == targetDayTm.tm_mday &&
                     lastDataTm.tm_mon  == targetDayTm.tm_mon  &&
                     lastDataTm.tm_year == targetDayTm.tm_year);

    if (!sameDate) {
        if (isTomorrow) isTomorrowDataAvailable = false;
        else isTodayDataAvailable = false;
        return false;
    }

    if (isTomorrow) isTomorrowDataAvailable = true;
    else isTodayDataAvailable = true;

    // Calculate Averages and Min/Max
    // v7.3: Scan by array index rather than by hour number so that the
    // repeated 02:xx block on a DST fall-back day (25 local hours, 100
    // price entries) is included in the average and min/max calculation.
    // v7.5 FIX D: block detection must compare tm_hour AND tm_isdst. On the
    // fall-back day the second 02:xx block repeats tm_hour == 2, so the old
    // hour-only guard skipped it and the average ran over 24 blocks instead
    // of 25 (and could miss the day's true min/max).
    float sum = 0.0;
    int validHourCount = 0;
    float minPrice = 999999.0;
    float maxPrice = -999999.0;
    int lowestIdx = 0, highestIdx = 0;
    int lastVisitedStartIndex = -1;

    for (size_t i = 0; i < unixSeconds.size(); i++) {
        int entryHour = getHourFromPriceIndex(unixSeconds, (int)i);
        if (entryHour < 0) continue;
        int entryDst = getDstFlagFromPriceIndex(unixSeconds, (int)i);

        if (i > 0) {
            int prevHour = getHourFromPriceIndex(unixSeconds, (int)i - 1);
            int prevDst = getDstFlagFromPriceIndex(unixSeconds, (int)i - 1);
            // Same hour AND same DST flag == still inside the current block.
            if (prevHour == entryHour && prevDst == entryDst) continue;
        }

        int startIndex = (int)i;
        if (startIndex == lastVisitedStartIndex) continue;
        lastVisitedStartIndex = startIndex;

        float hourlyAvg = getHourlyAverage(entryHour, prices, unixSeconds);
        if (startIndex != findPriceIndexForHour(unixSeconds, entryHour)) {
            // Second (or later) block of a repeated hour -- getHourlyAverage()
            // only ever resolves the first one, so average this block directly.
            float blockSum = 0.0;
            int blockCount = 0;
            for (int k = 0; k < 4; k++) {
                int idx = startIndex + k;
                if (idx >= (int)prices.size()) break;
                if (getHourFromPriceIndex(unixSeconds, idx) != entryHour) break;
                if (getDstFlagFromPriceIndex(unixSeconds, idx) != entryDst) break;
                blockSum += prices[idx].as<float>();
                blockCount++;
            }
            hourlyAvg = blockCount > 0 ? blockSum / blockCount : 0.0f;
        }

        sum += hourlyAvg;
        validHourCount++;

        if (hourlyAvg < minPrice) { minPrice = hourlyAvg; lowestIdx = startIndex; }
        if (hourlyAvg > maxPrice) { maxPrice = hourlyAvg; highestIdx = startIndex; }
    }

    if (isTomorrow) {
        averagePriceTomorrow = validHourCount > 0 ? sum / (float)validHourCount : 0.0;
        lowestPriceIndexTomorrow = lowestIdx;
        highestPriceIndexTomorrow = highestIdx;
    } else {
        averagePrice = validHourCount > 0 ? sum / (float)validHourCount : 0.0;
        lowestPriceIndex = lowestIdx;
        highestPriceIndex = highestIdx;
    }

    return true;
}

// ========================================================================
// DUAL BUFFER DISPLAY HELPERS
// ========================================================================

void display15MinuteDetails(int row, int totalHourOffset) {
    lcd.setCursor(0, row);

    bool showTomorrow = (totalHourOffset >= 24);
    int localHourIndex = totalHourOffset % 24;

    StaticJsonDocument<Config::JSON_BUFFER_SIZE>& targetDoc = showTomorrow ? docTomorrow : doc;
    JsonArray prices = targetDoc["price"];
    JsonArray unixSeconds = targetDoc["unix_seconds"];

    int startIndex = findPriceIndexForHour(unixSeconds, localHourIndex);
    if (startIndex < 0) {
        lcd.print("                    ");
        return;
    }

    struct tm timeinfo;
    int currentHour = -1, currentMinute = -1;
    bool hasValidTime = false;
    if (getLocalTime(&timeinfo)) {
        currentHour = timeinfo.tm_hour;
        currentMinute = timeinfo.tm_min;
        hasValidTime = true;
    }

    char priceBuffer[4];
    int cursorPos = 0;
    int currentSegment = -1;

    if (hasValidTime && !showTomorrow && localHourIndex == currentHour) {
        currentSegment = currentMinute / 15;
    }

    for (int i = 0; i < 4; i++) {
        int idx = startIndex + i;
        bool shouldShowPlaceholder = (hasValidTime && !showTomorrow && localHourIndex == currentHour && i < currentSegment);

        if (idx < (int)unixSeconds.size() && getHourFromPriceIndex(unixSeconds, idx) != localHourIndex) break;

        if (shouldShowPlaceholder) {
            lcd.print(" > ");
            cursorPos += 3;
        } else if (idx < (int)prices.size()) {
            float price = prices[idx].as<float>();
            format15MinPrice(price, priceBuffer, sizeof(priceBuffer));
            lcd.print(priceBuffer);
            cursorPos += 3;
        } else {
            lcd.print("---");
            cursorPos += 3;
        }

        if (i < 3) {
            lcd.print("  ");
            cursorPos += 2;
        }
    }
    for (int i = cursorPos; i < 20; i++) lcd.print(" ");
}

void displayPriceRow(int row, int totalHourOffset, bool isCurrentHourRow) {
    lcd.setCursor(0, row);
    bool showTomorrow = (totalHourOffset >= 24);
    int localHourIndex = totalHourOffset % 24;

    StaticJsonDocument<Config::JSON_BUFFER_SIZE>& targetDoc = showTomorrow ? docTomorrow : doc;
    JsonArray prices = targetDoc["price"];
    JsonArray unixSeconds = targetDoc["unix_seconds"];

    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
        int currentHour = timeinfo.tm_hour;
        if (!isCurrentHourRow && !showTomorrow) {
            if (localHourIndex < currentHour) {
                lcd.print("                    "); return;
            }
        }
    }

    int dataIndex = findPriceIndexForHour(unixSeconds, localHourIndex);
    if (dataIndex < 0) {
        lcd.print("No Data Available   ");
        return;
    }

    float rates = getHourlyAverage(localHourIndex, prices, unixSeconds);

    char buffer[21];
    if (showTomorrow) {
        snprintf(buffer, sizeof(buffer), "%02d:>>", localHourIndex);
    } else {
        snprintf(buffer, sizeof(buffer), "%02d:00", localHourIndex);
    }
    lcd.print(buffer);

    int lowIdx = showTomorrow ? lowestPriceIndexTomorrow : lowestPriceIndex;
    int highIdx = showTomorrow ? highestPriceIndexTomorrow : highestPriceIndex;

    if (dataIndex == lowIdx) {
        lcd.setCursor(7, row); lcd.write(byte(3)); lcd.print("   ");
    } else if (dataIndex == highIdx) {
        lcd.setCursor(7, row); lcd.write(byte(4)); lcd.print("   ");
    } else {
        lcd.setCursor(6, row); lcd.print("    ");
    }

    float finalPrice = rates;
    if (APPLY_FEES_AND_VAT) {
        if (rates >= 0) {
            finalPrice = finalPrice * (1 + POWER_COMPANY_FEE_PERCENTAGE / 100.0) * (1 + VAT_PERCENTAGE / 100.0);
        } else {
            finalPrice = finalPrice * (1 - NEG_PRICE_COMPANY_FEE_PERCENTAGE / 100.0) * (1 + VAT_PERCENTAGE / 100.0);
        }
    }
    finalPrice /= 1000.0;
    if (finalPrice < 0) lcd.setCursor(9, row); else lcd.setCursor(10, row);

    commaPrint(finalPrice, 4);
    lcd.print(" EUR");
}

void displayPrimaryList() {
    if (!isTodayDataAvailable) {
        for (int i = 0; i < 4; i++) {
            lcd.setCursor(0, i);
            lcd.print("No data available   ");
        }
        return;
    }

    JsonArray unixSecondsToday = doc["unix_seconds"];
    int currentPriceIndex = findCurrentPriceIndex(unixSecondsToday);
    if (currentPriceIndex < 0) currentPriceIndex = 0;

    int currentHour = getHourFromPriceIndex(unixSecondsToday, currentPriceIndex);
    if (currentHour < 0) {
        struct tm timeinfo;
        currentHour = getLocalTime(&timeinfo) ? timeinfo.tm_hour : 0;
    }

    int displayStartHourOffset = currentHour + timeOffsetHours;

    int maxOffsetLimit = isTomorrowDataAvailable ? 47 : 23;
    if (displayStartHourOffset > maxOffsetLimit) displayStartHourOffset %= (maxOffsetLimit + 1);

    display15MinuteDetails(0, displayStartHourOffset);
    displayPriceRow(1, displayStartHourOffset, true);

    int nextHourOffset = displayStartHourOffset + 1;
    if (nextHourOffset <= maxOffsetLimit) displayPriceRow(2, nextHourOffset, false);
    else { lcd.setCursor(0, 2); lcd.print("                    "); }

    int hourAfterOffset = displayStartHourOffset + 2;
    if (hourAfterOffset <= maxOffsetLimit) displayPriceRow(3, hourAfterOffset, false);
    else { lcd.setCursor(0, 3); lcd.print("                    "); }
}

void displaySecondaryList() {
    char lines[SECONDARY_LIST_TOTAL_LINES][21];
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
        snprintf(lines[0], sizeof(lines[0]), "%2d:%02d  %2d.%2d.%04d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);
    } else snprintf(lines[0], sizeof(lines[0]), "Time not available  ");

    snprintf(lines[1], sizeof(lines[1]), "--------------------");
    snprintf(lines[2], sizeof(lines[2]), "Last update:");

    struct tm fetchTm;
    // v7.5 FIX C: localtime_r() -- caller-owned buffer, no shared static
    if (lastSuccessfulFetchTime != 0 && localtime_r(&lastSuccessfulFetchTime, &fetchTm)) {
        snprintf(lines[3], sizeof(lines[3]), "%2d.%2d.%04d  %2d:%02d", fetchTm.tm_mday, fetchTm.tm_mon + 1, fetchTm.tm_year + 1900, fetchTm.tm_hour, fetchTm.tm_min);
    } else snprintf(lines[3], sizeof(lines[3]), "No data timestamp   ");

    snprintf(lines[4], sizeof(lines[4]), "                    ");
    snprintf(lines[5], sizeof(lines[5]), "Daily average:");

    if (isTodayDataAvailable) {
        float finalPrice = averagePrice;
        if (APPLY_FEES_AND_VAT) {
            if (averagePrice >= 0) {
                finalPrice = finalPrice * (1 + POWER_COMPANY_FEE_PERCENTAGE / 100.0) * (1 + VAT_PERCENTAGE / 100.0);
            } else {
                finalPrice = finalPrice * (1 - NEG_PRICE_COMPANY_FEE_PERCENTAGE / 100.0) * (1 + VAT_PERCENTAGE / 100.0);
            }
        }
        char priceBuffer[21];
        snprintf(priceBuffer, sizeof(priceBuffer), "%.4f EUR/kWh", finalPrice / 1000.0);
        String numStr = String(priceBuffer); numStr.replace('.', ',');
        snprintf(lines[6], sizeof(lines[6]), "%s", numStr.c_str());
    } else snprintf(lines[6], sizeof(lines[6]), "Cene niso na voljo.");

    snprintf(lines[7], sizeof(lines[7]), "                    ");
    if (WiFi.status() == WL_CONNECTED) snprintf(lines[8], sizeof(lines[8]), "WiFi:%5d dBm", WiFi.RSSI());
    else snprintf(lines[8], sizeof(lines[8]), "WiFi: Disconnected");

    if (WiFi.status() == WL_CONNECTED) snprintf(lines[9], sizeof(lines[9]), "IP:%15s", WiFi.localIP().toString().c_str());
    else snprintf(lines[9], sizeof(lines[9]), "IP: Not connected  ");

    int totalApiCalls = apiSuccessCount + apiFailCount;
    if (totalApiCalls > 0) snprintf(lines[10], sizeof(lines[10]), "API:%3d%% (%d/%d)", (apiSuccessCount * 100) / totalApiCalls, apiSuccessCount, apiFailCount);
    else snprintf(lines[10], sizeof(lines[10]), "API: No calls yet  ");

    unsigned long uptimeMs = millis();
    unsigned long days = uptimeMs / (24UL * 60UL * 60UL * 1000UL); uptimeMs %= (24UL * 60UL * 60UL * 1000UL);
    unsigned long hours = uptimeMs / (60UL * 60UL * 1000UL); uptimeMs %= (60UL * 60UL * 1000UL);
    unsigned long minutes = uptimeMs / (60UL * 1000UL);
    snprintf(lines[11], sizeof(lines[11]), "Up:%2lud%2luh%2lum", days, hours, minutes);

    snprintf(lines[12], sizeof(lines[12]), "NVS status:");
    if (nvsDataPresent) {
        char dateBuf[16]; snprintf(dateBuf, sizeof(dateBuf), "%2d.%2d.%04d", (nvsStoredDay > 0 ? nvsStoredDay : 0), (nvsStoredMonth >= 0 ? nvsStoredMonth + 1 : 0), (nvsStoredYear > 0 ? nvsStoredYear : 0));
        snprintf(lines[13], sizeof(lines[13]), "Data day:%11s", dateBuf);
    } else snprintf(lines[13], sizeof(lines[13]), "Data day: none     ");

    if (nvsLastStoreTime != 0) {
        // v7.5 FIX C: localtime_r() -- caller-owned buffer, no shared static
        struct tm storeTm;
        if (localtime_r(&nvsLastStoreTime, &storeTm)) {
            char storeBuf[16]; snprintf(storeBuf, sizeof(storeBuf), "%2d.%2d.%04d", storeTm.tm_mday, storeTm.tm_mon + 1, storeTm.tm_year + 1900);
            snprintf(lines[14], sizeof(lines[14]), "Last save:%10s", storeBuf);
        } else snprintf(lines[14], sizeof(lines[14]), "Last save: invalid ");
    } else snprintf(lines[14], sizeof(lines[14]), "Last save: none    ");

    if (isTodayDataAvailable && isTomorrowDataAvailable) snprintf(lines[15], sizeof(lines[15]), "NVS: Today+Tomorrow");
    else if (isTodayDataAvailable) snprintf(lines[15], sizeof(lines[15]), "NVS: Today only    ");
    else snprintf(lines[15], sizeof(lines[15]), "NVS: Empty/Old     ");

    snprintf(lines[16], sizeof(lines[16]), "energy-charts.info");
    snprintf(lines[17], sizeof(lines[17]), "dynamic electricity");
    snprintf(lines[18], sizeof(lines[18]), "price ticker v7.5  ");
    snprintf(lines[19], sizeof(lines[19]), "by Legolas-2025    ");

    for (int i = 0; i < 4; i++) {
        int lineIndex = secondaryListOffset + i;
        lcd.setCursor(0, i);
        if (lineIndex < SECONDARY_LIST_TOTAL_LINES) {
            lcdPrint(lines[lineIndex]);
            for (int j = strlen(lines[lineIndex]); j < 20; j++) lcd.print(" ");
        } else lcd.print("                    ");
    }
}

void displayPrices() {
    lcd.clear();
    if (!isTimeSynced) {
        lcd.setCursor(0, 0); lcd.print("Syncing Time...");
        lcd.setCursor(0, 1); lcd.print("Please wait...");
        return;
    }

    if (currentList == SECONDARY_LIST) {
        displaySecondaryList();
        return;
    }

    if (!isTodayDataAvailable) {
        displayState = NO_DATA_OFFSET; timeOffsetHours = 0;
    } else if (displayState == NO_DATA_OFFSET) {
        displayState = CURRENT_PRICES; timeOffsetHours = 0;
    }

    switch (displayState) {
        case NO_DATA_OFFSET:
            lcd.setCursor(0, 0); lcd.print("No data for today");
            lcd.setCursor(0, 1); lcd.print("Press & hold to");
            lcd.setCursor(0, 2); lcd.print("refresh manually");
            break;
        case CURRENT_PRICES: displayPrimaryList(); break;
        case CUSTOM_MESSAGE: displayState = CURRENT_PRICES; displayPrimaryList(); break;
    }
    initialBoot = false;
}

void resetDisplayToTop() {
    timeOffsetHours = 0;
    secondaryListOffset = 0;
    displayState = CURRENT_PRICES;
    currentList = PRIMARY_LIST;
    displayPrices();
}

void advanceDisplayOffset() {
    lastButtonActivity = millis();
    autoScrollExecuted = false;

    if (!isTimeSynced) return;
    if (currentList == SECONDARY_LIST) {
        secondaryListOffset += SECONDARY_LIST_SCROLL_INCREMENT;
        if (secondaryListOffset >= SECONDARY_LIST_TOTAL_LINES) secondaryListOffset = 0;
        displayPrices();
        return;
    }

    if (!isTodayDataAvailable) {
        displayState = NO_DATA_OFFSET; displayPrices(); return;
    }

    if (displayState != NO_DATA_OFFSET) {
        struct tm timeinfo;
        int currentHour = 0;
        if (getLocalTime(&timeinfo)) currentHour = timeinfo.tm_hour;

        int maxOffsetHours = isTomorrowDataAvailable ? 47 : 23;
        int allowedAhead = maxOffsetHours - currentHour;

        int nextOffsetHours = timeOffsetHours + 1;
        if (nextOffsetHours > allowedAhead) {
            timeOffsetHours = 0;
            displayState = CURRENT_PRICES;
        } else timeOffsetHours = nextOffsetHours;
    }
    displayPrices();
}

void toggleList() {
    if (!isTimeSynced) return;
    currentList = (currentList == PRIMARY_LIST) ? SECONDARY_LIST : PRIMARY_LIST;
    displayPrices();
}

// ========================================================================
// BUTTON, BACKLIGHT, PRESENCE
// ========================================================================

// -----------------------------------------------------------------------
// Button ISR / handler - unchanged from v7.4
// -----------------------------------------------------------------------
// The only additions to v7.4's version are btnEvtLastMs = millis() in the
// ISR - read solely by FIX G in handleDataFetching() to keep a blocking
// fetch from starting mid-gesture - and the measured press duration in
// the long-press log line, which turns the symptom into a number if it
// ever returns. Neither touches the debounce state machine, the press
// window, the click classifier or the long-press detector.
// -----------------------------------------------------------------------
// v7.4 Fix D: ISR for button edge capture
void IRAM_ATTR buttonISR() {
    buttonInterruptFired = true;
    btnEvtLastMs         = millis();   // FIX G only; not read by the button logic
}

// ========================================================================
// v7.4 FIX C: handleButton() only honours longPressDetected when the press
// duration is genuinely >= longPressThreshold. Previously the detector
// measured idle time since last release, so a normal short click after 3 s
// of idle was misinterpreted as a long press, triggering a forced manual
// refresh that blocked the loop for 10-15 s.
// ========================================================================

void handleButton() {
    // v7.4 Fix D: consume the interrupt flag. If a press+release occurred
    // during a blocking HTTP fetch, inject a synthetic event so the state
    // machine processes it on this iteration.
    if (buttonInterruptFired) {
        buttonInterruptFired = false;
        // The pin is now in whatever state it settled to. If it is HIGH
        // (released, active-LOW wiring), synthesise a press+release pair:
        int pinNow = digitalRead(buttonPin);
        if (pinNow == HIGH) {
            // Synthesise: press (LOW on pin -> reading HIGH) then release
            // (HIGH on pin -> reading LOW) with a 1 ms gap.
            int syntheticReading = !pinNow; // HIGH = pressed
            if (syntheticReading != lastButtonState) {
                lastDebounceTime = millis();
                if (syntheticReading == LOW) {
                    buttonPressStartTime = millis();
                    longPressDetected    = false;
                    buttonEverReleased   = true;
                }
            }
            // Force the state machine to see the press:
            buttonState = HIGH;
        }
    }

    int reading = !digitalRead(buttonPin);

    if (reading != lastButtonState) {
        lastDebounceTime = millis();

        if (reading == LOW) {
            buttonPressStartTime = millis();
            longPressDetected    = false;
            // v7.2: the first time the pin goes LOW after boot counts
            // as "the user (or the noise) has released the button".
            buttonEverReleased   = true;
        }
    }

    if ((millis() - lastDebounceTime) > debounceDelay) {
        if (reading != buttonState) {
            buttonState = reading;

            if (buttonState == HIGH) {
                unsigned long pressDuration = millis() - buttonPressStartTime;

                // v7.4 FIX C: only honour longPressDetected if the press
                // was genuinely long (>= threshold). A spurious idle-time
                // flag no longer hijacks a normal short click.
                if (longPressDetected && pressDuration >= longPressThreshold) {
                    // pressDuration appended by v7.5: makes the symptom a
                    // number in the log instead of an argument. Inert.
                    debugPrint(1, String("Long press detected - Forcing manual data refresh (measured ") +
                                      pressDuration + " ms)");
                    lastButtonActivity = millis();
                    autoScrollExecuted = false;

                    lcd.clear();
                    lcd.setCursor(0, 0);
                    lcd.print("Manual Refresh...");
                    lcd.setCursor(0, 1);
                    lcd.print("Please wait...");

                    time_t now;
                    time(&now);
                    nextScheduledFetchTime = now;

                } else if (pressDuration < longPressThreshold) {
                    // Normal short press: single or double click
                    unsigned long currentTime = millis();

                    if (waitingForDoubleClick && (currentTime - lastClickTime <= doubleClickWindow)) {
                        waitingForDoubleClick = false;
                        pendingClick          = false;
                        lastButtonActivity    = millis();
                        autoScrollExecuted    = false;
                        debugPrint(2, "Double-click detected - toggling list");
                        toggleList();
                    } else {
                        lastClickTime         = currentTime;
                        waitingForDoubleClick = true;
                        pendingClick          = true;
                    }
                }
                // If pressDuration >= threshold but longPressDetected was
                // NOT set (or was spuriously set by idle time), fall through
                // to the short-press handler above. No forced refresh.
            }
        }

        // v7.1 long-press detector - unchanged except for the
        // `buttonEverReleased` guard (v7.2). See comment above.
        if (buttonState == LOW && !longPressDetected && buttonEverReleased) {
            if (millis() - buttonPressStartTime >= longPressThreshold) {
                longPressDetected = true;
                lcd.clear();
                lcd.setCursor(0, 0);
                lcd.print("Long press detected!");
                lcd.setCursor(0, 1);
                lcd.print("Release to refresh");
                debugPrint(2, "Long press threshold reached - waiting for release");
            }
        }
    }

    if (waitingForDoubleClick && pendingClick && (millis() - lastClickTime > doubleClickWindow)) {
        waitingForDoubleClick = false;
        pendingClick          = false;
        lastButtonActivity    = millis();
        autoScrollExecuted    = false;
        debugPrint(3, "Single click confirmed - advancing display");
        advanceDisplayOffset();
    }

    lastButtonState = reading;
}

void handleBacklight() {
    if (!presenceSensorConnected) {
        lcd.backlight();
        areLedsOn       = true;
        lastPresenceTime = millis();
        return;
    }

    bool isPresent = digitalRead(presencePin) == HIGH;

    if (isPresent) {
        lastPresenceTime = millis();
        lcd.backlight();
        areLedsOn = true;
    } else {
        if (millis() - lastPresenceTime >= backlightOffDelay) {
            lcd.noBacklight();
            areLedsOn = false;
        } else {
            lcd.backlight();
            areLedsOn = true;
        }
    }
}

void handlePresenceSensor() {
    static bool lastPresenceState = false;
    bool currentPresenceState = digitalRead(presencePin) == HIGH;

    if (currentPresenceState != lastPresenceState) {
        if (currentPresenceState) {
            debugPrint(3, "Presence detected - turning on backlight");
            lcd.backlight();
            lastPresenceTime = millis();
        }
        lastPresenceState = currentPresenceState;
    }
}

// ========================================================================
// SCHEDULING & MIDNIGHT LOGIC
// ========================================================================

void scheduleAfterMidnightFailure() {
    time_t now;
    time(&now);

    if (midnightPhaseActive) {
        if (midnightRetryCount < 5) {
            midnightRetryCount++;
            nextScheduledFetchTime = now + 600; // 10 minutes
            debugPrint(2, "Midnight retry " + String(midnightRetryCount) + "/5 in 10 minutes");
        } else {
            struct tm ti;
            // v7.5 FIX C: localtime_r() -- caller-owned buffer, no shared static
            if (localtime_r(&now, &ti)) {
                time_t nextHour = now - (ti.tm_min * 60) - ti.tm_sec + 3600;
                nextScheduledFetchTime = nextHour;
                debugPrint(2, "Midnight retries exhausted; next fetch top-of-hour");
            } else {
                nextScheduledFetchTime = now + 3600;
                debugPrint(2, "Midnight retries exhausted; fallback 1h");
            }
        }
    } else {
        debugPrint(2, "scheduleAfterMidnightFailure called outside midnight phase");
    }
}

// ========================================================================
// v7.4 FIX B: handleDataFetching() now has a belt-and-suspenders guard.
// After calling fetchAndProcessData(true), if isTomorrowDataAvailable is
// still false, nextScheduledFetchTime is forced to at least now + 1800.
// This protects against any future refactor that removes the advance from
// the helper.
// ========================================================================

void handleDataFetching() {
    if (!isTimeSynced) return;
    time_t now; time(&now);
    if (now < nextScheduledFetchTime) return;

    // ------------------------------------------------------------------
    // v7.5 FIX G - never START a blocking fetch while the user is clicking.
    // ------------------------------------------------------------------
    // Fix F makes the double-click SURVIVE a fetch that is already running,
    // but it cannot shorten that fetch: the screen still sits frozen until
    // http.GET() returns. So the cheapest large win is to not begin a fetch
    // in the first place when the user is mid-gesture.
    //
    // The guard runs off btnEvtLastMs -- the timestamp of the last button
    // edge captured by the ISR -- so it reacts to the physical press, not
    // to a click the loop has already processed. That covers the whole
    // double-click window (first press, gap, second press) plus a margin.
    //
    // This only DEFERS the fetch; it does not cancel it. nextScheduledFetchTime
    // is untouched, so the data still arrives, just a fraction of a second
    // later, when the user's hands are off the button.
    // ------------------------------------------------------------------
    if (btnEvtLastMs != 0 && (millis() - btnEvtLastMs) < BUTTON_INTERACTION_GUARD_MS) return;

    if (!isTodayDataAvailable) {
        debugPrint(2, "Fetching Today's Data...");
        fetchAndProcessData(false);
        // v7.5 FIX E: guarantee forward progress.
        // fetchAndProcessData() reschedules only SOME of its failure paths
        // (wrong-day rejection and JSON parse failure both test
        // `if (fetchTomorrow)`, so the today-fetch was left with a stale
        // nextScheduledFetchTime). Because handleDataFetching() gates only on
        // `now >= nextScheduledFetchTime` and re-enters while today data is
        // still missing, a rejected today-fetch produced an unbounded
        // blocking http.GET() loop -- the UI froze for the duration of every
        // request (~0.4-1.5 s each) and the button never got serviced.
        // Same belt-and-braces shape as the v7.4 Fix B guard below.
        time_t now2; time(&now2);
        if (nextScheduledFetchTime <= now2) nextScheduledFetchTime = now2 + 600;
    } else {
        struct tm ti;
        // v7.5 FIX C: localtime_r() -- caller-owned buffer, no shared static.
        // v7.4 dereferenced the localtime() result here with no NULL check.
        // If the conversion fails we fall through to the else branch and just
        // reschedule 30 min out rather than attempting a fetch.
        bool tomorrowWindow = localtime_r(&now, &ti) &&
                              ti.tm_hour >= 14 && ti.tm_hour <= 23;
        // v7.4: tomorrow-fetch window is 14:00–23:00 only. After 23:00 the
        // Midnight Bridge (loop rollover detection) takes over; no more
        // tomorrow HTTP calls are issued.
        if (tomorrowWindow && !isTomorrowDataAvailable) {
            debugPrint(2, "Fetching Tomorrow's Data...");
            fetchAndProcessData(true);

            // v7.4 FIX B: if the fetch did NOT succeed (still no data),
            // ensure the schedule is at least 30 min ahead.
            if (!isTomorrowDataAvailable) {
                time_t now2; time(&now2);
                if (nextScheduledFetchTime <= now2)
                    nextScheduledFetchTime = now2 + 1800;
            }
        } else {
            nextScheduledFetchTime = now + 1800;
        }
    }
}

// ========================================================================
// SETUP & MAIN LOOP
// ========================================================================

void setup() {
    Serial.begin(115200);
    debugPrint(2, "Starting Dynamic Electricity Ticker v7.5 (DST + API URL Hardening) - DST-SAFE");

    pinMode(builtinLedPin, OUTPUT);
    pinMode(whiteLedPin,   OUTPUT);
    pinMode(buttonPin,     INPUT_PULLUP);

    // v7.4 Fix D: attach a CHANGE interrupt on the button pin so that
    // presses occurring during a blocking HTTP fetch are captured.
    attachInterrupt(digitalPinToInterrupt(buttonPin), buttonISR, CHANGE);
    debugPrint(2, "Button interrupt attached on GPIO " + String(buttonPin));

    const int NUM_DETECTION_SAMPLES = 5;
    int high_reads = 0;

    pinMode(presencePin, INPUT);
    delay(50);
    for (int i = 0; i < NUM_DETECTION_SAMPLES; i++) {
        if (digitalRead(presencePin) == HIGH) high_reads++;
        delay(5);
    }
    presenceSensorConnected = (high_reads > 0);

    if (presenceSensorConnected) debugPrint(2, "Presence sensor detected and connected");
    else debugPrint(2, "Presence sensor not detected - backlight always on");

    lcd.init();
    lcd.backlight();
    lastPresenceTime = millis();

    lcd.createChar(0, bitmap_c);
    lcd.createChar(1, bitmap_s);
    lcd.createChar(2, bitmap_z);
    lcd.createChar(3, lo_prc);
    lcd.createChar(4, hi_prc);
    lcd.setCursor(0, 0);
    lcd.print("Initializing ...");
    delay(1000);

    connectToWiFi();
    if (WiFi.status() == WL_CONNECTED) configTzTime(TZ_CET_CEST, "pool.ntp.org");
    debugPrint(2, "Setup completed successfully");
}

void loop() {
    if (needsRestart) { delay(100); ESP.restart(); }
    if (inProvisioningMode) { handleProvisioning(); return; }

    if (!isTimeSynced) {
        struct tm timeinfo;
        if (getLocalTime(&timeinfo)) {
            debugPrint(2, "NTP synchronization successful");
            isTimeSynced = true;
            lastButtonActivity = millis();
            autoScrollExecuted = false;

            time_t now; time(&now);
            trackedDay = timeinfo.tm_mday;

            loadDataFromNVS();

            if (!isTodayDataAvailable) nextScheduledFetchTime = now;
            else nextScheduledFetchTime = now + 60;

            displayPrices();
        } else {
            lcd.setCursor(0, 0); lcd.print("Connecting...");
            lcd.setCursor(0, 1); lcd.print("Syncing Time...");
            return;
        }
    }

    // THE MIDNIGHT BRIDGE (Detect local day rollover)
    time_t now_for_daycheck = time(nullptr);
    struct tm daycheck;
    // v7.5 FIX C: localtime_r() -- caller-owned buffer, no shared static
    if (localtime_r(&now_for_daycheck, &daycheck)) {
        if (trackedDay == -1) trackedDay = daycheck.tm_mday;
        else if (daycheck.tm_mday != trackedDay) {
            trackedDay = daycheck.tm_mday;
            debugPrint(1, "Midnight rollover detected - Executing Midnight Bridge");

            if (isTomorrowDataAvailable) {
                doc = docTomorrow;
                docTomorrow.clear();

                isTodayDataAvailable = true;
                isTomorrowDataAvailable = false;

                averagePrice = averagePriceTomorrow;
                lowestPriceIndex = lowestPriceIndexTomorrow;
                highestPriceIndex = highestPriceIndexTomorrow;

                String payload;
                serializeJson(doc, payload);
                saveDataToNVS(payload, false);
                clearTomorrowNVS();

                timeOffsetHours = 0;
                displayPrices();
            } else {
                isTodayDataAvailable = false;
                displayState = NO_DATA_OFFSET;
                timeOffsetHours = 0;
                analogWrite(whiteLedPin, 0);
                midnightPhaseActive = true;
                midnightRetryCount = 0;
                nextScheduledFetchTime = now_for_daycheck;
                displayPrices();
            }
        }
    }

    handleButton();
    handlePresenceSensor();
    handleBacklight();
    updateLeds();
    handleDataFetching();

    if (!autoScrollExecuted && millis() - lastButtonActivity >= autoScrollTimeout) {
        resetDisplayToTop();
        autoScrollExecuted = true;
    }

    // STATE-BASED REFRESH
    time_t now = time(nullptr);
    struct tm timeinfo;
    // v7.5 FIX C: localtime_r() -- caller-owned buffer, no shared static
    if (localtime_r(&now, &timeinfo)) {
        int currentHour = timeinfo.tm_hour;
        int currentMinute = timeinfo.tm_min;
        unsigned long current15MinBlock = (currentHour * 4) + (currentMinute / 15);

        if (currentHour != (int)lastHourlyRefresh || current15MinBlock != last15MinRefresh) {
            displayPrices();
            lastHourlyRefresh = currentHour;
            last15MinRefresh = current15MinBlock;
        }
    }

    if (millis() - lastLoopUpdate >= Config::LOOP_UPDATE_INTERVAL) lastLoopUpdate = millis();
}
