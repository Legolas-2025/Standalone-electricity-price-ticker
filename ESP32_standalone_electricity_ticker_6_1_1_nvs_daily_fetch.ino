/* 
  ESP32_standalone_electricity_ticker_6_1_1_nvs_daily_fetch.ino
  -----------------------------------------------------

  VERSION 6.1.1 CHANGES (2026-03-07):
  ----------------------------------
  FIX:
  - Daily lowest/highest hourly indicator (and daily average) now correctly considers
    ALL hourly averages, including negative values and 0.0 (which can be real prices).
  - The daily average is now computed using the number of valid hours actually present
    in the dataset (validHourCount), instead of always dividing by 24.

  NOTE:
  - This version is identical to v6.1 except for:
      * processJsonData() min/max/average fix
      * UI version strings ("v6.1.1")
      * header comment version/date
*/

// ========================================================================
// Dynamic Electricity Ticker for XIAO ESP32C3 - VERSION 6.1.1
// 15-MINUTE DETAIL MODE, DST / TIMEZONE FIXED (CET <-> CEST automatic)
// ========================================================================

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include <Preferences.h> // For non-volatile storage
#include <DNSServer.h>   // For captive portal
#include <WebServer.h>   // For provisioning web server
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
const long gmtOffset_sec = 3600;
const int  daylightOffset_sec = 3600;
const char* TZ_CET_CEST = "CET-1CEST,M3.5.0/02:00,M10.5.0/03:00";

// Price computation
const bool  APPLY_FEES_AND_VAT           = true;
const float POWER_COMPANY_FEE_PERCENTAGE = 12.0;
const float VAT_PERCENTAGE               = 22.0;

// Button handling
int buttonState          = 0;
int lastButtonState      = 0;
unsigned long lastDebounceTime    = 0;
unsigned long buttonPressStartTime = 0;
bool longPressDetected   = false;
const unsigned long debounceDelay      = 50;
const unsigned long longPressThreshold = 3000;

unsigned long lastClickTime     = 0;
const unsigned long doubleClickWindow = 500;
bool waitingForDoubleClick      = false;
bool pendingClick               = false;

// Auto scroll timeout
unsigned long lastButtonActivity = 0;
const unsigned long autoScrollTimeout = 10000;
bool autoScrollExecuted = false;

// Time-based display refresh markers
unsigned long lastHourlyRefresh  = 0;
unsigned long last15MinRefresh   = 0;

// Secondary list / menu
int secondaryListOffset = 0;
// INCREASED: now 20 lines total (16 old + 4 NVS status)
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

// JSON doc and data flags
StaticJsonDocument<Config::JSON_BUFFER_SIZE> doc;
bool isTodayDataAvailable = false;
float averagePrice        = 0.0;
int lowestPriceIndex      = -1;
int highestPriceIndex     = -1;

// NVS and provisioning
Preferences preferences;
DNSServer dnsServer;
WebServer  server(80);

const char* ap_ssid = "MyTicker_Setup";
bool inProvisioningMode = false;
bool needsRestart       = false;

// Time sync flag
bool isTimeSynced = false;
bool initialBoot  = true;

// Track the current local day so we can detect midnight rollover
int trackedDay = -1;

// NVS-related state (for secondary menu / debug)
bool  nvsDataLoadedForToday = false;
bool  nvsDataPresent        = false;
int   nvsStoredDay          = -1;
int   nvsStoredMonth        = -1;
int   nvsStoredYear         = -1;
time_t nvsLastStoreTime     = 0;

// Midnight retry tracking
int midnightRetryCount = 0;   // Number of 10-minute retries attempted since midnight (max 5)
bool midnightPhaseActive = false; // true from midnight rollover until successful fetch for that day

