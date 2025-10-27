/* 
  20251027_electricity_ticker_10_5_5_latest_DST_fix.ino
  -----------------------------------------------------
  Derived from the original Electricity-price-ticker v5.5
  Purpose: Preserve functionality and fix DST/timezone handling.
  Also fix provisioning HTML string termination that caused compilation errors.
  FIX (2025-10-27): At midnight switch to NO_DATA_OFFSET immediately (until
  today's prices are available). This uses a trackedDay variable to detect
  day rollover and force the "no data for today" state and an immediate
  aggressive fetch.
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

// New: Track the current local day so we can detect midnight rollover
int trackedDay = -1;

byte bitmap_c[8] = { B00100, B00000, B01110, B10001, B10000, B10001, B01110, B00000 };
byte bitmap_s[8] = { B00100, B00000, B01110, B10000, B01110, B00001, B11110, B00000 };
byte bitmap_z[8] = { B00100, B00000, B11111, B00010, B00100, B01000, B11111, B00000 };
byte lo_prc[] = { B00000, B00100, B00100, B00100, B10101, B01110, B00100, B00000 };
byte hi_prc[] = { B00000, B00100, B01110, B10101, B00100, B00100, B00100, B00000 };

// forward declarations to avoid prototype issues
int getCurrentQuarterHourIndex();
void displayPrices();
void processJsonData();

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

void updateLeds() {
    if (!ledsConnected || !areLedsOn || !isTodayDataAvailable || !isTimeSynced) {
        digitalWrite(whiteLedPin, LOW);
        breatheValue = 0;
        breatheDir = 1;
        blinkState = LOW;
        doubleBlinkCount = 0;
        return;
    }

    JsonArray prices = doc["price"];

    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return;

    int currentHour = timeinfo.tm_hour;
    int quarterHourIndex = getCurrentQuarterHourIndex();

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
// DATA FETCHING AND PROCESSING
// ========================================================================

// Helper function to calculate hourly average from 15-minute data
float getHourlyAverage(int hourIndex, const JsonArray& prices) {
	if (hourIndex < 0 || hourIndex >= 24) return 0.0;
	
	int startIndex = hourIndex * 4; // 4 values per hour (15-minute intervals)
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

// Helper function to get current 15-minute interval index within the hour
int getCurrentQuarterHourIndex() {
	struct tm timeinfo;
	if (!getLocalTime(&timeinfo)) return 0;
	
	int minute = timeinfo.tm_min;
	return minute / 15; // Returns 0, 1, 2, or 3
}

// Helper function to format 15-minute price values in compact format
void format15MinPrice(float price, char* buffer, int bufferSize) {
	if (APPLY_FEES_AND_VAT) {
		price = price * (1 + POWER_COMPANY_FEE_PERCENTAGE / 100.0) * (1 + VAT_PERCENTAGE / 100.0);
	}
	price /= 1000.0; // Convert to EUR/kWh
	
	// Convert to hundredths (0.1234 becomes 12.34)
	int hundredths = (int)round(price * 100);
	
	if (hundredths > 99) {
		snprintf(buffer, bufferSize, "+99");
	} else if (hundredths < -99) {
		snprintf(buffer, bufferSize, "-99");
	} else if (hundredths >= 0) {
		snprintf(buffer, bufferSize, " %02d", hundredths);
	} else {
		snprintf(buffer, bufferSize, "%03d", hundredths); // Includes the minus sign
	}
}

// Helper function to display 15-minute details for current hour
void display15MinuteDetails(int row, int hourIndex, const JsonArray& prices) {
	lcd.setCursor(0, row);
	
	if (hourIndex < 0 || hourIndex >= 24) {
		lcd.print("                    ");
		return;
	}
	
	// Enhanced wraparound detection (same logic as displayPriceRow)
	struct tm timeinfo;
	int currentHour = -1;
	int currentMinute = -1;
	bool hasValidTime = false;
	
	if (getLocalTime(&timeinfo)) {
		currentHour = timeinfo.tm_hour;
		currentMinute = timeinfo.tm_min;
		hasValidTime = true;
		
		// Case 1: Early hours (0-5) shown when we're in evening (>= 18) - likely wraparound
		if (hourIndex >= 0 && hourIndex <= 5 && currentHour >= 18) {
			lcd.print("                    ");
			return;
		}
		
		// Case 2: General past hours (except near midnight transitions)
		if (hourIndex < currentHour && currentHour < 22) {
			lcd.print("                    ");
			return;
		}
	}
	
	int startIndex = hourIndex * 4;
	char priceBuffer[4];
	int cursorPos = 0;
	
	// Calculate current 15-minute segment (0-3) for the displayed hour
	int currentSegment = -1;
	if (hasValidTime && hourIndex == currentHour) {
		currentSegment = currentMinute / 15; // 0-14min=0, 15-29min=1, 30-44min=2, 45-59min=3
	}
	
	for (int i = 0; i < 4; i++) {
		bool shouldShowPlaceholder = false;
		
		// Show placeholder for past segments if this is the current hour
		if (hasValidTime && hourIndex == currentHour && i < currentSegment) {
			shouldShowPlaceholder = true;
		}
		
		if (shouldShowPlaceholder) {
			// Show placeholder for past 15-minute segments
			lcd.print(" > ");
			cursorPos += 3;
		} else if (startIndex + i < (int)prices.size()) {
			// Show actual price for current and future segments
			float price = prices[startIndex + i].as<float>();
			format15MinPrice(price, priceBuffer, sizeof(priceBuffer));
			lcd.print(priceBuffer);
			cursorPos += 3;
		} else {
			// No data available
			lcd.print("---");
			cursorPos += 3;
		}
		
		if (i < 3) {
			lcd.print("  "); // Add spacing between values
			cursorPos += 2;
		}
	}
	
	// Fill remaining space (20 - cursorPos)
	for (int i = cursorPos; i < 20; i++) {
		lcd.print(" ");
	}
}

void fetchAndProcessData() {
	// MODIFIED: This function now requires isTimeSynced to be true
	if (WiFi.status() != WL_CONNECTED || !isTimeSynced) {
		debugPrint(1, "Not connected to WiFi or time not synced.");
		apiFailCount++; // V2.4a: Track failures
		return;
	}

	HTTPClient http;
	http.begin(api_url);
	http.setTimeout(Config::HTTP_TIMEOUT);
	http.setConnectTimeout(Config::HTTP_CONNECT_TIMEOUT);
	
	int httpResponseCode = http.GET();
	
	if (httpResponseCode > 0) {
		debugPrint(2, "HTTP Response: " + String(httpResponseCode));
		
		// 💡 OPTIMIZED: Deserialize directly from the HTTP stream to save RAM
		doc.clear();
		DeserializationError error = deserializeJson(doc, http.getStream());
		
		if (error) {
			debugPrint(1, "deserializeJson() failed: " + String(error.c_str()));
			apiFailCount++;
			// V5.0 FIX: Force display update on JSON failure
			displayPrices(); 
			http.end();
			return;
		}
		
		// Success - update counters and timestamps
		apiSuccessCount++;
		time_t now;
		time(&now);
		lastSuccessfulFetchTime = now;
		
		// Process and validate data
		processJsonData();
		
		httpGetRetryCount = 0;
		
		debugPrint(2, "Data fetched and processed successfully");
		
		// V5.0 FIX: Force an immediate display update after the entire process.
		displayPrices(); 
		
		// V5.3 QUICK RE-SCHEDULE: If fetch succeeds but the data is marked as stale, 
		// schedule the next fetch in 10 minutes (600 seconds).
		if (!isTodayDataAvailable) {
			time_t now;
			time(&now);
			nextScheduledFetchTime = now + 600; 
			debugPrint(2, "Data is stale. Next fetch aggressively scheduled in 10 minutes.");
		}

	} else {
		debugPrint(1, "HTTP request failed with code: " + String(httpResponseCode));
		apiFailCount++;
		httpGetRetryCount++;
		
		// V5.0 FIX: Force display update on HTTP failure
		displayPrices(); 
		
		// *** MODIFICATION START - V5.4: Aggressive Fetch Retry on Failure ***
		// If HTTP failed, schedule an aggressive retry in 5 minutes (300 seconds)
		time_t now;
		time(&now);
		nextScheduledFetchTime = now + 300; 
		debugPrint(2, "HTTP failed. Next fetch aggressively scheduled in 5 minutes.");
		// *** MODIFICATION END ***
	}
	
	http.end();
}

void processJsonData() {
	JsonArray prices = doc["price"];
	JsonArray unixSeconds = doc["unix_seconds"];
	
	if (prices.size() == 0 || unixSeconds.size() == 0) {
		debugPrint(1, "No price data in API response");
		isTodayDataAvailable = false;
		return;
	}
	
	// 💡 CORRECTED LOGIC: Check if the API data is for the current day
	unsigned long firstUnixTime = unixSeconds[0].as<unsigned long>();
	time_t firstDataTime = (time_t)firstUnixTime;
	struct tm* timeinfo_ptr = localtime(&firstDataTime);

	struct tm currentTimeinfo;
	if (!getLocalTime(&currentTimeinfo)) {
		debugPrint(1, "Could not get current time for data validation.");
		isTodayDataAvailable = false;
		return;
	}
	
	if (timeinfo_ptr != NULL && timeinfo_ptr->tm_mday != currentTimeinfo.tm_mday) {
		debugPrint(1, "Fetched data is not for the current day. Ignoring.");
		isTodayDataAvailable = false;
		
		// V5.2 FIX: Reset the display state to force the NO_DATA_OFFSET screen.
		if (displayState != NO_DATA_OFFSET) {
			displayState = CURRENT_PRICES; 
		}
		
		return;
	}

	// Data is for today, proceed with processing
	isTodayDataAvailable = true; 
	
	// Calculate daily average and find min/max using hourly averages
	float sum = 0;
	float minPrice = 999999; // Initialize with large value
	float maxPrice = -999999; // Initialize with small value
	lowestPriceIndex = 0;
	highestPriceIndex = 0;
	
	// Calculate hourly averages and find min/max among them
	for (int hour = 0; hour < 24; hour++) {
		float hourlyAvg = getHourlyAverage(hour, prices);
		if (hourlyAvg > 0) { // Only consider valid hourly averages
			sum += hourlyAvg;
			
			if (hourlyAvg < minPrice) {
				minPrice = hourlyAvg;
				lowestPriceIndex = hour * 4; // Convert back to 15-minute index for display
			}
			if (hourlyAvg > maxPrice) {
				maxPrice = hourlyAvg;
				highestPriceIndex = hour * 4; // Convert back to 15-minute index for display
			}
		}
	}
	
	averagePrice = sum / 24; // Average of 24 hourly averages
	
	debugPrint(3, "Processed " + String(prices.size()) + " price entries (15-min intervals)");
	debugPrint(3, "Daily average: " + String(averagePrice) + " EUR/MWh");
}

void displayPriceRow(int row, int hourIndex, const JsonArray& prices, const JsonArray& unixSeconds) {
	lcd.setCursor(0, row);
	
	// Enhanced wraparound detection to prevent showing early hours after late hours
	struct tm timeinfo;
	if (getLocalTime(&timeinfo)) {
		int currentHour = timeinfo.tm_hour;
		
		// Case 1: Early hours (0-5) shown when we're in evening (>= 18) - likely wraparound
		if (hourIndex >= 0 && hourIndex <= 5 && currentHour >= 18) {
			lcd.print("                    ");
			return;
		}
		
		// Case 2: General past hours (except near midnight transitions)
		if (hourIndex < currentHour && currentHour < 22) {
			lcd.print("                    ");
			return;
		}
	}
	
	if (hourIndex >= 0 && hourIndex < 24) {
		// Calculate hourly average from 15-minute data
		float rates = getHourlyAverage(hourIndex, prices);
		
		// Use the first 15-minute timestamp of the hour for time display
		int dataIndex = hourIndex * 4;
		if (dataIndex < (int)unixSeconds.size()) {
			unsigned long hourlyUnixTime = unixSeconds[dataIndex].as<unsigned long>();

			if (isValidUnixTime(hourlyUnixTime)) {
				time_t t = (time_t)hourlyUnixTime;
				struct tm *time_ptr = localtime(&t);
				
				if (time_ptr != NULL) {
					char buffer[21]; // 💡 OPTIMIZED: Use a buffer for formatting
					int hour = time_ptr->tm_hour;
					int minute = 0; // Always show full hour (00 minutes)
					snprintf(buffer, sizeof(buffer), "%02d:%02d", hour, minute);
					//snprintf(buffer, sizeof(buffer), "%2dh", hour);
					lcd.print(buffer);

					// Price indicators based on hourly averages
					if (dataIndex == lowestPriceIndex) {
						lcd.setCursor(7, row);
						lcd.write(byte(3)); 	// Low price indicator
						lcd.print("   ");
					} else if (dataIndex == highestPriceIndex) {
						lcd.setCursor(7, row);
						lcd.write(byte(4)); 	// High price indicator
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
	JsonArray prices = doc["price"];
	JsonArray unixSeconds = doc["unix_seconds"];
	
	if (!isTodayDataAvailable) {
		for (int i = 0; i < 4; i++) {
			lcd.setCursor(0, i);
			lcd.print("No data available   ");
		}
		return;
	}
	
	// Find current hour base index
	int baseHour = 0;
	struct tm timeinfo;
	if (getLocalTime(&timeinfo)) {
		baseHour = timeinfo.tm_hour; // Always use current hour as base
	}
	
	// Calculate display hours with smart end-of-day handling
	int displayStartHour = baseHour + timeOffsetHours;
	
	// Special end-of-day adjustment: if we're at hour 21-23, modify the base for scrolling
	if (baseHour >= 21 && timeOffsetHours > 0) {
		// Allow seeing hours 21, 22, 23 by adjusting the base
		int adjustedBase = 21;
		displayStartHour = adjustedBase + timeOffsetHours;
	}
	
	// Handle day wraparound
	if (displayStartHour >= 24) {
		displayStartHour = displayStartHour % 24;
	}
	
	// Row 0: 15-minute details for current hour (MOVED TO TOP)
	display15MinuteDetails(0, displayStartHour, prices);
	
	// Row 1: Current hour average (MOVED TO SECOND)
	displayPriceRow(1, displayStartHour, prices, unixSeconds);
	
	// Row 2: Next hour average
	int nextHour = (displayStartHour + 1) % 24;
	displayPriceRow(2, nextHour, prices, unixSeconds);
	
	// Row 3: Hour after that
	int hourAfter = (displayStartHour + 2) % 24;
	displayPriceRow(3, hourAfter, prices, unixSeconds);
}

// V2.4a: Enhanced secondary list with 16 scrollable lines
void displaySecondaryList() {
	// Generate all 16 lines of content
	char lines[SECONDARY_LIST_TOTAL_LINES][21]; // 💡 OPTIMIZED: Use char arrays

	// Line 0: Current Date & Time (HH:MM      DD.MM.YYYY)
	struct tm timeinfo;
	if (getLocalTime(&timeinfo)) {
		// V5.2 FIX: Reduced spaces from 6 to 3 to ensure fit: HH:MM   DD.MM.YYYY (5 + 3 + 10 = 18 chars)
		snprintf(lines[0], sizeof(lines[0]), "%d:%02d     %d.%d.%04d", 
					timeinfo.tm_hour, timeinfo.tm_min, 
					timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);
	} else {
		snprintf(lines[0], sizeof(lines[0]), "Time not available  ");
	}
	
	// Line 1: Separator
	snprintf(lines[1], sizeof(lines[1]), "--------------------");
	
	// Line 2: "Zadnja posodobitev:" header
	snprintf(lines[2], sizeof(lines[2]), "Zadnja posodobitev:");
	
	// Line 3: Timestamp (DD.MM.YYYY ob HH:MM)
	struct tm* timeinfo_ptr = localtime(&lastSuccessfulFetchTime);
	if (timeinfo_ptr != NULL) {
		snprintf(lines[3], sizeof(lines[3]), "%d.%d.%04d ob %2d:%02d", 
					timeinfo_ptr->tm_mday, timeinfo_ptr->tm_mon + 1, timeinfo_ptr->tm_year + 1900,
					timeinfo_ptr->tm_hour, timeinfo_ptr->tm_min);
	} else {
		snprintf(lines[3], sizeof(lines[3]), "No data timestamp   ");
	}
	
	// Line 4: Empty separator
	snprintf(lines[4], sizeof(lines[4]), "");
	
	// Line 5: "Dnevno povpre~je:" header
	snprintf(lines[5], sizeof(lines[5]), "Dnevno povpre^je:");
	
	// Line 6: Daily average value OR "Cene niso na voljo"
	if (isTodayDataAvailable) {
		float finalPrice = averagePrice;
		if (APPLY_FEES_AND_VAT) {
			finalPrice = finalPrice * (1 + POWER_COMPANY_FEE_PERCENTAGE / 100.0) * (1 + VAT_PERCENTAGE / 100.0);
		}
		char priceBuffer[21];
		snprintf(priceBuffer, sizeof(priceBuffer), "%.4f EUR/kWh", finalPrice / 1000.0);
		String numStr = String(priceBuffer);
		numStr.replace('.', ',');
		snprintf(lines[6], sizeof(lines[6]), numStr.c_str());
	} else {
		snprintf(lines[6], sizeof(lines[6]), "Cene niso na voljo.");
	}

	
	// Line 7: Empty separator
	snprintf(lines[7], sizeof(lines[7]), "");
	
	// Line 8: WiFi signal strength
	if (WiFi.status() == WL_CONNECTED) {
		snprintf(lines[8], sizeof(lines[8]), "WiFi:%5d dBm", WiFi.RSSI());
	} else {
		snprintf(lines[8], sizeof(lines[8]), "WiFi: Disconnected");
	}
	
	// Line 9: IP Address
	if (WiFi.status() == WL_CONNECTED) {
		snprintf(lines[9], sizeof(lines[9]), "IP:%15s", WiFi.localIP().toString().c_str());
	} else {
		snprintf(lines[9], sizeof(lines[9]), "IP:   Not connected");
	}
	
	// Line 10: API Success Rate
	int totalApiCalls = apiSuccessCount + apiFailCount;
	if (totalApiCalls > 0) {
		int successRate = (apiSuccessCount * 100) / totalApiCalls;
		snprintf(lines[10], sizeof(lines[10]), "API: %d%% (%d/%d)", successRate, apiSuccessCount, totalApiCalls);
	} else {
		snprintf(lines[10], sizeof(lines[10]), "API: No calls yet");
	}
	
	// Line 11: System uptime
	unsigned long uptimeMs = millis();
	unsigned long days = uptimeMs / (24 * 60 * 60 * 1000);
	uptimeMs %= (24 * 60 * 60 * 1000);
	unsigned long hours = uptimeMs / (60 * 60 * 1000);
	uptimeMs %= (60 * 60 * 1000);
	unsigned long minutes = uptimeMs / (60 * 1000);
	snprintf(lines[11], sizeof(lines[11]), "Up:  %ldd %ldh %ldm", days, hours, minutes);
	
	// Lines 12-15: Static credits
	snprintf(lines[12], sizeof(lines[12]), "energy-charts.info");
	snprintf(lines[13], sizeof(lines[13]), "dynamic electricity");
	snprintf(lines[14], sizeof(lines[14]), "price ticker v5.5");
	snprintf(lines[15], sizeof(lines[15]), "by Amir Toki^ (2025)");
	
	// Display current 4-line window based on scroll offset
	for (int i = 0; i < 4; i++) {
		int lineIndex = secondaryListOffset + i;
		lcd.setCursor(0, i);
		
		if (lineIndex < SECONDARY_LIST_TOTAL_LINES) {
			lcdPrint(lines[lineIndex]);
			// Clear remainder
			for (int j = strlen(lines[lineIndex]); j < 20; j++) {
				lcd.print(" ");
			}
		} else {
			// Clear empty lines
			lcd.print("                    ");
		}
	}
}

void displayPrices() {
	lcd.clear();
	// MODIFIED: This check has been moved to the start of the function
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

	// *** MODIFICATION START - V5.4 CORE FIX: Absolute State Enforcement ***
	if (!isTodayDataAvailable) {
		// If data is unavailable (LED off), absolutely force the NO_DATA_OFFSET state.
		// This resolves the screen/LED desync and ensures stale data is not displayed.
		displayState = NO_DATA_OFFSET;
		timeOffsetHours = 0; // Reset scroll position too
	} else if (displayState == NO_DATA_OFFSET) {
		// If data IS available now, and we were stuck on the NO_DATA screen, reset to CURRENT_PRICES.
		displayState = CURRENT_PRICES;
		timeOffsetHours = 0;
	}
	// *** MODIFICATION END ***


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
	// NEW: Reset the initialBoot flag after the first successful display update.
	initialBoot = false;
}

void resetDisplayToTop() {
	// Always reset to show current hour (offset 0)
	timeOffsetHours = 0;
	secondaryListOffset = 0;
	displayState = CURRENT_PRICES;
	currentList = PRIMARY_LIST;
	
	debugPrint(2, "Auto-scroll timeout - Reset display to current hour");
	displayPrices();
}

void advanceDisplayOffset() {
	// MODIFIED: This function now requires isTimeSynced to be true
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
		// Get current hour for scrolling logic
		struct tm timeinfo;
		int currentHour = 0;
		if (getLocalTime(&timeinfo)) {
			currentHour = timeinfo.tm_hour;
		}
		
		// Calculate maximum offset with minimum scrollability at end of day
		int maxOffset = 23 - currentHour;
		
		// Ensure minimum scrollability at end of day (can see at least 2 previous hours)
		if (maxOffset < 2 && currentHour >= 21) {
			maxOffset = 2;
		}
		
		// Advance by 1 hour, but don't exceed the maximum
		int nextOffsetHours = timeOffsetHours + 1;

		if (nextOffsetHours > maxOffset) {
			// Reset to beginning (current hour)
			timeOffsetHours = 0;
			displayState = CURRENT_PRICES;
		} else {
			timeOffsetHours = nextOffsetHours;
		}
	}
	displayPrices();
}

void toggleList() {
	// MODIFIED: This function now requires isTimeSynced to be true
	if (!isTimeSynced) return;
	currentList = (currentList == PRIMARY_LIST) ? SECONDARY_LIST : PRIMARY_LIST;
	debugPrint(2, "Toggled to " + String(currentList == PRIMARY_LIST ? "PRIMARY" : "SECONDARY") + " list");
	displayPrices();
}

void handleButton() {
	// MODIFIED: Button presses are now handled even if time isn't synced
	int reading = !digitalRead(buttonPin);
	
	if (reading != lastButtonState) {
		lastDebounceTime = millis();
		
		if (reading == LOW) {
			buttonPressStartTime = millis();
			longPressDetected = false;
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
					
					// Clear screen and show message immediately
					lcd.clear();
					lcd.setCursor(0, 0);
					lcd.print("Manual Refresh...");
					lcd.setCursor(0, 1);
					lcd.print("Please wait...");
					
					// Force the fetch immediately
					time_t now;
					time(&now);
					nextScheduledFetchTime = now;
					
				} else if (pressDuration < longPressThreshold) {
					unsigned long currentTime = millis();
					
					if (waitingForDoubleClick && (currentTime - lastClickTime <= doubleClickWindow)) {
						waitingForDoubleClick = false;
						pendingClick = false;
						lastButtonActivity = millis();
						autoScrollExecuted = false;
						debugPrint(2, "Double-click detected - toggling list");
						toggleList();
					} else {
						lastClickTime = currentTime;
						waitingForDoubleClick = true;
						pendingClick = true;
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
		pendingClick = false;
		lastButtonActivity = millis();
		autoScrollExecuted = false;
		debugPrint(3, "Single click confirmed - advancing display");
		advanceDisplayOffset();
	}
	
	lastButtonState = reading;
}

void handleBacklight() {
	if (!presenceSensorConnected) {
		lcd.backlight();
		areLedsOn = true;
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
// SCHEDULING AND DATA FETCHING
// ========================================================================

void handleDataFetching() {
	// MODIFIED: Now requires isTimeSynced to be true to run
	if (!isTimeSynced) {
		return;
	}
	time_t now;
	time(&now);
	
	if (now >= nextScheduledFetchTime) {
		debugPrint(2, "Scheduled data fetch triggered");
		fetchAndProcessData();
		
		struct tm* timeinfo_ptr = localtime(&now);
		if (timeinfo_ptr != NULL) {
			lastSuccessfulFetchDay = timeinfo_ptr->tm_mday;
			
			// Only reschedule to the top of the next hour if we aren't already aggressively retrying
			// Aggressive retries are 300s (HTTP failed) or 600s (Stale data)
			if (nextScheduledFetchTime < now + 300) { 
				time_t nextHour = now - (timeinfo_ptr->tm_min * 60) - timeinfo_ptr->tm_sec + 3600;
				nextScheduledFetchTime = nextHour;
				debugPrint(2, "Next fetch scheduled for the top of the next hour.");
			}
		}
	}
}

// ========================================================================
// SETUP AND MAIN LOOP
// ========================================================================

void setup() {
	Serial.begin(115200);
	debugPrint(2, "Starting Dynamic Electricity Ticker v5.5 (15-MINUTE DETAIL MODE) - DST fixed");
	
	pinMode(builtinLedPin, OUTPUT);
	pinMode(whiteLedPin, OUTPUT);
	pinMode(buttonPin, INPUT_PULLUP);
	
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
	lcd.print("Initializing...");
	delay(1000);
	
	// MODIFIED: connectToWiFi() no longer just connects, it also provisions
	connectToWiFi();
	
	if (WiFi.status() == WL_CONNECTED) {
		// MODIFIED: Use timezone-aware config so DST is applied automatically.
		// This will make localtime() and conversions of API unix timestamps
		// respect CET <-> CEST transitions.
		configTzTime(TZ_CET_CEST, "pool.ntp.org");
		// REMOVED: No more blocking `waitUntilTimeIsSet()` call
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
		return; // Do not run main ticker loop logic
	}
	
	// NEW: Non-blocking time sync check
	if (!isTimeSynced) {
		struct tm timeinfo;
		if (getLocalTime(&timeinfo)) {
			debugPrint(2, "NTP synchronization successful");
			isTimeSynced = true;
			lastButtonActivity = millis();
			autoScrollExecuted = false;
			// First fetch after time sync
			time_t now;
			time(&now);
			nextScheduledFetchTime = now;
			// Track the current day so we can detect midnight rollover later
			trackedDay = timeinfo.tm_mday;
			debugPrint(2, "Tracked day set to: " + String(trackedDay));
			displayPrices(); // Update display to show real data
		} else {
			// Display syncing message while we wait for time
			lcd.setCursor(0, 0);
			lcd.print("Connecting...");
			lcd.setCursor(0, 1);
			lcd.print("Syncing Time...");
			return; // Skip the rest of the loop until time is synced
		}
	}

	// NEW: Detect local day rollover (midnight) and immediately switch to NO_DATA_OFFSET
	time_t now_for_daycheck = time(nullptr);
	struct tm* daycheck = localtime(&now_for_daycheck);
	if (daycheck != nullptr) {
		if (trackedDay == -1) {
			// First-time set if somehow not set earlier
			trackedDay = daycheck->tm_mday;
		} else if (daycheck->tm_mday != trackedDay) {
			// Day rollover detected
			trackedDay = daycheck->tm_mday;
			debugPrint(1, "Midnight rollover detected - switching to NO_DATA_OFFSET until today's data available");
			isTodayDataAvailable = false;
			displayState = NO_DATA_OFFSET;
			timeOffsetHours = 0;
			// Ensure LEDs are turned off
			digitalWrite(whiteLedPin, LOW);
			// Aggressively attempt to fetch immediately for new day's data
			nextScheduledFetchTime = now_for_daycheck;
			// Update the display so users see "No data for today" right away
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
		unsigned long currentHour = timeinfo->tm_hour;
		unsigned long currentMinute = timeinfo->tm_min;
		
		// Hourly refresh at top of hour
		if (currentMinute == 0 && lastHourlyRefresh != currentHour) {
			debugPrint(2, "Top of hour refresh - Hour: " + String(currentHour));
			displayPrices();
			lastHourlyRefresh = currentHour;
		}
		
		// 15-minute refresh for updating ">" placeholders (at minutes 0, 15, 30, 45)
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