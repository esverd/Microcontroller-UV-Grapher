#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include "secrets.h" // Your secrets

// --- Configuration ---
String openMeteoUrl = "https://api.open-meteo.com/v1/forecast";
const unsigned long UPDATE_INTERVAL_MS = 15 * 60 * 1000;
unsigned long lastUpdateTime = 0;
const int WIFI_CONNECTION_TIMEOUT_MS = 15000;

// --- Global Variables ---
TFT_eSPI tft = TFT_eSPI();
String lastUpdateTimeStr = "Never"; // Will be 24-hour format

const int HOURLY_FORECAST_COUNT = 6;
float hourlyUV[HOURLY_FORECAST_COUNT];
int forecastHours[HOURLY_FORECAST_COUNT];

// Button and Display State
#define BUTTON_INFO_PIN 0 
#define BUTTON_MODE_PIN 35 // New button for location mode
bool showInfoOverlay = false;
uint32_t button_info_last_press_time = 0;
bool button_info_last_state = HIGH; 

// Location Mode Button State
uint32_t button_mode_hold_start_time = 0;
bool button_mode_last_state = HIGH;
bool button_mode_is_held = false;
const uint16_t MODE_BUTTON_HOLD_TIME_MS = 1000; // 1 second hold to toggle

const uint16_t DEBOUNCE_TIME_MS = 70; 
bool force_display_update = true; 
bool dataJustUpdated = false; 

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
  Serial.println("\nUV Index Monitor (Layout V3.1 - Fixes) Starting...");

  pinMode(BUTTON_INFO_PIN, INPUT_PULLUP); 
  pinMode(BUTTON_MODE_PIN, INPUT_PULLUP); // Initialize mode button pin

  deviceLatitude = MY_LATITUDE;   // Ensure initial values are from secrets
  deviceLongitude = MY_LONGITUDE;
  locationDisplayStr = "Secrets GPS";

  initializeForecastData();

  tft.init();
  tft.setRotation(1); 
  
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);

  displayMessage("Connecting to WiFi...", "", TFT_YELLOW);
  connectToWiFi(); 

  if (WiFi.status() == WL_CONNECTED) {
    displayMessage("Fetching UV data...", "", TFT_CYAN);
    if (fetchUVData()) { 
      lastUpdateTime = millis();
    }
  }
}

// --- Main Loop ---
void loop() {
  handleButtonPress();    // For info overlay
  handleModeButton();     // For location mode toggle, can set dataJustUpdated = true

  unsigned long currentMillis = millis();

  // Check if it's time to update data (either by interval or by mode change)
  // The 'dataJustUpdated' flag from handleModeButton signals an immediate need to fetch.
  if (dataJustUpdated || (currentMillis - lastUpdateTime >= UPDATE_INTERVAL_MS)) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Condition met for fetching new UV data.");
      displayMessage("Updating UV data...", locationDisplayStr, TFT_CYAN); // Show what we are doing

      bool fetchSuccess = fetchUVData(); // This function sets its own dataJustUpdated to true on success/failure

      if (fetchSuccess) {
        lastUpdateTime = currentMillis; // Update timestamp only on successful fetch
        Serial.println("UV Data fetch successful.");
      } else {
        Serial.println("UV Data fetch failed. Displaying stale or placeholder data.");
        // If fetch fails, we might want to revert lastUpdateTime so it tries sooner,
        // or rely on dataJustUpdated being set by fetchUVData to show placeholders.
        // For now, displayInfo() will be called anyway.
      }
    } else {
      Serial.println("No WiFi connection. Cannot fetch UV data.");
      initializeForecastData();    // Clear out old data
      lastUpdateTimeStr = "Never"; // Update time string
      // We need to ensure the display reflects this state.
      // fetchUVData() already sets dataJustUpdated if WiFi is down,
      // but if we don't call it, set it here.
      dataJustUpdated = true;
    }
    // After attempting a fetch (or deciding not to due to WiFi),
    // dataJustUpdated should be true if new data was processed or if an error state needs displaying.
    // force_display_update is also useful if only the overlay changed.
  }

  // Display updates if forced or if new data has been processed
  if (force_display_update || dataJustUpdated) {
    displayInfo();
    force_display_update = false; // Reset flag after display
    dataJustUpdated = false;    // Reset flag after display
  }
  delay(50);
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
            dataJustUpdated = true;      // Force a weather data re-fetch

            if (useGpsFromSecrets) {
                Serial.println("Location Mode: Switched to GPS from secrets.h");
                deviceLatitude = MY_LATITUDE;
                deviceLongitude = MY_LONGITUDE;
                locationDisplayStr = "Secrets GPS";
                // No need to fetch IP if we switched TO secrets
            } else {
                Serial.println("Location Mode: Switched to IP Geolocation. Fetching IP location...");
                locationDisplayStr = "IP Geo..."; // Temporary display
                displayInfo(); // Show "IP Geo..." immediately
                if (fetchLocationFromIp()) { // This function will update deviceLatitude/Longitude
                    Serial.println("IP Geolocation successful.");
                    // locationDisplayStr is updated inside fetchLocationFromIp on success
                } else {
                    Serial.println("IP Geolocation failed. Reverting to Secrets GPS.");
                    useGpsFromSecrets = true; // Revert on failure
                    deviceLatitude = MY_LATITUDE;
                    deviceLongitude = MY_LONGITUDE;
                    locationDisplayStr = "IP Fail>GPS";
                }
            }
            // After mode change, new weather data will be fetched because dataJustUpdated is true
        }
    }
    button_mode_last_state = current_mode_button_state;
}

