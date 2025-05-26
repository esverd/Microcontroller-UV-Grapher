#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include "secrets.h" // Your secrets

// --- Configuration ---
String openMeteoUrl = "https://api.open-meteo.com/v1/forecast";
const unsigned long UPDATE_INTERVAL_MS = 15 * 60 * 1000; // 15 minutes
unsigned long lastUpdateTime = 0; // Stores millis() of last successful data fetch
const int WIFI_CONNECTION_TIMEOUT_MS = 15000;

// --- Debugging Flags ---
#define DEBUG_GRAPH_DRAWING 1 // Set to 1 to enable detailed graph drawing logs, 0 to disable

// --- Global Variables ---
TFT_eSPI tft = TFT_eSPI();
String lastUpdateTimeStr = "Never"; // Will be 24-hour format (e.g., "14:30")

const int HOURLY_FORECAST_COUNT = 6;
float hourlyUV[HOURLY_FORECAST_COUNT];
int forecastHours[HOURLY_FORECAST_COUNT];

// Button and Display State
#define BUTTON_INFO_PIN 0
#define BUTTON_MODE_PIN 35
bool showInfoOverlay = false;
uint32_t button_info_last_press_time = 0;
bool button_info_last_state = HIGH;

// Location Mode Button State
uint32_t button_mode_hold_start_time = 0;
bool button_mode_last_state = HIGH;
bool button_mode_is_held = false;
const uint16_t MODE_BUTTON_HOLD_TIME_MS = 1000; // 1 second hold to toggle

const uint16_t DEBOUNCE_TIME_MS = 70;
bool force_display_update = true; // Flag to signal the display needs a full redraw
bool dataJustUpdated = false;     // Flag to signal that new weather data has been fetched or an attempt was made

// Location Management
bool useGpsFromSecrets = true; // True = use secrets.h, False = use IP Geolocation
float deviceLatitude = MY_LATITUDE;   // Initialize with secrets, will be updated
float deviceLongitude = MY_LONGITUDE; // Initialize with secrets, will be updated
String locationDisplayStr = "Secrets"; // To show on info overlay

// --- Function Declarations ---
void connectToWiFi();
bool fetchUVData();
void displayInfo();
void drawForecastGraph(int start_y_offset);
void displayMessage(String msg_line1, String msg_line2 = "", int color = TFT_WHITE);
void initializeForecastData();
void handleButtonPress();
void handleModeButton();
bool fetchLocationFromIp();

// --- Setup ---
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000); // Wait for serial, but not forever
  Serial.println("\nUV Index Monitor Starting...");

  pinMode(BUTTON_INFO_PIN, INPUT_PULLUP);
  pinMode(BUTTON_MODE_PIN, INPUT_PULLUP);

  // Initialize location from secrets.h
  deviceLatitude = MY_LATITUDE;
  deviceLongitude = MY_LONGITUDE;
  locationDisplayStr = "Secrets GPS"; // Initial display string

  initializeForecastData(); // Set forecast arrays to default/invalid values

  tft.init();
  tft.setRotation(1); // Adjust for your display (0-3)
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM); // Default text datum

  displayMessage("Connecting to WiFi...", "", TFT_YELLOW);
  connectToWiFi(); // Attempt to connect

  if (WiFi.status() == WL_CONNECTED) {
    displayMessage("Fetching UV data...", locationDisplayStr, TFT_CYAN);
    if (fetchUVData()) { // This sets dataJustUpdated
      lastUpdateTime = millis(); // Record time of successful fetch
    }
  } else {
    // dataJustUpdated will be set by fetchUVData if WiFi is not connected
    // or connectToWiFi might have already set an error message.
    // Ensure displayInfo is called to show the offline status.
    force_display_update = true;
  }
  // displayInfo() will be called in the first loop iteration due to force_display_update or dataJustUpdated
}