// NEW: track whether last processJsonData() accepted dataset as "today"
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
        lcd.print("Elec. Rate SI v6.1.1");
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
    if (!ledsConnected || !areLedsOn || !isTodayDataAvailable || !isTimeSynced) {
        digitalWrite(whiteLedPin, LOW);
        breatheValue = 0;
        breatheDir   = 1;
        blinkState   = LOW;
        doubleBlinkCount = 0;
        return;
    }

    JsonArray prices = doc["price"];

    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return;

    int currentHour        = timeinfo.tm_hour;
    int quarterHourIndex   = getCurrentQuarterHourIndex();
    int currentIntervalIndex = currentHour * 4 + quarterHourIndex;

    if (currentIntervalIndex >= (int)prices.size()) {
        digitalWrite(whiteLedPin, LOW);
        return;
    }

    float currentRate = prices[currentIntervalIndex].as<float>();

    if (currentRate <= 0) {
        digitalWrite(whiteLedPin, LOW);
        return;
    }

    float finalPrice = currentRate;
    if (APPLY_FEES_AND_VAT) {
        finalPrice = finalPrice * (1 + POWER_COMPANY_FEE_PERCENTAGE / 100.0) * (1 + VAT_PERCENTAGE / 100.0);
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
        digitalWrite(whiteLedPin, HIGH);
        doubleBlinkCount = 0;
    } else if (finalPrice <= PRICE_THRESHOLD_0_25) {
        if (millis() - lastBlinkMillis > BLINK_INTERVAL_1000MS) {
            blinkState = !blinkState;
            digitalWrite(whiteLedPin, blinkState);
            lastBlinkMillis = millis();
        }
        doubleBlinkCount = 0;
    } else if (finalPrice <= PRICE_THRESHOLD_0_35) {
        if (millis() - lastBlinkMillis > BLINK_INTERVAL_500MS) {
            blinkState = !blinkState;
            digitalWrite(whiteLedPin, blinkState);
            lastBlinkMillis = millis();
        }
        doubleBlinkCount = 0;
    } else if (finalPrice <= PRICE_THRESHOLD_0_50) {
        int targetBlinks = 2;
        if (doubleBlinkCount < targetBlinks * 2) {
            if (millis() - lastDoubleBlinkMillis > DOUBLE_BLINK_FAST_INTERVAL) {
                doubleBlinkState = !doubleBlinkState;
                digitalWrite(whiteLedPin, doubleBlinkState);
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
                digitalWrite(whiteLedPin, HIGH);
                lastDoubleBlinkMillis = millis();
                doubleBlinkCount++;
            }
        } else if (doubleBlinkCount == 1) {
            if (millis() - lastDoubleBlinkMillis > DOUBLE_BLINK_FAST_INTERVAL) {
                digitalWrite(whiteLedPin, LOW);
                lastDoubleBlinkMillis = millis();
                doubleBlinkCount++;
            }
        } else if (doubleBlinkCount == 2) {
            if (millis() - lastDoubleBlinkMillis > DOUBLE_BLINK_FAST_INTERVAL) {
                digitalWrite(whiteLedPin, HIGH);
                lastDoubleBlinkMillis = millis();
                doubleBlinkCount++;
            }
        } else if (doubleBlinkCount == 3) {
            if (millis() - lastDoubleBlinkMillis > DOUBLE_BLINK_FAST_INTERVAL) {
                digitalWrite(whiteLedPin, LOW);
                lastDoubleBlinkMillis = millis();
                doubleBlinkCount++;
            }
        } else if (doubleBlinkCount == 4) {
            if (millis() - lastDoubleBlinkMillis > DOUBLE_BLINK_FAST_INTERVAL) {
                digitalWrite(whiteLedPin, HIGH);
                lastDoubleBlinkMillis = millis();
                doubleBlinkCount++;
            }
        } else if (doubleBlinkCount == 5) {
            if (millis() - lastDoubleBlinkMillis > DOUBLE_BLINK_LONG_ON_INTERVAL) {
                digitalWrite(whiteLedPin, LOW);
                lastDoubleBlinkMillis = millis();
                doubleBlinkCount = 0;
            }
        }
    }
}

// ========================================================================
// DATA FETCHING / NVS PERSISTENCE
// ========================================================================

// Helper: hourly average from 15-minute data
float getHourlyAverage(int hourIndex, const JsonArray& prices) {
    if (hourIndex < 0 || hourIndex >= 24) return 0.0;

    int startIndex = hourIndex * 4;
    if (startIndex + 3 >= (int)prices.size()) return 0.0;

    float sum = 0.0;
    int validCount = 0;

    for (int i = 0; i < 4; i++) {
        if (startIndex + i < (int)prices.size()) {
            sum += prices[startIndex + i].as<float>();
            validCount++;
        }
    }
    return validCount > 0 ? sum / validCount : 0.0;
}

// Helper: current 15‑minute index within hour
int getCurrentQuarterHourIndex() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return 0;

    int minute = timeinfo.tm_min;
    return minute / 15;
}