void handleButtonPress() {
    // Read the current state of the INFO button
    bool current_info_button_state = digitalRead(BUTTON_INFO_PIN);

    // Check for a falling edge (button press: HIGH to LOW)
    if (current_info_button_state == LOW && button_info_last_state == HIGH) {
        // Check if enough time has passed since the last registered press (debounce)
        if (millis() - button_info_last_press_time > DEBOUNCE_TIME_MS) {
            showInfoOverlay = !showInfoOverlay;                // Toggle the overlay state
            button_info_last_press_time = millis();            // Record the time of this valid press
            force_display_update = true;                       // Signal that the display needs to be redrawn
            Serial.printf("Info Button pressed, showInfoOverlay: %s\n", showInfoOverlay ? "true" : "false");
        }
    }

    // Always update the last known state of the info button for the next loop iteration
    button_info_last_state = current_info_button_state;
}

// --- Function Definitions ---

void initializeForecastData() {
    for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
        hourlyUV[i] = -1.0; 
        forecastHours[i] = -1;
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
    // For frequent use or commercial projects, consider their paid plans or other services.
    String url = "http://ip-api.com/json/?fields=status,message,lat,lon,city";
    Serial.print("Fetching IP Geolocation: "); Serial.println(url);

    http.begin(url);
    int httpCode = http.GET();
    Serial.print("IP Geolocation HTTP Code: "); Serial.println(httpCode);

    bool success = false;
    if (httpCode > 0) {
        String payload = http.getString();
        Serial.println("IP Geolocation Payload: " + payload);

        JsonDocument doc; // Auto-adjusts size, good for small payloads
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
        Serial.print("IP Geolocation HTTP Error: "); Serial.println(httpCode);
        locationDisplayStr = String("IP (HTTP Err)");
    }
    http.end();
    return success;
}

void displayMessage(String msg_line1, String msg_line2, int color) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.setTextFont(2); 
  tft.drawString(msg_line1, tft.width() / 2, tft.height() / 2 - 10);
  if (msg_line2 != "") {
    tft.drawString(msg_line2, tft.width() / 2, tft.height() / 2 + 10);
  }
  Serial.println(msg_line1 + " " + msg_line2);
}