// --- Main Loop ---
void loop() {
  handleButtonPress();    // For info overlay
  handleModeButton();     // For location mode toggle, can set dataJustUpdated = true

  unsigned long currentMillis = millis();

  // Check if it's time to update data (either by interval or by mode change)
  // The 'dataJustUpdated' flag, if set by handleModeButton, signals an immediate need to fetch.
  // Note: fetchUVData() also sets dataJustUpdated internally.
  bool needsDataFetch = dataJustUpdated; // Prioritize fetch if mode button triggered it
  if (!needsDataFetch && (WiFi.status() == WL_CONNECTED) && (currentMillis - lastUpdateTime >= UPDATE_INTERVAL_MS)) {
      Serial.println("Update interval reached.");
      needsDataFetch = true;
  }


  if (needsDataFetch) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Condition met for fetching new UV data.");
      // Inform user. displayMessage clears the screen, which might be too much if updates are frequent.
      // Consider a less intrusive notification if this is an issue.
      // For now, it clearly indicates activity.
      String tempLocStr = locationDisplayStr; // Capture current location string before potential change in fetchUVData
      if (tempLocStr.startsWith("IP:")) tempLocStr = tempLocStr.substring(0,15); // Keep it short
      else if (tempLocStr.startsWith("Secrets")) tempLocStr = "Secrets GPS";

      displayMessage("Updating UV data...", tempLocStr, TFT_CYAN);

      bool fetchSuccess = fetchUVData(); // This function sets its own dataJustUpdated to true on success/failure

      if (fetchSuccess) {
        lastUpdateTime = currentMillis; // Update timestamp only on successful fetch
        Serial.println("UV Data fetch successful.");
      } else {
        Serial.println("UV Data fetch failed. Displaying stale or placeholder data.");
        // dataJustUpdated is set by fetchUVData() to ensure displayInfo() runs
      }
    } else {
      Serial.println("No WiFi connection. Cannot fetch UV data for periodic update.");
      initializeForecastData();    // Clear out old data
      lastUpdateTimeStr = "Offline"; // Update time string to reflect no connection for data
      dataJustUpdated = true; // Signal that display needs to show this new state
    }
    // After attempting a fetch (or deciding not to due to WiFi),
    // dataJustUpdated should be true if new data was processed or if an error state needs displaying.
    // force_display_update is also useful if only the overlay changed.
  }

  // Display updates if forced or if new data has been processed (dataJustUpdated)
  if (force_display_update || dataJustUpdated) {
    displayInfo();
    force_display_update = false; // Reset flag after display
    dataJustUpdated = false;    // Reset flag after display
  }
  delay(50); // Small delay for stability
}


// --- Button Handling ---
void handleModeButton() {
    bool current_mode_button_state = digitalRead(BUTTON_MODE_PIN);

    if (current_mode_button_state == LOW && button_mode_last_state == HIGH) { // Button just pressed
        button_mode_hold_start_time = millis();
        button_mode_is_held = false; // Reset held flag
    } else if (current_mode_button_state == LOW && !button_mode_is_held) { // Button is being held
        if (millis() - button_mode_hold_start_time > MODE_BUTTON_HOLD_TIME_MS) {
            // --- Long Press Detected ---
            useGpsFromSecrets = !useGpsFromSecrets;
            button_mode_is_held = true; // Mark as held so it doesn't re-trigger
            force_display_update = true; // Force a screen update
            dataJustUpdated = true;      // CRITICAL: Signal that new weather data needs to be fetched

            if (useGpsFromSecrets) {
                Serial.println("Location Mode: Switched to GPS from secrets.h");
                deviceLatitude = MY_LATITUDE;
                deviceLongitude = MY_LONGITUDE;
                locationDisplayStr = "Secrets GPS";
                // No need to call fetchLocationFromIp()
                // New weather data will be fetched in main loop because dataJustUpdated is true
            } else {
                Serial.println("Location Mode: Switched to IP Geolocation. Attempting IP location fetch...");
                locationDisplayStr = "IP Geo..."; // Temporary display
                displayInfo(); // Show "IP Geo..." immediately while fetching IP
                if (fetchLocationFromIp()) { // This function will update deviceLatitude/Longitude and locationDisplayStr
                    Serial.println("IP Geolocation successful.");
                } else {
                    Serial.println("IP Geolocation failed. Reverting to Secrets GPS.");
                    useGpsFromSecrets = true; // Revert on failure
                    deviceLatitude = MY_LATITUDE;
                    deviceLongitude = MY_LONGITUDE;
                    locationDisplayStr = "IP Fail>GPS";
                }
            }
            // After mode change, new weather data will be fetched in the main loop
            // because dataJustUpdated is true.
             Serial.printf("Location set to: %s (Lat: %.4f, Lon: %.4f)\n", locationDisplayStr.c_str(), deviceLatitude, deviceLongitude);
        }
    }
    button_mode_last_state = current_mode_button_state;
}

