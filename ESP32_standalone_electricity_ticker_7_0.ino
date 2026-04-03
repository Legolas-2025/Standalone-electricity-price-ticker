/*
  ESP32_standalone_electricity_ticker_7_0.ino
  -----------------------------------------------------

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
// - Spring forward: Last Sunday of March at 02:00 → 03:00 (CEST, UTC+2)
// - Fall back: Last Sunday of October at 03:00 → 02:00 (CET, UTC+1)
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

// ========================================================================
// NEW VERSION 7.0 GLOBALS (DUAL BUFFERS)
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

// Find the price index that corresponds to a given local Unix timestamp.
// Searches unixSeconds array for the first entry whose hour matches the
// given timestamp's hour. This works correctly on DST days because it
// uses actual Unix timestamps rather than assuming 24 hours * 4 = 96 entries.
// Returns the index into the prices/unixSeconds arrays, or -1 if not found.
int findPriceIndexForHour(const JsonArray& unixSeconds, int targetHour) {
    if (unixSeconds.size() == 0) return -1;

    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return -1;

    // Search through unixSeconds to find entries matching the target hour
    for (size_t i = 0; i < unixSeconds.size(); i++) {
        unsigned long unixTime = unixSeconds[i].as<unsigned long>();
        if (!isValidUnixTime(unixTime)) continue;

        time_t t = (time_t)unixTime;
        struct tm* ptm = localtime(&t);
        if (ptm != NULL && ptm->tm_hour == targetHour) {
            // Found the first entry for this hour
            return (int)i;
        }
    }

    return -1;
}

// Find the current price index based on the current Unix timestamp.
// This finds the 15-minute interval that CONTAINS the current time.
int findCurrentPriceIndex(const JsonArray& unixSeconds) {
    if (unixSeconds.size() == 0) return -1;

    time_t now;
    time(&now);

    // Search from the end of the day backwards to find the latest 
    // interval that has already started (unixTime <= now).
    // This ensures that at exactly 20:00:00, we correctly get the 20:00 index.
    for (size_t i = unixSeconds.size(); i > 0; i--) {
        size_t idx = i - 1;
        unsigned long unixTime = unixSeconds[idx].as<unsigned long>();
        if (!isValidUnixTime(unixTime)) continue;

        if ((time_t)unixTime <= now) {
            return (int)idx;
        }
    }

    // Edge case: current time is before the first entry in the dataset
    return 0;
}

// Get the hour (0-23) from a price array index using unixSeconds.
// Returns -1 if index is invalid or unixSeconds is not available.
int getHourFromPriceIndex(const JsonArray& unixSeconds, int priceIndex) {
    if (priceIndex < 0 || priceIndex >= (int)unixSeconds.size()) return -1;

    unsigned long unixTime = unixSeconds[priceIndex].as<unsigned long>();
    if (!isValidUnixTime(unixTime)) return -1;

    time_t t = (time_t)unixTime;
    struct tm* ptm = localtime(&t);
    if (ptm == NULL) return -1;

    return ptm->tm_hour;
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

// NEW: Updated connectToWiFi with v7.0 String
void connectToWiFi() {
    String stored_ssid = "";
    String stored_pass = "";

    preferences.begin("my-ticker", false);
    stored_ssid = preferences.getString("ssid", "");
    stored_pass = preferences.getString("pass", "");
    preferences.end();

    if (stored_ssid.length() > 0) {
        lcd.setCursor(0, 0);
        lcd.print("Elec. Rate SI v7.0");
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

    // Use timestamp-based lookup for DST safety
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
        // Very high price: triple blink-ish pattern (same as before), but PWM-only.
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

// Helper: hourly average from 15-minute data
// Uses timestamp-based lookup for DST safety
float getHourlyAverage(int hourIndex, const JsonArray& prices, const JsonArray& unixSeconds) {
    if (hourIndex < 0 || hourIndex >= 24) return 0.0;
    if (unixSeconds.size() == 0) return 0.0;

    // Find the first index for this hour using timestamps
    int startIndex = findPriceIndexForHour(unixSeconds, hourIndex);
    if (startIndex < 0 || startIndex + 3 >= (int)prices.size()) return 0.0;

    float sum = 0.0;
    int validCount = 0;

    // Get up to 4 consecutive 15-min prices for this hour
    for (int i = 0; i < 4; i++) {
        int idx = startIndex + i;
        if (idx >= (int)prices.size()) break;

        // Verify this entry is still the same hour (important for DST)
        int entryHour = getHourFromPriceIndex(unixSeconds, idx);
        if (entryHour != hourIndex) break; // Stop if we moved to next hour

        sum += prices[idx].as<float>();
        validCount++;
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

// ========================================================================
// NEW VERSION 7.0: NVS PERSISTENCE & DUAL FETCHING
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

    // Load Today
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
    
    // Load Tomorrow
    if (!storedJsonTomorrow.isEmpty()) {
        DeserializationError err = deserializeJson(docTomorrow, storedJsonTomorrow);
        if (!err) {
            processJsonData(true);
        }
    }
    
    return isTodayDataAvailable;
}

void fetchAndProcessData(bool fetchTomorrow) {
    if (WiFi.status() != WL_CONNECTED || !isTimeSynced) {
        apiFailCount++;
        return;
    }

    String url = api_url;
    if (fetchTomorrow) {
        time_t now;
        time(&now);
        now += 24 * 3600; // Add 24 hours
        struct tm* tmr = localtime(&now);
        char dateStr[20];
        snprintf(dateStr, sizeof(dateStr), "%04d-%02d-%02d", tmr->tm_year + 1900, tmr->tm_mon + 1, tmr->tm_mday);
        url += "&start=";
        url += dateStr;
    }

    HTTPClient http;
    http.begin(url);
    http.setTimeout(Config::HTTP_TIMEOUT);
    http.setConnectTimeout(Config::HTTP_CONNECT_TIMEOUT);

    int httpResponseCode = http.GET();
    if (httpResponseCode > 0) {
        String payload = http.getString();
        
        StaticJsonDocument<Config::JSON_BUFFER_SIZE>& targetDoc = fetchTomorrow ? docTomorrow : doc;
        targetDoc.clear();
        DeserializationError error = deserializeJson(targetDoc, payload);
        
        if (error) {
            apiFailCount++;
            http.end();
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
        }
        
        if (!fetchTomorrow) httpGetRetryCount = 0;
        displayPrices();
    } else {
        apiFailCount++;
        if (!fetchTomorrow) httpGetRetryCount++;
        displayPrices();
        
        if (!fetchTomorrow) {
            time_t now; time(&now);
            if (!midnightPhaseActive) nextScheduledFetchTime = now + 600;
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
    size_t lastIndex = unixSeconds.size() - 1;
    unsigned long lastUnix = unixSeconds[lastIndex].as<unsigned long>();
    time_t lastDataTime = (time_t)lastUnix;
    struct tm* lastDataTm = localtime(&lastDataTime);

    time_t targetTime = time(nullptr);
    if (isTomorrow) targetTime += 24 * 3600;
    struct tm* targetDayTm = localtime(&targetTime);

    bool sameDate = (lastDataTm->tm_mday == targetDayTm->tm_mday &&
                     lastDataTm->tm_mon == targetDayTm->tm_mon &&
                     lastDataTm->tm_year == targetDayTm->tm_year);

    if (!sameDate) {
        if (isTomorrow) isTomorrowDataAvailable = false;
        else isTodayDataAvailable = false;
        return false;
    }

    if (isTomorrow) isTomorrowDataAvailable = true;
    else isTodayDataAvailable = true;

    // Calculate Averages and Min/Max
    float sum = 0.0;
    int validHourCount = 0;
    float minPrice = 999999.0;
    float maxPrice = -999999.0;
    int lowestIdx = 0, highestIdx = 0;

    for (int hour = 0; hour < 24; hour++) {
        int startIndex = findPriceIndexForHour(unixSeconds, hour);
        if (startIndex < 0) continue;

        float hourlyAvg = getHourlyAverage(hour, prices, unixSeconds);
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
// NEW VERSION 7.0: DUAL BUFFER DISPLAY HELPERS
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
            if (localHourIndex >= 0 && localHourIndex <= 5 && currentHour >= 18) {
                lcd.print("                    "); return;
            }
            if (localHourIndex < currentHour && currentHour < 22) {
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
    if (APPLY_FEES_AND_VAT) finalPrice = finalPrice * (1 + POWER_COMPANY_FEE_PERCENTAGE / 100.0) * (1 + VAT_PERCENTAGE / 100.0);
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
    if (currentHour >= 21 && timeOffsetHours > 0) {
        displayStartHourOffset = 21 + timeOffsetHours;
    }

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

    struct tm* timeinfo_ptr = localtime(&lastSuccessfulFetchTime);
    if (timeinfo_ptr != NULL && lastSuccessfulFetchTime != 0) {
        snprintf(lines[3], sizeof(lines[3]), "%2d.%2d.%04d  %2d:%02d", timeinfo_ptr->tm_mday, timeinfo_ptr->tm_mon + 1, timeinfo_ptr->tm_year + 1900, timeinfo_ptr->tm_hour, timeinfo_ptr->tm_min);
    } else snprintf(lines[3], sizeof(lines[3]), "No data timestamp   ");

    snprintf(lines[4], sizeof(lines[4]), "                    ");
    snprintf(lines[5], sizeof(lines[5]), "Daily average:");

    if (isTodayDataAvailable) {
        float finalPrice = averagePrice;
        if (APPLY_FEES_AND_VAT) finalPrice = finalPrice * (1 + POWER_COMPANY_FEE_PERCENTAGE / 100.0) * (1 + VAT_PERCENTAGE / 100.0);
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
    if (totalApiCalls > 0) snprintf(lines[10], sizeof(lines[10]), "API:%3d%% (%d/%d)", (apiSuccessCount * 100) / totalApiCalls, apiSuccessCount, totalApiCalls);
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
        struct tm* st = localtime(&nvsLastStoreTime);
        if (st != NULL) {
            char storeBuf[16]; snprintf(storeBuf, sizeof(storeBuf), "%2d.%2d.%04d", st->tm_mday, st->tm_mon + 1, st->tm_year + 1900);
            snprintf(lines[14], sizeof(lines[14]), "Last save:%10s", storeBuf);
        } else snprintf(lines[14], sizeof(lines[14]), "Last save: invalid ");
    } else snprintf(lines[14], sizeof(lines[14]), "Last save: none    ");

    if (isTodayDataAvailable && isTomorrowDataAvailable) snprintf(lines[15], sizeof(lines[15]), "NVS: Today+Tomorrow");
    else if (isTodayDataAvailable) snprintf(lines[15], sizeof(lines[15]), "NVS: Today only    ");
    else snprintf(lines[15], sizeof(lines[15]), "NVS: Empty/Old     ");

    snprintf(lines[16], sizeof(lines[16]), "energy-charts.info");
    snprintf(lines[17], sizeof(lines[17]), "dynamic electricity");
    snprintf(lines[18], sizeof(lines[18]), "price ticker v7.0  ");
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
        
        if (allowedAhead < 2 && currentHour >= 21 && !isTomorrowDataAvailable) allowedAhead = 2;

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

// ========================================================================
// NEW VERSION 7.0: SMART SCHEDULING & MIDNIGHT BRIDGE
// ========================================================================

void handleDataFetching() {
    if (!isTimeSynced) return;
    time_t now; time(&now);
    if (now < nextScheduledFetchTime) return;

    if (!isTodayDataAvailable) {
        // Priority 1: We have no data for today, fetch immediately
        debugPrint(2, "Fetching Today's Data...");
        fetchAndProcessData(false); 
    } else {
        struct tm* ti = localtime(&now);
        // Priority 2: If it's after 14:00 (2 PM) and we don't have tomorrow's data yet
        if (ti->tm_hour >= 14 && !isTomorrowDataAvailable) {
            debugPrint(2, "Fetching Tomorrow's Data...");
            fetchAndProcessData(true);
        } else {
            // Already have everything we need, defer checks by 30 mins
            nextScheduledFetchTime = now + 1800; 
        }
    }
}

// ========================================================================
// SETUP & MAIN LOOP
// ========================================================================

void setup() {
    Serial.begin(115200);
    debugPrint(2, "Starting Dynamic Electricity Ticker v7.0 (Rolling 48H) - DST-SAFE");

    pinMode(builtinLedPin, OUTPUT);
    pinMode(whiteLedPin,   OUTPUT);
    pinMode(buttonPin,     INPUT_PULLUP);

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
            
            // Load dual buffer from NVS
            loadDataFromNVS();
            
            // If missing today's data, fetch immediately. Otherwise defer check.
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
    struct tm* daycheck = localtime(&now_for_daycheck);
    if (daycheck != nullptr) {
        if (trackedDay == -1) trackedDay = daycheck->tm_mday;
        else if (daycheck->tm_mday != trackedDay) {
            trackedDay = daycheck->tm_mday;
            debugPrint(1, "Midnight rollover detected - Executing Midnight Bridge");
            
            if (isTomorrowDataAvailable) {
                // Swap tomorrow to today instantly
                doc = docTomorrow;
                docTomorrow.clear();
                
                isTodayDataAvailable = true;
                isTomorrowDataAvailable = false;
                
                averagePrice = averagePriceTomorrow;
                lowestPriceIndex = lowestPriceIndexTomorrow;
                highestPriceIndex = highestPriceIndexTomorrow;
                
                // Overwrite Today's NVS with the new data and clear the Tomorrow slot
                String payload;
                serializeJson(doc, payload);
                saveDataToNVS(payload, false);
                clearTomorrowNVS();
                
                timeOffsetHours = 0;
                displayPrices();
            } else {
                // If tomorrow's fetch failed earlier, fall back to "No Data" mode
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
    struct tm* timeinfo = localtime(&now);
    if (timeinfo != nullptr) {
        int currentHour = timeinfo->tm_hour;
        int currentMinute = timeinfo->tm_min;
        unsigned long current15MinBlock = (currentHour * 4) + (currentMinute / 15);

        if (currentHour != (int)lastHourlyRefresh || current15MinBlock != last15MinRefresh) {
            displayPrices();
            lastHourlyRefresh = currentHour;
            last15MinRefresh = current15MinBlock;
        }
    }

    if (millis() - lastLoopUpdate >= Config::LOOP_UPDATE_INTERVAL) lastLoopUpdate = millis();
}