// Helper: compact 15‑minute price formatting
void format15MinPrice(float price, char* buffer, int bufferSize) {
    if (APPLY_FEES_AND_VAT) {
        price = price * (1 + POWER_COMPANY_FEE_PERCENTAGE / 100.0) * (1 + VAT_PERCENTAGE / 100.0);
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

// ---- NVS helpers for data persistence ----

void saveDataToNVS(const String& rawJson) {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        debugPrint(1, "saveDataToNVS: cannot get local time, aborting save");
        return;
    }

    int day   = timeinfo.tm_mday;
    int month = timeinfo.tm_mon;         // 0-11
    int year  = timeinfo.tm_year + 1900; // full year

    time_t now;
    time(&now);

    preferences.begin("my-ticker", false);
    preferences.putInt("data_day",  day);
    preferences.putInt("data_mon",  month);
    preferences.putInt("data_year", year);
    preferences.putString("data_prc", rawJson);
    preferences.putULong("data_last_store", (unsigned long)now);
    preferences.end();

    nvsDataPresent        = true;
    nvsStoredDay          = day;
    nvsStoredMonth        = month;
    nvsStoredYear         = year;
    nvsLastStoreTime      = now;
    nvsDataLoadedForToday = true;

    debugPrint(2, "Data saved to NVS for day " + String(day) + "." + String(month + 1) + "." + String(year));
}

bool loadDataFromNVSForToday() {
    preferences.begin("my-ticker", false);

    int storedDay   = preferences.getInt("data_day",  -1);
    int storedMonth = preferences.getInt("data_mon",  -1);
    int storedYear  = preferences.getInt("data_year", -1);
    String storedJson = preferences.getString("data_prc", "");
    unsigned long storedTime = preferences.getULong("data_last_store", 0);

    preferences.end();

    if (storedDay == -1 || storedMonth == -1 || storedYear == -1 || storedJson.length() == 0) {
        debugPrint(1, "NVS: no valid stored data");
        nvsDataPresent        = false;
        nvsDataLoadedForToday = false;
        nvsStoredDay          = -1;
        nvsStoredMonth        = -1;
        nvsStoredYear         = -1;
        nvsLastStoreTime      = 0;
        return false;
    }

    nvsDataPresent   = true;
    nvsStoredDay     = storedDay;
    nvsStoredMonth   = storedMonth;
    nvsStoredYear    = storedYear;
    nvsLastStoreTime = storedTime;

    struct tm nowInfo;
    if (!getLocalTime(&nowInfo)) {
        debugPrint(1, "NVS: cannot get local time to validate stored data");
        nvsDataLoadedForToday = false;
        return false;
    }

    if (storedDay == nowInfo.tm_mday &&
        storedMonth == nowInfo.tm_mon &&
        storedYear == (nowInfo.tm_year + 1900)) {

        DeserializationError err = deserializeJson(doc, storedJson);
        if (err) {
            debugPrint(1, "NVS: failed to parse stored JSON: " + String(err.c_str()));
            nvsDataLoadedForToday = false;
            return false;
        }

        // Process data as if freshly fetched
        processJsonData();
        isTodayDataAvailable = true;
        nvsDataLoadedForToday = true;

        debugPrint(2, "NVS: loaded data for TODAY from NVS successfully");
        return true;
    } else {
        debugPrint(1, "NVS: stored data is NOT for today - ignoring");
        nvsDataLoadedForToday = false;
        return false;
    }
}

// ---- Core fetch & process ----

void fetchAndProcessData() {
    // Require Wi‑Fi and valid time
    if (WiFi.status() != WL_CONNECTED || !isTimeSynced) {
        debugPrint(1, "Not connected to WiFi or time not synced.");
        apiFailCount++;
        return;
    }

    HTTPClient http;
    http.begin(api_url);
    http.setTimeout(Config::HTTP_TIMEOUT);
    http.setConnectTimeout(Config::HTTP_CONNECT_TIMEOUT);

    int httpResponseCode = http.GET();

    if (httpResponseCode > 0) {
        debugPrint(2, "HTTP Response: " + String(httpResponseCode));

        // Read full body into String so we can also store it into NVS
        String payload = http.getString();

        doc.clear();
        DeserializationError error = deserializeJson(doc, payload);
        if (error) {
            debugPrint(1, "deserializeJson() failed: " + String(error.c_str()));
            apiFailCount++;
            displayPrices();
            http.end();
            return;
        }

        apiSuccessCount++;
        time(&lastSuccessfulFetchTime);

        // Reset acceptance flag before processing
        lastProcessJsonAcceptedToday = false;
        bool acceptedToday = processJsonData();

        if (acceptedToday && isTodayDataAvailable) {
            saveDataToNVS(payload);
        }

        httpGetRetryCount = 0;
        debugPrint(2, "Data fetched, acceptedToday=" + String(acceptedToday ? "true" : "false"));

        // Force display update (may still be NO_DATA_OFFSET if not accepted)
        displayPrices();

        // If we are in midnight retry phase and we now have today's data,
        // end the midnight phase and reset counters.
        if (midnightPhaseActive && isTodayDataAvailable && acceptedToday) {
            debugPrint(2, "Midnight phase completed successfully - data for today acquired");
            midnightPhaseActive = false;
            midnightRetryCount  = 0;
        }

        // Note: do NOT set nextScheduledFetchTime here; handleDataFetching()
        // will decide based on acceptedToday + success/failure state.

    } else {
        debugPrint(1, "HTTP request failed with code: " + String(httpResponseCode));
        apiFailCount++;
        httpGetRetryCount++;

        displayPrices();

        // For non-midnight usage (e.g. manual long-press refresh),
        // schedule a modest backoff (10 minutes).
        time_t now;
        time(&now);

        if (midnightPhaseActive) {
            debugPrint(2, "HTTP failed during midnight phase; scheduler will adjust next retries");
            // scheduleAfterMidnightFailure() will be called in handleDataFetching()
        } else {
            nextScheduledFetchTime = now + 600; // 10 minutes
            debugPrint(2, "HTTP failed (non-midnight). Next fetch scheduled in 10 minutes.");
        }
    }

    http.end();
}

// ========================================================================
// IMPROVED "TODAY" DETECTION AND DATA PROCESSING
// ========================================================================

bool processJsonData() {
    lastProcessJsonAcceptedToday = false;

    JsonArray prices      = doc["price"];
    JsonArray unixSeconds = doc["unix_seconds"];

    if (prices.size() == 0 || unixSeconds.size() == 0) {
        debugPrint(1, "No price data in API response");
        isTodayDataAvailable = false;
        return false;
    }

    struct tm currentTimeinfo;
    if (!getLocalTime(&currentTimeinfo)) {
        debugPrint(1, "Could not get current time for data validation.");
        isTodayDataAvailable = false;
        return false;
    }

    // Determine "market day" from LAST timestamp of the dataset.
    // The JSON you provided covers one full day at 15-min intervals.
    size_t lastIndex = unixSeconds.size() - 1;
    unsigned long lastUnix = unixSeconds[lastIndex].as<unsigned long>();
    time_t lastDataTime = (time_t)lastUnix;
    struct tm* lastDataTm = localtime(&lastDataTime);

    if (!lastDataTm) {
        debugPrint(1, "localtime() failed for last data timestamp.");
        isTodayDataAvailable = false;
        return false;
    }

    int dataDay   = lastDataTm->tm_mday;
    int dataMonth = lastDataTm->tm_mon;
    int dataYear  = lastDataTm->tm_year;

    int currDay   = currentTimeinfo.tm_mday;
    int currMonth = currentTimeinfo.tm_mon;
    int currYear  = currentTimeinfo.tm_year;

    bool sameDate = (dataDay == currDay &&
                     dataMonth == currMonth &&
                     dataYear == currYear);

    if (!sameDate) {
        debugPrint(1, "Fetched data day does not match current local day; "
                      "dataDay=" + String(dataDay) +
                      " currDay=" + String(currDay));
        isTodayDataAvailable = false;
        return false;
    }

    // Data is considered valid for today's market day
    isTodayDataAvailable = true;
    lastProcessJsonAcceptedToday = true;

    // --------------------------------------------------------------------
    // v6.1.1 FIX:
    // - Include negative and zero hourly averages in min/max and average.
    // - Compute daily average using count of valid hours (validHourCount).
    // --------------------------------------------------------------------
    float sum = 0.0;
    int   validHourCount = 0;

    float minPrice = 999999.0;
    float maxPrice = -999999.0;

    lowestPriceIndex  = 0;
    highestPriceIndex = 0;

    for (int hour = 0; hour < 24; hour++) {
        int startIndex = hour * 4;

        // Valid hour = we have all 4×15-min values for this hour.
        // Do NOT use price value (0.0) as a validity signal, because 0 can be real.
        if (startIndex + 3 >= (int)prices.size()) {
            continue;
        }

        float hourlyAvg = getHourlyAverage(hour, prices);

        sum += hourlyAvg;
        validHourCount++;

        if (hourlyAvg < minPrice) {
            minPrice = hourlyAvg;
            lowestPriceIndex = startIndex;
        }
        if (hourlyAvg > maxPrice) {
            maxPrice = hourlyAvg;
            highestPriceIndex = startIndex;
        }
    }

    if (validHourCount > 0) {
        averagePrice = sum / (float)validHourCount;
    } else {
        // Should not happen for a full-day dataset, but keep safe defaults
        averagePrice = 0.0;
        lowestPriceIndex = -1;
        highestPriceIndex = -1;
    }

    debugPrint(3, "Processed " + String(prices.size()) + " price entries (15-min intervals)");
    debugPrint(3, "Daily average: " + String(averagePrice) + " EUR/MWh");

    return true;
}

// ========================================================================
// DISPLAY HELPERS
// ========================================================================

void display15MinuteDetails(int row, int hourIndex, const JsonArray& prices) {
    lcd.setCursor(0, row);

    if (hourIndex < 0 || hourIndex >= 24) {
        lcd.print("                    ");
        return;
    }

    struct tm timeinfo;
    int currentHour   = -1;
    int currentMinute = -1;
    bool hasValidTime = false;

    if (getLocalTime(&timeinfo)) {
        currentHour   = timeinfo.tm_hour;
        currentMinute = timeinfo.tm_min;
        hasValidTime  = true;

        // Prevent wraparound showing early hours at night
        if (hourIndex >= 0 && hourIndex <= 5 && currentHour >= 18) {
            lcd.print("                    ");
            return;
        }
        if (hourIndex < currentHour && currentHour < 22) {
            lcd.print("                    ");
            return;
        }
    }

    int startIndex = hourIndex * 4;
    char priceBuffer[4];
    int cursorPos = 0;

    int currentSegment = -1;
    if (hasValidTime && hourIndex == currentHour) {
        currentSegment = currentMinute / 15;
    }

    for (int i = 0; i < 4; i++) {
        bool shouldShowPlaceholder = false;

        if (hasValidTime && hourIndex == currentHour && i < currentSegment) {
            shouldShowPlaceholder = true;
        }

        if (shouldShowPlaceholder) {
            lcd.print(" > ");
            cursorPos += 3;
        } else if (startIndex + i < (int)prices.size()) {
            float price = prices[startIndex + i].as<float>();
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

    for (int i = cursorPos; i < 20; i++) {
        lcd.print(" ");
    }
}

void displayPriceRow(int row, int hourIndex, const JsonArray& prices, const JsonArray& unixSeconds) {
    lcd.setCursor(0, row);

    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
        int currentHour = timeinfo.tm_hour;

        if (hourIndex >= 0 && hourIndex <= 5 && currentHour >= 18) {
            lcd.print("                    ");
            return;
        }
        if (hourIndex < currentHour && currentHour < 22) {
            lcd.print("                    ");
            return;
        }
    }

    if (hourIndex >= 0 && hourIndex < 24) {
        float rates = getHourlyAverage(hourIndex, prices);
        int dataIndex = hourIndex * 4;
        if (dataIndex < (int)unixSeconds.size()) {
            unsigned long hourlyUnixTime = unixSeconds[dataIndex].as<unsigned long>();

            if (isValidUnixTime(hourlyUnixTime)) {
                time_t t = (time_t)hourlyUnixTime;
                struct tm *time_ptr = localtime(&t);

                if (time_ptr != NULL) {
                    char buffer[21];
                    int hour   = time_ptr->tm_hour;
                    int minute = 0;
                    snprintf(buffer, sizeof(buffer), "%02d:%02d", hour, minute);
                    lcd.print(buffer);

                    if (dataIndex == lowestPriceIndex) {
                        lcd.setCursor(7, row);
                        lcd.write(byte(3));
                        lcd.print("   ");
                    } else if (dataIndex == highestPriceIndex) {
                        lcd.setCursor(7, row);
                        lcd.write(byte(4));
                        lcd.print("   ");
                    } else {
                        lcd.setCursor(6, row);
                        lcd.print("    ");
                    }

                    float finalPrice = rates;
                    if (APPLY_FEES_AND_VAT) {
                        finalPrice = finalPrice * (1 + POWER_COMPANY_FEE_PERCENTAGE / 100.0) * (1 + VAT_PERCENTAGE / 100.0);
                    }
                    finalPrice /= 1000.0;

                    if (finalPrice < 0) {
                        lcd.setCursor(9, row);
                    } else {
                        lcd.setCursor(10, row);
                    }

                    commaPrint(finalPrice, 4);
                    lcd.print(" EUR");
                } else {
                    lcd.print("Time Error          ");
                }
            } else {
                lcd.print("Invalid Time        ");
            }
        } else {
            lcd.print("No Data Available   ");
        }
    } else {
        lcd.print("                    ");
    }
}

void displayPrimaryList() {
    JsonArray prices      = doc["price"];
    JsonArray unixSeconds = doc["unix_seconds"];

    if (!isTodayDataAvailable) {
        for (int i = 0; i < 4; i++) {
            lcd.setCursor(0, i);
            lcd.print("No data available   ");
        }
        return;
    }

    int baseHour = 0;
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
        baseHour = timeinfo.tm_hour;
    }

    int displayStartHour = baseHour + timeOffsetHours;

    if (baseHour >= 21 && timeOffsetHours > 0) {
        int adjustedBase = 21;
        displayStartHour = adjustedBase + timeOffsetHours;
    }

    if (displayStartHour >= 24) {
        displayStartHour = displayStartHour % 24;
    }

    // Row 0: 15min details
    display15MinuteDetails(0, displayStartHour, prices);

    // Row 1: current hour average
    displayPriceRow(1, displayStartHour, prices, unixSeconds);

    // Row 2: next hour
    int nextHour = (displayStartHour + 1) % 24;
    displayPriceRow(2, nextHour, prices, unixSeconds);

    // Row 3: hour after next
    int hourAfter = (displayStartHour + 2) % 24;
    displayPriceRow(3, hourAfter, prices, unixSeconds);
}

// Secondary list with 20 scrollable lines (16 old + 4 new NVS lines)
void displaySecondaryList() {
    char lines[SECONDARY_LIST_TOTAL_LINES][21];

    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
        snprintf(lines[0], sizeof(lines[0]), "%2d:%02d  %2d.%2d.%04d",
                 timeinfo.tm_hour, timeinfo.tm_min,
                 timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);
    } else {
        snprintf(lines[0], sizeof(lines[0]), "Time not available  ");
    }

    snprintf(lines[1], sizeof(lines[1]), "--------------------");
    snprintf(lines[2], sizeof(lines[2]), "Zadnja posodobitev:");

    struct tm* timeinfo_ptr = localtime(&lastSuccessfulFetchTime);
    if (timeinfo_ptr != NULL && lastSuccessfulFetchTime != 0) {
        snprintf(lines[3], sizeof(lines[3]), "%2d.%2d.%04d  %2d:%02d",
                 timeinfo_ptr->tm_mday, timeinfo_ptr->tm_mon + 1,
                 timeinfo_ptr->tm_year + 1900,
                 timeinfo_ptr->tm_hour, timeinfo_ptr->tm_min);
    } else {
        snprintf(lines[3], sizeof(lines[3]), "No data timestamp   ");
    }

    snprintf(lines[4], sizeof(lines[4]), "                    ");
    snprintf(lines[5], sizeof(lines[5]), "Dnevno povpre^je:");

    if (isTodayDataAvailable) {
        float finalPrice = averagePrice;
        if (APPLY_FEES_AND_VAT) {
            finalPrice = finalPrice * (1 + POWER_COMPANY_FEE_PERCENTAGE / 100.0) * (1 + VAT_PERCENTAGE / 100.0);
        }
        char priceBuffer[21];
        snprintf(priceBuffer, sizeof(priceBuffer), "%.4f EUR/kWh", finalPrice / 1000.0);
        String numStr = String(priceBuffer);
        numStr.replace('.', ',');
        snprintf(lines[6], sizeof(lines[6]), "%s", numStr.c_str());
    } else {
        snprintf(lines[6], sizeof(lines[6]), "Cene niso na voljo.");
    }

    snprintf(lines[7], sizeof(lines[7]), "                    ");

    if (WiFi.status() == WL_CONNECTED) {
        snprintf(lines[8], sizeof(lines[8]), "WiFi:%5d dBm", WiFi.RSSI());
    } else {
        snprintf(lines[8], sizeof(lines[8]), "WiFi: Disconnected");
    }

    if (WiFi.status() == WL_CONNECTED) {
        snprintf(lines[9], sizeof(lines[9]), "IP:%15s", WiFi.localIP().toString().c_str());
    } else {
        snprintf(lines[9], sizeof(lines[9]), "IP: Not connected  ");
    }

    int totalApiCalls = apiSuccessCount + apiFailCount;
    if (totalApiCalls > 0) {
        int successRate = (apiSuccessCount * 100) / totalApiCalls;
        snprintf(lines[10], sizeof(lines[10]), "API:%3d%% (%d/%d)", successRate, apiSuccessCount, totalApiCalls);
    } else {
        snprintf(lines[10], sizeof(lines[10]), "API: No calls yet  ");
    }

    unsigned long uptimeMs = millis();
    unsigned long days = uptimeMs / (24UL * 60UL * 60UL * 1000UL);
    uptimeMs %= (24UL * 60UL * 60UL * 1000UL);
    unsigned long hours = uptimeMs / (60UL * 60UL * 1000UL);
    uptimeMs %= (60UL * 60UL * 1000UL);
    unsigned long minutes = uptimeMs / (60UL * 1000UL);
    snprintf(lines[11], sizeof(lines[11]), "Up:%2lud%2luh%2lum", days, hours, minutes);
   
    // NEW NVS STATUS LINES (12–15)
    snprintf(lines[12], sizeof(lines[12]), "NVS status:");

    if (nvsDataPresent) {
        // Data day line: "DD.MM.YYYY"
        char dateBuf[16];
        snprintf(dateBuf, sizeof(dateBuf), "%2d.%2d.%04d",
                 (nvsStoredDay > 0 ? nvsStoredDay : 0),
                 (nvsStoredMonth >= 0 ? nvsStoredMonth + 1 : 0),
                 (nvsStoredYear > 0 ? nvsStoredYear : 0));
        snprintf(lines[13], sizeof(lines[13]), "Data day:%11s", dateBuf);
    } else {
        snprintf(lines[13], sizeof(lines[13]), "Data day: none     ");
    }

    if (nvsLastStoreTime != 0) {
        struct tm* st = localtime(&nvsLastStoreTime);
        if (st != NULL) {
            char storeBuf[12];
            snprintf(storeBuf, sizeof(storeBuf), "%2d.%2d.%02d",
                     st->tm_mday, st->tm_mon + 1, (st->tm_year + 1900) % 100);
            snprintf(lines[14], sizeof(lines[14]), "Last save:%8s", storeBuf);
        } else {
            snprintf(lines[14], sizeof(lines[14]), "Last save: invalid ");
        }
    } else {
        snprintf(lines[14], sizeof(lines[14]), "Last save: none    ");
    }

    if (nvsDataPresent && nvsDataLoadedForToday) {
        snprintf(lines[15], sizeof(lines[15]), "NVS: OK (today)    ");
    } else if (nvsDataPresent && !nvsDataLoadedForToday) {
        snprintf(lines[15], sizeof(lines[15]), "NVS: old data      ");
    } else {
        snprintf(lines[15], sizeof(lines[15]), "NVS: empty         ");
    }

    // CREDITS MOVED TO LINES 16–19
    snprintf(lines[16], sizeof(lines[16]), "energy-charts.info");
    snprintf(lines[17], sizeof(lines[17]), "dynamic electricity");
    snprintf(lines[18], sizeof(lines[18]), "price ticker v6.1.1");
    snprintf(lines[19], sizeof(lines[19]), "by Legolas-2025");

    // Render current window
    for (int i = 0; i < 4; i++) {
        int lineIndex = secondaryListOffset + i;
        lcd.setCursor(0, i);

        if (lineIndex < SECONDARY_LIST_TOTAL_LINES) {
            lcdPrint(lines[lineIndex]);
            for (int j = strlen(lines[lineIndex]); j < 20; j++) {
                lcd.print(" ");
            }
        } else {
            lcd.print("                    ");
        }
    }
}