void handleButtonPress() {
    bool current_info_button_state = digitalRead(BUTTON_INFO_PIN);
    if (current_info_button_state == LOW && button_info_last_state == HIGH) {
        if (millis() - button_info_last_press_time > DEBOUNCE_TIME_MS) {
            showInfoOverlay = !showInfoOverlay;
            button_info_last_press_time = millis();
            force_display_update = true; // Signal that the display needs to be redrawn
            Serial.printf("Info Button pressed, showInfoOverlay: %s\n", showInfoOverlay ? "true" : "false");
        }
    }
    button_info_last_state = current_info_button_state;
}

// --- Function Definitions ---

void initializeForecastData() {
    Serial.println("Initializing forecast data to defaults.");
    for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
        hourlyUV[i] = -1.0f; // Use -1.0f to indicate invalid/unset UV data
        forecastHours[i] = -1;    // Use -1 to indicate invalid/unset hour
    }
}

bool fetchLocationFromIp() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Cannot fetch IP location: WiFi not connected.");
        locationDisplayStr = "IP (NoNet)";
        return false;
    }

    HTTPClient http;
    // Using ip-api.com which is free but has usage limits (e.g. 45 req/min from same IP)
    String url = "http://ip-api.com/json/?fields=status,message,lat,lon,city";
    Serial.print("Fetching IP Geolocation: "); Serial.println(url);

    http.begin(url);
    http.setTimeout(10000); // 10 second timeout
    int httpCode = http.GET();
    Serial.print("IP Geolocation HTTP Code: "); Serial.println(httpCode);

    bool success = false;
    if (httpCode == HTTP_CODE_OK) { // Check for 200 OK
        String payload = http.getString();
        Serial.println("IP Geolocation Payload: " + payload);

        JsonDocument doc; // ArduinoJson V6+
        DeserializationError error = deserializeJson(doc, payload);

        if (error) {
            Serial.print(F("deserializeJson() for IP Geo failed: ")); Serial.println(error.c_str());
            locationDisplayStr = "IP (JSON Err)";
        } else {
            const char* status = doc["status"];
            if (status && strcmp(status, "success") == 0) {
                deviceLatitude = doc["lat"].as<float>();
                deviceLongitude = doc["lon"].as<float>();
                const char* city = doc["city"];
                if (city) {
                    locationDisplayStr = String("IP: ") + city;
                } else {
                    locationDisplayStr = "IP: Unknown";
                }
                Serial.printf("IP Geo Location: Lat=%.4f, Lon=%.4f, City=%s\n", deviceLatitude, deviceLongitude, city ? city : "N/A");
                success = true;
            } else {
                const char* message = doc["message"];
                Serial.print("IP Geolocation API Error: "); Serial.println(message ? message : "Unknown error");
                locationDisplayStr = String("IP (API Err)");
            }
        }
    } else {
        Serial.print("IP Geolocation HTTP Error or Timeout. Code: "); Serial.println(httpCode);
        locationDisplayStr = String("IP (HTTP Err)");
    }
    http.end();
    return success;
}

void displayMessage(String msg_line1, String msg_line2, int color) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.setTextFont(2); // Consistent font for messages
  tft.setTextDatum(MC_DATUM); // Ensure it's centered
  int16_t width = tft.width();
  int16_t height = tft.height();

  tft.drawString(msg_line1, width / 2, height / 2 - (msg_line2 != "" ? 10 : 0) );
  if (msg_line2 != "") {
    tft.drawString(msg_line2, width / 2, height / 2 + 10);
  }
  Serial.println("Displaying Message: " + msg_line1 + " " + msg_line2);
}