void connectToWiFi() {
    Serial.println("Connecting to WiFi using secrets.h values...");
    WiFi.mode(WIFI_STA);
    bool connected = false;
    String connected_ssid = "";

    #if defined(WIFI_SSID_1) && defined(WIFI_PASS_1)
      if (strlen(WIFI_SSID_1) > 0) {
        Serial.print("Attempting SSID: "); Serial.println(WIFI_SSID_1);
        WiFi.begin(WIFI_SSID_1, WIFI_PASS_1);
        unsigned long startTime = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_CONNECTION_TIMEOUT_MS) { delay(500); Serial.print("."); }
        if (WiFi.status() == WL_CONNECTED) { connected = true; connected_ssid = WIFI_SSID_1; } else { WiFi.disconnect(true); delay(100); }
      }
    #endif
    #if defined(WIFI_SSID_2) && defined(WIFI_PASS_2)
      if (!connected && strlen(WIFI_SSID_2) > 0) {
        Serial.print("\nAttempting SSID: "); Serial.println(WIFI_SSID_2);
        WiFi.begin(WIFI_SSID_2, WIFI_PASS_2);
        unsigned long startTime = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_CONNECTION_TIMEOUT_MS) { delay(500); Serial.print("."); }
        if (WiFi.status() == WL_CONNECTED) { connected = true; connected_ssid = WIFI_SSID_2; } else { WiFi.disconnect(true); delay(100); }
      }
    #endif

    Serial.println();
    if (connected) {
        Serial.println("WiFi connected!"); Serial.print("SSID: "); Serial.println(connected_ssid); Serial.print("IP address: "); Serial.println(WiFi.localIP());
        #if defined(MY_GMT_OFFSET_SEC) && defined(MY_DAYLIGHT_OFFSET_SEC)
            Serial.println("Configuring time via NTP...");
            configTime(MY_GMT_OFFSET_SEC, MY_DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");
            struct tm timeinfo;
            if(!getLocalTime(&timeinfo, 5000)){
                Serial.println("Failed to obtain time from NTP."); lastUpdateTimeStr = "Time N/A";
            } else {
                Serial.println("Time configured via NTP.");
                char timeStr[10]; 
                strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo); // 24-HOUR FORMAT
                lastUpdateTimeStr = String(timeStr);
            }
        #else
            lastUpdateTimeStr = "Time UTC?"; Serial.println("Timezone offsets not defined in secrets.h.");
        #endif
    } else {
        Serial.println("Could not connect to any WiFi network from secrets.h.");
    }
}

