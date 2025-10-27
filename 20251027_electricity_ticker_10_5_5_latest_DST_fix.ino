/* 
  20251027_electricity_ticker_10_5_5_latest_DST_fix.ino
  -----------------------------------------------------
  Derived from the original Electricity-price-ticker v5.5
  Purpose: Preserve functionality and fix DST/timezone handling.
  Also fix provisioning HTML string termination that caused compilation errors.
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include <Preferences.h> // For non-volatile storage
#include <DNSServer.h> // For captive portal
#include <WebServer.h> // For provisioning web server
#include <time.h>

// ========================================================================
// Dynamic Electricity Ticker for XIAO ESP32C3 - VERSION 5.5 (15-MINUTE DETAIL MODE)
// DST / TIMEZONE FIXED (CET <-> CEST automatic switching)
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

const int whiteLedPin = 5;
const int builtinLedPin = 21;
const int buttonPin = 4;
const int presencePin = 9;

bool ledsConnected = HAS_WHITE_LED;
bool areLedsOn = false;

int breatheValue = 0;
int breatheDir = 1;
unsigned long lastBreatheMillis = 0;
const int breatheInterval = 10;

bool blinkState = false;
unsigned long lastBlinkMillis = 0;
const int BLINK_INTERVAL_1000MS = 1000;
const int BLINK_INTERVAL_500MS = 500;
const int BLINK_INTERVAL_200MS = 200;

bool doubleBlinkState = false;
int doubleBlinkCount = 0;
unsigned long lastDoubleBlinkMillis = 0;
const int DOUBLE_BLINK_FAST_INTERVAL = 200;
const int DOUBLE_BLINK_LONG_ON_INTERVAL = 400;
const int DOUBLE_BLINK_PAUSE_INTERVAL = 1000;

const float PRICE_THRESHOLD_0_05 = 0.05;
const float PRICE_THRESHOLD_0_15 = 0.15;
const float PRICE_THRESHOLD_0_25 = 0.25;
const float PRICE_THRESHOLD_0_35 = 0.35;
const float PRICE_THRESHOLD_0_50 = 0.50;

LiquidCrystal_I2C lcd(0x27, 20, 4);

const char* api_url = "https://api.energy-charts.info/price?bzn=SI";

time_t nextScheduledFetchTime = 0;
int lastSuccessfulFetchDay = 0;
int httpGetRetryCount = 0;
const int HTTP_GET_RETRY_MAX = 5;
const int HTTP_GET_BACKOFF_FACTOR = 2;
time_t lastSuccessfulFetchTime;

int apiSuccessCount = 0;
int apiFailCount = 0;

const long gmtOffset_sec = 3600;
const int daylightOffset_sec = 3600;
const char* TZ_CET_CEST = "CET-1CEST,M3.5.0/02:00,M10.5.0/03:00";

const bool APPLY_FEES_AND_VAT = true;
const float POWER_COMPANY_FEE_PERCENTAGE = 12.0;
const float VAT_PERCENTAGE = 22.0;

int buttonState = 0;
int lastButtonState = 0;
unsigned long lastDebounceTime = 0;
unsigned long buttonPressStartTime = 0;
bool longPressDetected = false;
const unsigned long debounceDelay = 50;
const unsigned long longPressThreshold = 2000;

unsigned long lastClickTime = 0;
const unsigned long doubleClickWindow = 500;
bool waitingForDoubleClick = false;
bool pendingClick = false;

unsigned long lastButtonActivity = 0;
const unsigned long autoScrollTimeout = 10000;
bool autoScrollExecuted = false;

unsigned long lastHourlyRefresh = 0;
unsigned long last15MinRefresh = 0;

int secondaryListOffset = 0;
const int SECONDARY_LIST_TOTAL_LINES = 16;
const int SECONDARY_LIST_SCROLL_INCREMENT = 4;

const unsigned long backlightOffDelay = 30000;
unsigned long lastPresenceTime = 0;
bool presenceSensorConnected = false;

unsigned long lastLoopUpdate = 0;

enum DisplayState { CURRENT_PRICES, CUSTOM_MESSAGE, NO_DATA_OFFSET };
DisplayState displayState = CURRENT_PRICES;
int timeOffsetHours = 0;

enum ListType { PRIMARY_LIST, SECONDARY_LIST };
ListType currentList = PRIMARY_LIST;

StaticJsonDocument<Config::JSON_BUFFER_SIZE> doc;
bool isTodayDataAvailable = false;
float averagePrice = 0.0;
int lowestPriceIndex = -1;
int highestPriceIndex = -1;

Preferences preferences;
DNSServer dnsServer;
WebServer server(80);

const char* ap_ssid = "MyTicker_Setup";
bool inProvisioningMode = false;
bool needsRestart = false;

bool isTimeSynced = false;
bool initialBoot = true;

byte bitmap_c[8] = { B00100, B00000, B01110, B10001, B10000, B10001, B01110, B00000 };
byte bitmap_s[8] = { B00100, B00000, B01110, B10000, B01110, B00001, B11110, B00000 };
byte bitmap_z[8] = { B00100, B00000, B11111, B00010, B00100, B01000, B11111, B00000 };
byte lo_prc[] = { B00000, B00100, B00100, B00100, B10101, B01110, B00100, B00000 };
byte hi_prc[] = { B00000, B00100, B01110, B10101, B00100, B00100, B00100, B00000 };

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
        // Fixed: terminating quote and closed form tag
        String html = "<h3>Wi-Fi Setup</h3><form action='/save' method='get'>SSID: <input type='text' name='ssid'><br>Password: <input type='password' name='pass'><br><input type='submit' value='Save'></form>";
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
        lcd.print("Elec. Rate SI");
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