void connectToWiFi() {
    Serial.println("Connecting to WiFi using secrets.h values...");
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true,true); // Disconnect from any previous session and erase credentials
    delay(100);


    bool connected = false;
    String connected_ssid = "";

    // Attempt WiFi 1
    #if defined(WIFI_SSID_1) && defined(WIFI_PASS_1)
      if (strlen(WIFI_SSID_1) > 0 && strlen(WIFI_PASS_1) > 0) { // Ensure SSID and PASS are not empty
        Serial.print("Attempting SSID: "); Serial.println(WIFI_SSID_1);
        WiFi.begin(WIFI_SSID_1, WIFI_PASS_1);
        unsigned long startTime = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_CONNECTION_TIMEOUT_MS) {
          delay(500); Serial.print(".");
        }
        if (WiFi.status() == WL_CONNECTED) {
          connected = true; connected_ssid = WIFI_SSID_1;
        } else {
          Serial.println("\nFailed to connect to SSID 1.");
          WiFi.disconnect(true,true); delay(100); // Disconnect and clear before trying next
        }
      } else {
         Serial.println("WIFI_SSID_1 or WIFI_PASS_1 is not defined or empty in secrets.h");
      }
    #endif

    // Attempt WiFi 2 if WiFi 1 failed
    #if defined(WIFI_SSID_2) && defined(WIFI_PASS_2)
      if (!connected && strlen(WIFI_SSID_2) > 0 && strlen(WIFI_PASS_2) > 0) { // Ensure SSID and PASS are not empty
        Serial.print("\nAttempting SSID: "); Serial.println(WIFI_SSID_2);
        WiFi.begin(WIFI_SSID_2, WIFI_PASS_2);
        unsigned long startTime = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_CONNECTION_TIMEOUT_MS) {
          delay(500); Serial.print(".");
        }
        if (WiFi.status() == WL_CONNECTED) {
          connected = true; connected_ssid = WIFI_SSID_2;
        } else {
          Serial.println("\nFailed to connect to SSID 2.");
          WiFi.disconnect(true,true); delay(100);
        }
      } else if (!connected) { // Only print this if we haven't connected and SSID2 was the one with issues
         Serial.println("WIFI_SSID_2 or WIFI_PASS_2 is not defined or empty in secrets.h, or WiFi 1 already connected.");
      }
    #endif

    Serial.println();
    if (connected) {
        Serial.println("WiFi connected!");
        Serial.print("SSID: "); Serial.println(connected_ssid);
        Serial.print("IP address: "); Serial.println(WiFi.localIP());

        #if defined(MY_GMT_OFFSET_SEC) && defined(MY_DAYLIGHT_OFFSET_SEC)
            Serial.println("Configuring time via NTP...");
            // configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, [ntpServer2], [ntpServer3])
            configTime(MY_GMT_OFFSET_SEC, MY_DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");
            struct tm timeinfo;
            if(!getLocalTime(&timeinfo, 10000)){ // 10 sec timeout for NTP
                Serial.println("Failed to obtain time from NTP after 10 seconds.");
                lastUpdateTimeStr = "Time N/A";
            } else {
                Serial.println("Time configured via NTP.");
                char timeStr[10];
                strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo); // 24-HOUR FORMAT
                lastUpdateTimeStr = String(timeStr);
                Serial.print("Current local time from ESP32: "); Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
            }
        #else
            lastUpdateTimeStr = "Time UTC?";
            Serial.println("Timezone offsets (MY_GMT_OFFSET_SEC, MY_DAYLIGHT_OFFSET_SEC) not defined in secrets.h. Time will be UTC.");
            // Configure time to UTC if offsets are not defined
            configTime(0, 0, "pool.ntp.org", "time.nist.gov");
            // You might still want to get and display UTC time here
            struct tm timeinfo_utc;
            if(getLocalTime(&timeinfo_utc, 5000)){
                 char timeStr[10]; strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo_utc);
                 lastUpdateTimeStr = String(timeStr) + " UTC";
            }
        #endif
    } else {
        Serial.println("Could not connect to any configured WiFi network.");
        displayMessage("WiFi Connection Failed.", "Check credentials.", TFT_RED);
        lastUpdateTimeStr = "Offline"; // Indicate no connection for data
        // No need to call initializeForecastData here, fetchUVData will handle it if called without WiFi
    }
}