void displayPrices() {
    lcd.clear();

    if (!isTimeSynced) {
        lcd.setCursor(0, 0);
        lcd.print("Syncing Time...");
        lcd.setCursor(0, 1);
        lcd.print("Please wait...");
        return;
    }

    if (currentList == SECONDARY_LIST) {
        displaySecondaryList();
        return;
    }

    // Enforce NO_DATA_OFFSET when no data
    if (!isTodayDataAvailable) {
        displayState = NO_DATA_OFFSET;
        timeOffsetHours = 0;
    } else if (displayState == NO_DATA_OFFSET) {
        displayState = CURRENT_PRICES;
        timeOffsetHours = 0;
    }

    switch (displayState) {
        case NO_DATA_OFFSET:
            lcd.setCursor(0, 0);
            lcd.print("No data for today");
            lcd.setCursor(0, 1);
            lcd.print("Press & hold to");
            lcd.setCursor(0, 2);
            lcd.print("refresh manually");
            break;
        case CURRENT_PRICES:
            displayPrimaryList();
            break;
        case CUSTOM_MESSAGE:
            displayState = CURRENT_PRICES;
            displayPrimaryList();
            break;
    }

    initialBoot = false;
}

void resetDisplayToTop() {
    timeOffsetHours      = 0;
    secondaryListOffset  = 0;
    displayState         = CURRENT_PRICES;
    currentList          = PRIMARY_LIST;

    debugPrint(2, "Auto-scroll timeout - Reset display to current hour");
    displayPrices();
}