bool fetchUVData() {
  if (WiFi.status() != WL_CONNECTED) { 
    Serial.println("WiFi not connected, cannot fetch UV data.");
    initializeForecastData(); 
    dataJustUpdated = true; 
    return false; 
  }

  HTTPClient http;
  String apiUrl = openMeteoUrl + "?latitude=" + String(deviceLatitude, 4) +
                  "&longitude=" + String(deviceLongitude, 4) +
                  "&hourly=uv_index&forecast_days=1&timezone=auto"; 
  Serial.println("----------------------------------------");
  Serial.print("Fetching URL: "); Serial.println(apiUrl); // DEBUG: Print full URL
  http.begin(apiUrl);
  int httpCode = http.GET();
  Serial.print("HTTP Code: "); Serial.println(httpCode); // DEBUG: Print HTTP status

  if (httpCode > 0) {
    String payload = http.getString();
    Serial.println("JSON Payload received:"); // DEBUG: Print full JSON payload
    Serial.println(payload);                 // DEBUG
    Serial.println("----------------------------------------");


    JsonDocument doc; 
    DeserializationError error = deserializeJson(doc, payload);
    if (error) { 
      Serial.print(F("deserializeJson() failed: ")); Serial.println(error.c_str());
      initializeForecastData(); http.end(); 
      dataJustUpdated = true; 
      return false; 
    }
    
    if (doc.containsKey("hourly") && doc["hourly"].containsKey("time") && doc["hourly"].containsKey("uv_index")) {
        JsonArray hourly_time_list = doc["hourly"]["time"].as<JsonArray>();
        JsonArray hourly_uv_list = doc["hourly"]["uv_index"].as<JsonArray>();
        struct tm timeinfo;
        if (!getLocalTime(&timeinfo, 5000)) { // Attempt to get ESP32's local time
            Serial.println("Failed to obtain ESP32 local time for forecast matching.");
            initializeForecastData();
        } else {
            int currentHourLocal = timeinfo.tm_hour; // ESP32's current local hour
            char timeBuff[30];
            strftime(timeBuff, sizeof(timeBuff), "%Y-%m-%d %H:%M:%S (%A)", &timeinfo);
            Serial.printf("ESP32 Current Local Time for matching: %s (Hour: %02d)\n", timeBuff, currentHourLocal); // DEBUG

            int startIndex = -1;
            Serial.println("API Hourly Times (should be local to GPS due to timezone=auto):"); // DEBUG
            for (int i = 0; i < hourly_time_list.size(); ++i) {
                String api_time_str = hourly_time_list[i].as<String>(); 
                Serial.print("  API slot "); Serial.print(i); Serial.print(": "); Serial.println(api_time_str); // DEBUG
                if (api_time_str.length() >= 13) {
                    int api_hour = api_time_str.substring(11, 13).toInt();
                    // We are looking for the first API hour that is >= ESP32's current local hour
                    if (api_hour >= currentHourLocal) { 
                        startIndex = i;
                        Serial.printf(">>> Match Found: Forecast startIndex = %d (API hour %02d >= ESP32 local hour %02d)\n", startIndex, api_hour, currentHourLocal); // DEBUG
                        break; 
                    }
                }
            }

            if (startIndex != -1) {
                for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
                    if (startIndex + i < hourly_uv_list.size() && startIndex + i < hourly_time_list.size()) {
                        hourlyUV[i] = hourly_uv_list[startIndex + i].as<float>();
                        if (hourlyUV[i] < 0) hourlyUV[i] = 0; 
                        String api_t_str = hourly_time_list[startIndex + i].as<String>();
                        forecastHours[i] = api_t_str.substring(11, 13).toInt();
                        Serial.printf("  Populating Forecast slot %d: Hour %02d, UV %.1f\n", i, forecastHours[i], hourlyUV[i]); // DEBUG
                    } else {
                        hourlyUV[i] = -1.0; forecastHours[i] = -1;
                        Serial.printf("  Populating Forecast slot %d: Not enough data\n", i); // DEBUG
                    }
                }
            } else {
                Serial.println(">>> No suitable starting index found in hourly forecast data for the ESP32's current local hour."); // DEBUG
                initializeForecastData(); 
            }
        }
    } else {
      Serial.println("Hourly UV data structure not found in JSON. Check 'hourly', 'time', 'uv_index' keys."); // DEBUG
      initializeForecastData(); 
    }
    struct tm timeinfo_update;
    if(getLocalTime(&timeinfo_update, 1000)){
      char timeStr[10]; 
      strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo_update); 
      lastUpdateTimeStr = String(timeStr);
    }
    http.end();
    dataJustUpdated = true; 
    return true;
  } else {
    Serial.print("Error on HTTP request. HTTP Code: "); Serial.println(httpCode); // DEBUG
    if (httpCode == HTTP_CODE_NOT_FOUND) Serial.println("Check API URL and parameters. Resource not found.");
    else if (httpCode == HTTP_CODE_BAD_REQUEST) Serial.println("Bad request. Check API parameters like lat/lon format.");
    // You can add more specific error messages for different HTTP codes.
    initializeForecastData(); http.end(); 
    dataJustUpdated = true; 
    return false;
  }
}