bool fetchUVData() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, cannot fetch UV data.");
    initializeForecastData(); // Set data to invalid
    lastUpdateTimeStr = "Offline"; // Or use specific error for update time
    dataJustUpdated = true;   // Signal display needs to be updated with this state
    return false;
  }

  HTTPClient http;
  // Using String concatenation for URL construction
  String apiUrl = openMeteoUrl +
                  "?latitude=" + String(deviceLatitude, 4) + // Use 4 decimal places for lat/lon
                  "&longitude=" + String(deviceLongitude, 4) +
                  "&hourly=uv_index&forecast_days=1&timezone=auto";

  Serial.println("----------------------------------------");
  Serial.print("Fetching UV Data from URL: "); Serial.println(apiUrl);
  http.begin(apiUrl);
  http.setTimeout(15000); // 15 seconds timeout for API request
  int httpCode = http.GET();
  Serial.print("Open-Meteo API HTTP Code: "); Serial.println(httpCode);

  bool success = false;
  if (httpCode == HTTP_CODE_OK) { // Successful request
    String payload = http.getString();
    Serial.println("JSON Payload received:");
    // Serial.println(payload); // Uncomment for full payload debugging, can be very long
    Serial.println("----------------------------------------");

    JsonDocument doc; // For ArduinoJson v6+
                      // Adjust size if payload is very large and causes issues: StaticJsonDocument<2048> doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
      Serial.print(F("deserializeJson() for UV data failed: ")); Serial.println(error.c_str());
      initializeForecastData();
    } else {
      if (doc.containsKey("hourly") && doc["hourly"].containsKey("time") && doc["hourly"].containsKey("uv_index")) {
        JsonArray hourly_time_list = doc["hourly"]["time"].as<JsonArray>();
        JsonArray hourly_uv_list = doc["hourly"]["uv_index"].as<JsonArray>();
        struct tm timeinfo;

        if (!getLocalTime(&timeinfo, 5000)) { // Attempt to get ESP32's configured local time
            Serial.println("Failed to obtain ESP32 local time for forecast matching. UV data might be misaligned.");
            initializeForecastData(); // Cannot reliably match forecast
        } else {
            int currentHourLocal = timeinfo.tm_hour; // ESP32's current local hour
            char timeBuff[60]; // Increased buffer size for full date-time string
            strftime(timeBuff, sizeof(timeBuff), "%Y-%m-%d %H:%M:%S (%A)", &timeinfo);
            Serial.printf("ESP32 Current Local Time for matching: %s (Hour: %02d)\n", timeBuff, currentHourLocal);

            int startIndex = -1;
            Serial.println("API Hourly Times (should be local to GPS due to timezone=auto):");
            for (int i = 0; i < hourly_time_list.size(); ++i) {
                String api_time_str = hourly_time_list[i].as<String>(); // e.g., "2023-05-15T14:00"
                // Serial.print("  API slot "); Serial.print(i); Serial.print(": "); Serial.println(api_time_str); // Verbose
                if (api_time_str.length() >= 13) { // Ensure "TXX:XX" part exists
                    // Extract hour from "YYYY-MM-DDTHH:MM"
                    int api_hour = api_time_str.substring(11, 13).toInt();
                    // We are looking for the first API hour that is >= ESP32's current local hour
                    // This simple match assumes the API provides data for the current day starting before or at currentHourLocal
                    if (api_hour >= currentHourLocal) {
                        startIndex = i;
                        Serial.printf(">>> Match Found: Forecast startIndex = %d (API hour %02d >= ESP32 local hour %02d)\n", startIndex, api_hour, currentHourLocal);
                        break;
                    }
                }
            }

            if (startIndex != -1) {
                for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
                    if (startIndex + i < hourly_uv_list.size() && startIndex + i < hourly_time_list.size()) {
                        JsonVariant uv_val_variant = hourly_uv_list[startIndex + i];
                        if (uv_val_variant.isNull()) { // Handle null UV values from API
                            hourlyUV[i] = 0.0f; // Or some other indicator like -1.0f if you prefer
                            Serial.printf("  Populating Forecast slot %d: API returned null UV, defaulting to 0.0\n", i);
                        } else {
                           hourlyUV[i] = uv_val_variant.as<float>();
                        }
                        if (hourlyUV[i] < 0) hourlyUV[i] = 0.0f; // Ensure UV is not negative

                        String api_t_str = hourly_time_list[startIndex + i].as<String>();
                        forecastHours[i] = api_t_str.substring(11, 13).toInt();
                        Serial.printf("  Populating Forecast slot %d: Hour %02d, UV %.1f\n", i, forecastHours[i], hourlyUV[i]);
                    } else {
                        hourlyUV[i] = -1.0f; forecastHours[i] = -1; // Mark as invalid if not enough data
                        Serial.printf("  Populating Forecast slot %d: Not enough data from API at index %d\n", i, startIndex + i);
                    }
                }
                success = true; // Data populated
            } else {
                Serial.println(">>> No suitable starting index found in hourly forecast data for the ESP32's current local hour. API data might be for a future period or past.");
                initializeForecastData(); // Couldn't align data
            }
        }
      } else {
        Serial.println("Hourly UV data structure not found in JSON. Check API response format ('hourly', 'time', 'uv_index' keys).");
        initializeForecastData();
      }
    }
    // Update the "Last Updated" time string
    struct tm timeinfo_update;
    if(getLocalTime(&timeinfo_update, 1000)){ // Quick attempt to get time
      char timeStr[10];
      strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo_update);
      lastUpdateTimeStr = String(timeStr);
    } else {
      // If getLocalTime fails here, lastUpdateTimeStr retains its previous value or "Time N/A" from connectToWiFi
      Serial.println("Could not get local time to update 'lastUpdateTimeStr' after data fetch.");
    }
  } else {
    Serial.print("Error on HTTP request to Open-Meteo. HTTP Code: "); Serial.println(httpCode);
    if (httpCode == HTTP_CODE_NOT_FOUND) Serial.println("Check API URL and parameters. Resource not found (404).");
    else if (httpCode == HTTP_CODE_BAD_REQUEST) Serial.println("Bad request (400). Check API parameters like lat/lon format or range.");
    else if (httpCode > 0) Serial.printf("HTTP Error %d: %s\n", httpCode, http.errorToString(httpCode).c_str());
    else Serial.printf("HTTP Request failed. Error: %s\n", http.errorToString(httpCode).c_str()); // e.g. timeout, DNS resolution failed

    initializeForecastData(); // Clear data on error
  }
  http.end();
  dataJustUpdated = true; // Signal that display needs to be updated with new data or error state
  return success;
}