void advanceDisplayOffset() {
    if (!isTimeSynced) return;

    if (currentList == SECONDARY_LIST) {
        secondaryListOffset += SECONDARY_LIST_SCROLL_INCREMENT;
        if (secondaryListOffset >= SECONDARY_LIST_TOTAL_LINES) {
            secondaryListOffset = 0;
        }
        displayPrices();
        return;
    }

    if (!isTodayDataAvailable) {
        displayState = NO_DATA_OFFSET;
        displayPrices();
        return;
    }

    if (displayState == NO_DATA_OFFSET) {
        return;
    } else {
        struct tm timeinfo;
        int currentHour = 0;
        if (getLocalTime(&timeinfo)) {
            currentHour = timeinfo.tm_hour;
        }

        int maxOffset = 23 - currentHour;
        if (maxOffset < 2 && currentHour >= 21) {
            maxOffset = 2;
        }

        int nextOffsetHours = timeOffsetHours + 1;

        if (nextOffsetHours > maxOffset) {
            timeOffsetHours = 0;
            displayState    = CURRENT_PRICES;
        } else {
            timeOffsetHours = nextOffsetHours;
        }
    }
    displayPrices();
}

void toggleList() {
    if (!isTimeSynced) return;
    currentList = (currentList == PRIMARY_LIST) ? SECONDARY_LIST : PRIMARY_LIST;
    debugPrint(2, "Toggled to " + String(currentList == PRIMARY_LIST ? "PRIMARY" : "SECONDARY") + " list");
    displayPrices();
}