void displayInfo() {
  tft.fillScreen(TFT_BLACK); 
  int padding = 4; 
  int top_y_offset = padding; // Default top padding for graph if no overlay/warning

  tft.setTextFont(2); // Font for info text
  int info_font_height = tft.fontHeight(2); 
  
  // Declare top_row_text_y here so it's in scope for all branches below
  // This is the Y-coordinate for the center of a single line of text at the top.
  int base_top_text_line_y = padding + info_font_height / 2;

  if (showInfoOverlay) {
    int current_info_y = base_top_text_line_y; // Start Y for the first line of info

    // Line 1: WiFi and Update Time
    tft.setTextDatum(TL_DATUM);
    if (WiFi.status() == WL_CONNECTED) {
      tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
      String ssid_str = WiFi.SSID();
      if (ssid_str.length() > 16) ssid_str = ssid_str.substring(0,13) + "...";
      tft.drawString("WiFi: " + ssid_str, padding, current_info_y);
    } else {
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.drawString("WiFi: Offline", padding, current_info_y);
    }
    
    tft.setTextDatum(TR_DATUM); 
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("Upd: " + lastUpdateTimeStr, tft.width() - padding, current_info_y);

    // Move Y down for the second line of info (Location)
    current_info_y += (info_font_height + padding); 

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_SKYBLUE, TFT_BLACK); 
    String locText;
    if (locationDisplayStr.startsWith("IP:")) { 
        locText = locationDisplayStr; // Already formatted "IP: City" or "IP: Unknown"
    } else if (locationDisplayStr.startsWith("Secrets")) {
        locText = "Loc: " + String(deviceLatitude, 2) + ", " + String(deviceLongitude, 2);
    } else { // Fallback or other statuses like "IP Fail>GPS"
        locText = "Loc: " + locationDisplayStr;
    }

    if(locText.length() > 32) locText = locText.substring(0,29) + "..."; // Ensure it fits width
    tft.drawString(locText, padding, current_info_y);
    
    // Calculate offset for graph to start below all info text
    top_y_offset = current_info_y + info_font_height / 2 + padding; 

  } else { // Overlay is OFF
    if (WiFi.status() != WL_CONNECTED) {
      tft.setTextDatum(MC_DATUM); 
      tft.setTextColor(TFT_RED, TFT_BLACK);
      // Use base_top_text_line_y for the single warning message
      tft.drawString("! No WiFi Connection !", tft.width() / 2, base_top_text_line_y); 
      // Calculate offset for graph to start below the warning
      top_y_offset = base_top_text_line_y + info_font_height / 2 + padding; 
    }
    // If overlay is off AND WiFi is connected, top_y_offset remains its initial value (`padding`),
    // allowing the graph to use almost the full screen height.
  }
  drawForecastGraph(top_y_offset); 
}