void displayInfo() {
  tft.fillScreen(TFT_BLACK);
  int padding = 4;
  int top_y_offset = padding; // Default top padding for graph if no overlay/warning

  tft.setTextFont(2); // Font for info text
  int info_font_height = tft.fontHeight(2); // Height of Font 2

  // This is the Y-coordinate for the center of a single line of text at the top.
  int base_top_text_line_y = padding + info_font_height / 2;

  if (showInfoOverlay) {
    int current_info_y = base_top_text_line_y; // Start Y for the first line of info

    // Line 1: WiFi and Update Time
    tft.setTextDatum(TL_DATUM); // Top-Left for WiFi SSID
    if (WiFi.status() == WL_CONNECTED) {
      tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
      String ssid_str = WiFi.SSID();
      if (ssid_str.length() > 16) ssid_str = ssid_str.substring(0,13) + "..."; // Truncate if too long
      tft.drawString("WiFi: " + ssid_str, padding, current_info_y);
    } else {
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.drawString("WiFi: Offline", padding, current_info_y);
    }

    tft.setTextDatum(TR_DATUM); // Top-Right for Update Time
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("Upd: " + lastUpdateTimeStr, tft.width() - padding, current_info_y);

    // Move Y down for the second line of info (Location)
    current_info_y += (info_font_height + padding);

    tft.setTextDatum(TL_DATUM); // Top-Left for Location
    tft.setTextColor(TFT_SKYBLUE, TFT_BLACK);
    String locText;
    if (locationDisplayStr.startsWith("IP:")) {
        locText = locationDisplayStr;
    } else if (locationDisplayStr.startsWith("Secrets")) {
        locText = "Loc: " + String(deviceLatitude, 2) + ", " + String(deviceLongitude, 2);
    } else { // Fallback or other statuses like "IP Fail>GPS"
        locText = "Loc: " + locationDisplayStr;
    }
    if(locText.length() > 32) locText = locText.substring(0,29) + "..."; // Ensure it fits
    tft.drawString(locText, padding, current_info_y);

    // Calculate offset for graph to start below all info text
    top_y_offset = current_info_y + info_font_height / 2 + padding * 2; // Added a bit more padding

  } else { // Overlay is OFF
    if (WiFi.status() != WL_CONNECTED) {
      tft.setTextDatum(MC_DATUM); // Middle-Center for warning
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.drawString("! No WiFi Connection !", tft.width() / 2, base_top_text_line_y);
      top_y_offset = base_top_text_line_y + info_font_height / 2 + padding * 2; // More padding
    }
    // If overlay is off AND WiFi is connected, top_y_offset remains its initial value (`padding`),
    // allowing the graph to use almost the full screen height.
  }
  drawForecastGraph(top_y_offset);
}