// ========================================================================
// BUTTON, BACKLIGHT, PRESENCE
// ========================================================================

void handleButton() {
    int reading = !digitalRead(buttonPin);

    if (reading != lastButtonState) {
        lastDebounceTime = millis();

        if (reading == LOW) {
            buttonPressStartTime = millis();
            longPressDetected    = false;
        }
    }

    if ((millis() - lastDebounceTime) > debounceDelay) {
        if (reading != buttonState) {
            buttonState = reading;

            if (buttonState == HIGH) {
                unsigned long pressDuration = millis() - buttonPressStartTime;

                if (longPressDetected) {
                    debugPrint(1, "Long press detected - Forcing manual data refresh");
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
                    unsigned long currentTime = millis();

                    if (waitingForDoubleClick && (currentTime - lastClickTime <= doubleClickWindow)) {
                        waitingForDoubleClick = false;
                        pendingClick          = false;
                        lastButtonActivity    = millis();
                        autoScrollExecuted    = false;
                        debugPrint(2, "Double-click detected - toggling list");
                        toggleList();
                    } else {
                        lastClickTime        = currentTime;
                        waitingForDoubleClick = true;
                        pendingClick          = true;
                    }
                }
            }
        }

        if (buttonState == LOW && !longPressDetected) {
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

// When a fetch fails during midnight phase, set the next retry time
void scheduleAfterMidnightFailure() {
    time_t now;
    time(&now);

    if (midnightPhaseActive) {
        if (midnightRetryCount < 5) {
            // Retry every 10 minutes for first 5 attempts
            midnightRetryCount++;
            nextScheduledFetchTime = now + 600; // 10 minutes
            debugPrint(2, "Midnight retry " + String(midnightRetryCount) + "/5 in 10 minutes");
        } else {
            // After 5 attempts, retry only at top of each hour
            struct tm* ti = localtime(&now);
            if (ti != NULL) {
                time_t nextHour = now - (ti->tm_min * 60) - ti->tm_sec + 3600;
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

void handleDataFetching() {
    if (!isTimeSynced) return;

    time_t now;
    time(&now);

    if (now < nextScheduledFetchTime) {
        return;
    }

    debugPrint(2, "Scheduled data fetch triggered");
    int beforeSuccessCount = apiSuccessCount;

    // Reset acceptance flag before fetch
    lastProcessJsonAcceptedToday = false;
    fetchAndProcessData();

    bool httpJsonSucceeded = (apiSuccessCount != beforeSuccessCount);
    bool dataAcceptedToday = lastProcessJsonAcceptedToday;

    // Treat as "success" ONLY if HTTP/JSON succeeded AND data is accepted for today
    if (!httpJsonSucceeded || !dataAcceptedToday) {
        // FAILURE for scheduling / daily logic
        if (midnightPhaseActive) {
            // Ensure "no data" UI
            isTodayDataAvailable = false;
            displayState         = NO_DATA_OFFSET;
            digitalWrite(whiteLedPin, LOW);
            displayPrices();
            // Schedule next retry in midnight logic
            scheduleAfterMidnightFailure();
        } else {
            // Non-midnight failure: schedule a modest backoff if not already set
            if (nextScheduledFetchTime <= now) {
                nextScheduledFetchTime = now + 600;
            }
        }
    } else {
        // SUCCESS with data for today
        struct tm* timeinfo_ptr = localtime(&now);
        if (timeinfo_ptr != NULL) {
            lastSuccessfulFetchDay = timeinfo_ptr->tm_mday;
        }

        // Clear midnight phase if still active
        if (midnightPhaseActive) {
            midnightPhaseActive = false;
            midnightRetryCount  = 0;
        }

        // For v6.1, after any successful "today" fetch, schedule next automatic
        // fetch far in the future (24h). Midnight logic will reset this next day.
        nextScheduledFetchTime = now + 24 * 3600;
    }
}

// ========================================================================
// SETUP & MAIN LOOP
// ========================================================================

void setup() {
    Serial.begin(115200);
    debugPrint(2, "Starting Dynamic Electricity Ticker v6.1.1 (15-MINUTE DETAIL MODE) - DST fixed");

    pinMode(builtinLedPin, OUTPUT);
    pinMode(whiteLedPin,   OUTPUT);
    pinMode(buttonPin,     INPUT_PULLUP);

    const int NUM_DETECTION_SAMPLES = 5;
    int high_reads = 0;

    pinMode(presencePin, INPUT);
    delay(50);

    for (int i = 0; i < NUM_DETECTION_SAMPLES; i++) {
        if (digitalRead(presencePin) == HIGH) {
            high_reads++;
        }
        delay(5);
    }

    presenceSensorConnected = (high_reads > 0);
    if (presenceSensorConnected) {
        debugPrint(2, "Presence sensor detected and connected");
    } else {
        debugPrint(2, "Presence sensor not detected - backlight always on");
    }

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

    if (WiFi.status() == WL_CONNECTED) {
        configTzTime(TZ_CET_CEST, "pool.ntp.org");
    }

    debugPrint(2, "Setup completed successfully");
}

void loop() {
    if (needsRestart) {
        delay(100);
        ESP.restart();
    }

    if (inProvisioningMode) {
        handleProvisioning();
        return;
    }

    // Non-blocking time sync
    if (!isTimeSynced) {
        struct tm timeinfo;
        if (getLocalTime(&timeinfo)) {
            debugPrint(2, "NTP synchronization successful");
            isTimeSynced        = true;
            lastButtonActivity  = millis();
            autoScrollExecuted  = false;
            time_t now;
            time(&now);
            nextScheduledFetchTime = now; // initial fetch decision

            trackedDay = timeinfo.tm_mday;
            debugPrint(2, "Tracked day set to: " + String(trackedDay));

            // Once time is known, attempt to load today's data from NVS.
            bool loaded = loadDataFromNVSForToday();
            if (!loaded) {
                // No today's data in NVS -> schedule immediate fetch
                nextScheduledFetchTime = now;
            } else {
                // We have today's data from NVS.
                isTodayDataAvailable = true;
                // No need to fetch immediately; defer next fetch to midnight
                nextScheduledFetchTime = now + 24 * 3600;
                debugPrint(2, "Today's data loaded from NVS; skipping initial API fetch");
            }

            displayPrices();
        } else {
            lcd.setCursor(0, 0);
            lcd.print("Connecting...");
            lcd.setCursor(0, 1);
            lcd.print("Syncing Time...");
            return;
        }
    }

    // Detect local day rollover (midnight)
    time_t now_for_daycheck = time(nullptr);
    struct tm* daycheck = localtime(&now_for_daycheck);
    if (daycheck != nullptr) {
        if (trackedDay == -1) {
            trackedDay = daycheck->tm_mday;
        } else if (daycheck->tm_mday != trackedDay) {
            // Day rollover detected
            trackedDay = daycheck->tm_mday;
            debugPrint(1, "Midnight rollover detected - switching to NO_DATA_OFFSET until today's data available");

            // Immediately invalidate any "today" data
            isTodayDataAvailable = false;
            displayState         = NO_DATA_OFFSET;
            timeOffsetHours      = 0;
            digitalWrite(whiteLedPin, LOW);

            // Start midnight phase
            midnightPhaseActive = true;
            midnightRetryCount  = 0;

            // Aggressively attempt to fetch immediately
            nextScheduledFetchTime = now_for_daycheck;

            // Update display so user sees "No data for today" right away
            displayPrices();
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

    time_t now = time(nullptr);
    struct tm* timeinfo = localtime(&now);
    if (timeinfo != nullptr) {
        unsigned long currentHour   = timeinfo->tm_hour;
        unsigned long currentMinute = timeinfo->tm_min;

        if (currentMinute == 0 && lastHourlyRefresh != currentHour) {
            debugPrint(2, "Top of hour refresh - Hour: " + String(currentHour));
            displayPrices();
            lastHourlyRefresh = currentHour;
        }

        unsigned long current15MinBlock = (currentHour * 4) + (currentMinute / 15);
        if ((currentMinute % 15 == 0) && last15MinRefresh != current15MinBlock) {
            debugPrint(2, "15-minute refresh - updating placeholders");
            displayPrices();
            last15MinRefresh = current15MinBlock;
        }
    }

    if (millis() - lastLoopUpdate >= Config::LOOP_UPDATE_INTERVAL) {
        lastLoopUpdate = millis();
    }
}