void drawForecastGraph(int start_y_offset) {
    int padding = 2; 

    // --- Font Size Definitions ---
    int first_uv_val_font = 6;   // Prominent font for the first (leftmost) UV value
    int other_uv_val_font = 2;   // Reverted to Font 2 for reliability for the next 5 UV values
    int hour_label_font = 2;     // Font for all hour labels below bars

    // --- Calculate Text Heights ---
    tft.setTextFont(first_uv_val_font);
    int first_uv_text_h = tft.fontHeight();
    tft.setTextFont(other_uv_val_font);
    int other_uv_text_h = tft.fontHeight();
    tft.setTextFont(hour_label_font);
    int hour_label_text_h = tft.fontHeight();

    // --- Y-Position Calculations ---
    int first_uv_value_y = start_y_offset + padding + first_uv_text_h / 2;
    int other_uv_values_line_y = first_uv_value_y + first_uv_text_h / 2 + padding + other_uv_text_h / 2;
    if (HOURLY_FORECAST_COUNT <= 1) { 
        other_uv_values_line_y = first_uv_value_y; 
    }

    int hour_label_y = tft.height() - padding - hour_label_text_h / 2;
    int graph_baseline_y = hour_label_y - hour_label_text_h / 2 - padding;
    
    int bottom_of_uv_texts = (HOURLY_FORECAST_COUNT > 1) ? (other_uv_values_line_y + other_uv_text_h / 2) : (first_uv_value_y + first_uv_text_h / 2);
    int max_bar_pixel_height = graph_baseline_y - (bottom_of_uv_texts + padding);
    
    if (max_bar_pixel_height > tft.height() * 0.55) max_bar_pixel_height = tft.height() * 0.55; 
    if (max_bar_pixel_height < 15) max_bar_pixel_height = 15; 

    // --- X-Position Calculations ---
    int graph_area_total_width = tft.width() - 2 * padding;
    int bar_slot_width = graph_area_total_width / HOURLY_FORECAST_COUNT;
    int bar_actual_width = bar_slot_width * 0.80; 
    if (bar_actual_width < 5) bar_actual_width = 5;
    int graph_area_x_start = (tft.width() - (bar_slot_width * HOURLY_FORECAST_COUNT)) / 2; // Center the block of bars

    // --- Drawing Loop ---
    for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
        int bar_center_x = graph_area_x_start + (i * bar_slot_width) + (bar_slot_width / 2);

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

        // DEBUG: Print data for each bar slot
        // Serial.printf("Draw Bar %d: Hour=%d, Raw UV=%.2f, Rounded UV=%d, ", i, forecastHours[i], uvVal, roundedUV);

        if (roundedUV != -1 && forecastHours[i] != -1) { 
            // ... (bar height and color calculation as before) ...
            float max_uv_for_scaling = 12.0f; 
            float uvValForBarScaling = (uvVal > max_uv_for_scaling) ? max_uv_for_scaling : ((uvVal < 0) ? 0 : uvVal);
            int bar_height = (int)((uvValForBarScaling / max_uv_for_scaling) * max_bar_pixel_height);
            if (bar_height < 0) bar_height = 0;
            if (bar_height == 0 && roundedUV == 0 && uvVal >= 0) bar_height = 1; 
            else if (bar_height == 0 && roundedUV > 0) bar_height = 2;
            int bar_top_y = graph_baseline_y - bar_height;
            uint16_t barColor = (roundedUV < 1) ? TFT_DARKGREY : ((roundedUV < 3) ? TFT_GREEN : ((roundedUV < 6) ? TFT_YELLOW : ((roundedUV < 8) ? TFT_ORANGE : ((roundedUV < 11) ? TFT_RED : TFT_VIOLET))));
            // Serial.printf("BarH=%d, BarColor=0x%X, ", bar_height, barColor);

            tft.fillRect(bar_center_x - bar_actual_width / 2, bar_top_y, bar_actual_width, bar_height, barColor);

            // --- Draw UV Value Text (KEY CHANGE: Using explicit background color for all numbers) ---
            uint16_t uvTextColor = TFT_WHITE;
            if (barColor == TFT_YELLOW || barColor == TFT_ORANGE) {
                uvTextColor = TFT_BLACK; 
            }
            // Serial.printf("UVTextColor=0x%X\n", uvTextColor);
            
            tft.setTextDatum(MC_DATUM); // Ensure datum is set before each drawString
            if (i == 0) { 
                tft.setTextFont(first_uv_val_font); // Font 6 for first value
                tft.setTextColor(uvTextColor, TFT_BLACK); // Explicit black background
                tft.drawString(String(roundedUV), bar_center_x, first_uv_value_y);
            } else { 
                tft.setTextFont(other_uv_val_font); // Font 2 for other values
                tft.setTextColor(uvTextColor, TFT_BLACK); // Explicit black background
                tft.drawString(String(roundedUV), bar_center_x, other_uv_values_line_y);
            }
            
        } else { 
            // Serial.println("--> No valid UV/Hour data for this slot, drawing placeholders.");
            int placeholder_y;
            int placeholder_font;
            if (i == 0) { placeholder_font = first_uv_val_font; placeholder_y = first_uv_value_y; }
            else { placeholder_font = other_uv_val_font; placeholder_y = other_uv_values_line_y; }
            
            tft.setTextFont(placeholder_font);
            tft.setTextColor(TFT_DARKGREY, TFT_BLACK); 
            tft.setTextDatum(MC_DATUM);
            tft.drawString("-", bar_center_x, placeholder_y); 
        }
    }
    // Serial.println("--- End of Graph Draw Cycle ---");
}