void drawForecastGraph(int start_y_offset) {
    int padding = 2; // Internal padding for graph elements

    // --- Font Size Definitions ---
    int first_uv_val_font = 6;   // Prominent font for the first (leftmost) UV value
    int other_uv_val_font = 4;   // Font for the next 5 UV values
    int hour_label_font = 2;     // Font for all hour labels below bars

    // --- Calculate Text Heights (once per font) ---
    tft.setTextFont(first_uv_val_font); int first_uv_text_h = tft.fontHeight();
    tft.setTextFont(other_uv_val_font); int other_uv_text_h = tft.fontHeight();
    tft.setTextFont(hour_label_font);   int hour_label_text_h = tft.fontHeight();

    // --- Y-Position Calculations for new "glanceable" layout ---

    // UV Numbers will be positioned near the top of the graph area
    // Center Y for the large UV value of the first bar
    int first_uv_value_y = start_y_offset + padding + first_uv_text_h / 2;
    // Center Y for the other UV values. Let's align their tops with the first UV value's top for a cleaner look.
    // To do this, their center would be: start_y_offset + padding + other_uv_text_h / 2
    // However, to make them distinct, let's place them slightly lower or ensure alignment.
    // For simplicity, let's keep the previous logic where 'other' values are on a separate conceptual line,
    // but this line will be close to the first one.
    // int other_uv_values_line_y = first_uv_value_y + first_uv_text_h / 2 - other_uv_text_h / 2; // Aligns centers if fonts were same, needs adjustment
    // Let's try aligning the baseline of the numbers, or just placing the 'other' numbers slightly below the first large one.
    // A simple approach: the "other" numbers are on a line that starts after the large number, or share a common top y reference.
    // For now, let's keep the previous distinct line for other_uv_values, it worked with Font 6 & 2.
    // With Font 6 & 4, their height difference is less.
    int other_uv_values_line_y = first_uv_value_y + (first_uv_text_h / 2) - (other_uv_text_h / 2) ; // This aligns their Y centers vertically if first is reference.
    // If first_uv_val_font (48px) and other_uv_val_font (26px), this means:
    // first_uv_value_y center. other_uv_values_line_y center will be at the same Y.
    // This is good for glanceability if the numbers should be on one "line".
    // Let's test this alignment:
    if (HOURLY_FORECAST_COUNT <= 1) {
        other_uv_values_line_y = first_uv_value_y;
    }


    // Hour labels at the very bottom
    int hour_label_y = tft.height() - padding - hour_label_text_h / 2;
    // Baseline for bars (where they "sit" and grow upwards from)
    int graph_baseline_y = hour_label_y - hour_label_text_h / 2 - padding;

    // Max bar height: from baseline up to just below the start_y_offset (top of graph area)
    int max_bar_pixel_height = graph_baseline_y - (start_y_offset + padding);
    if (max_bar_pixel_height < 10) max_bar_pixel_height = 10; // Ensure a minimum drawable area

    // --- X-Position Calculations (remain similar) ---
    int graph_area_total_width = tft.width() - 2 * padding;
    int bar_slot_width = graph_area_total_width / HOURLY_FORECAST_COUNT;
    int bar_actual_width = bar_slot_width * 0.75;
    if (bar_actual_width < 4) bar_actual_width = 4;
    if (bar_actual_width > 30) bar_actual_width = 30; // Max bar width for aesthetics
    int graph_area_x_start = (tft.width() - (bar_slot_width * HOURLY_FORECAST_COUNT)) / 2 + padding;

    #if DEBUG_GRAPH_DRAWING
    if (true) {
        Serial.println("\n--- Graph Drawing Debug (Glanceable Layout) ---");
        Serial.printf("Screen H: %d, W: %d\n", tft.height(), tft.width());
        Serial.printf("start_y_offset: %d\n", start_y_offset);
        Serial.printf("Font Heights: UV1(F%d)=%d, UV_other(F%d)=%d, HourLabel(F%d)=%d\n", first_uv_val_font, first_uv_text_h, other_uv_val_font, other_uv_text_h, hour_label_font, hour_label_text_h);
        Serial.printf("Y Pos: UV1_val_Y=%d, UV_other_Y=%d, HourLabel_Y=%d\n", first_uv_value_y, other_uv_values_line_y, hour_label_y);
        Serial.printf("Y Pos: GraphBaseline_Y=%d\n", graph_baseline_y);
        Serial.printf("MaxBarPixelHeight (calc from baseline to start_y_offset+pad): %d\n", max_bar_pixel_height);
        Serial.printf("Bar Slot W: %d, Actual W: %d, Area Start X: %d\n", bar_slot_width, bar_actual_width, graph_area_x_start);
    }
    #endif

    // --- Drawing Loop (bar drawing, text drawing logic largely same, uses new Y coords) ---
    for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
        int bar_center_x = graph_area_x_start + (i * bar_slot_width) + (bar_slot_width / 2);

        // --- Draw Hour Label ---
        tft.setTextFont(hour_label_font);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        if (forecastHours[i] != -1) {
            tft.drawString(String(forecastHours[i]), bar_center_x, hour_label_y);
        } else {
            tft.drawString("-", bar_center_x, hour_label_y);
        }

        float uvVal = hourlyUV[i];
        int roundedUV = (uvVal >= -0.01f) ? round(uvVal) : -1;

        #if DEBUG_GRAPH_DRAWING
        Serial.printf("Bar %d: CenterX=%d, Hour=%d, UVRaw=%.2f, UVRounded=%d", i, bar_center_x, forecastHours[i], uvVal, roundedUV);
        #endif

        if (roundedUV != -1 && forecastHours[i] != -1) {
            // Calculate Bar Height & Color
            float max_uv_for_scaling = 12.0f;
            float uvValForBarScaling = (uvVal > max_uv_for_scaling) ? max_uv_for_scaling : ((uvVal < 0) ? 0 : uvVal);
            int bar_height = (int)((uvValForBarScaling / max_uv_for_scaling) * max_bar_pixel_height);
            if (bar_height < 0) bar_height = 0;
            if (bar_height == 0 && uvVal >= 0 && uvVal < 0.5f) bar_height = 1;
            else if (bar_height < 2 && uvVal >= 0.5f) bar_height = 2;

            int bar_top_y = graph_baseline_y - bar_height;
            uint16_t barColor = (roundedUV < 1) ? TFT_DARKGREY :
                                ((roundedUV < 3) ? TFT_GREEN :
                                ((roundedUV < 6) ? TFT_YELLOW :
                                ((roundedUV < 8) ? TFT_ORANGE :
                                ((roundedUV < 11) ? TFT_RED :
                                TFT_VIOLET))));

            #if DEBUG_GRAPH_DRAWING
            Serial.printf(", BarH=%d, BarTopY=%d, BarColor=0x%X", bar_height, bar_top_y, barColor);
            #endif

            if (bar_height > 0) {
                tft.fillRect(bar_center_x - bar_actual_width / 2, bar_top_y, bar_actual_width, bar_height, barColor);
            }

            // Draw UV Value Text (Always White text on Black background box for visibility)
            tft.setTextColor(TFT_WHITE, TFT_BLACK);
            tft.setTextDatum(MC_DATUM);

            int current_uv_text_y;
            if (i == 0) {
                tft.setTextFont(first_uv_val_font);
                current_uv_text_y = first_uv_value_y;
            } else {
                tft.setTextFont(other_uv_val_font);
                current_uv_text_y = other_uv_values_line_y;
            }
            tft.drawString(String(roundedUV), bar_center_x, current_uv_text_y);

            #if DEBUG_GRAPH_DRAWING
            Serial.printf(", UVTextY=%d (WhiteText)\n", current_uv_text_y);
            #endif

        } else { // Placeholder for invalid data
            int placeholder_y;
            int placeholder_font;
            if (i == 0) { placeholder_font = first_uv_val_font; placeholder_y = first_uv_value_y; }
            else { placeholder_font = other_uv_val_font; placeholder_y = other_uv_values_line_y; }

            tft.setTextFont(placeholder_font);
            tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
            tft.setTextDatum(MC_DATUM);
            tft.drawString("-", bar_center_x, placeholder_y);
            #if DEBUG_GRAPH_DRAWING
            Serial.println(", --> No valid UV/Hour data, drawing placeholders.");
            #endif
        }
    }
    #if DEBUG_GRAPH_DRAWING
    Serial.println("--- End of Graph Draw Cycle (Glanceable) ---");
    #endif
}