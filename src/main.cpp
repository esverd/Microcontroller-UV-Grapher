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
#define TFT_DARK_ORANGE 0xFC60 
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
// MODIFIED: Default to IP Geolocation
bool useGpsFromSecrets = false; // False = use IP Geolocation (default), True = use secrets.h
float deviceLatitude = MY_LATITUDE;   // Initialize with secrets as fallback
float deviceLongitude = MY_LONGITUDE; // Initialize with secrets as fallback
String locationDisplayStr = "Initializing..."; // Initial display string

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
  while (!Serial && millis() < 2000); 
  Serial.println("\nUV Index Monitor Starting...");

  pinMode(BUTTON_INFO_PIN, INPUT_PULLUP);
  pinMode(BUTTON_MODE_PIN, INPUT_PULLUP);

  // Initialize with secrets.h values as a definitive fallback state
  deviceLatitude = MY_LATITUDE;
  deviceLongitude = MY_LONGITUDE;
  // locationDisplayStr will be updated based on actual mode achieved
  
  useGpsFromSecrets = false; // Default to IP Geolocation for initial setup logic

  initializeForecastData(); 

  tft.init();
  tft.setRotation(1); 
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM); 

  displayMessage("Connecting to WiFi...", "", TFT_YELLOW);
  connectToWiFi(); 

  if (WiFi.status() == WL_CONNECTED) {
    // IP Geolocation is the default
    displayMessage("Fetching IP Location...", "", TFT_SKYBLUE);
    if (fetchLocationFromIp()) { // This updates lat/lon and locationDisplayStr
      Serial.println("IP Geolocation successful on startup.");
      // locationDisplayStr is set by fetchLocationFromIp
      useGpsFromSecrets = false; // Confirm current mode
    } else {
      Serial.println("IP Geolocation failed on startup. Using GPS from secrets.h as fallback.");
      useGpsFromSecrets = true; // Fallback to secrets.h mode
      deviceLatitude = MY_LATITUDE; // Ensure these are set
      deviceLongitude = MY_LONGITUDE;
      locationDisplayStr = "IP Fail>Secrets"; // Update display string
    }

    // Now fetch UV data for the determined location
    // locationDisplayStr should be set appropriately by now
    String currentStatusForDisplay = locationDisplayStr;
    if (currentStatusForDisplay.length() > 18) currentStatusForDisplay = currentStatusForDisplay.substring(0,15) + "...";
    displayMessage("Fetching UV data...", currentStatusForDisplay, TFT_CYAN);
    if (fetchUVData()) { 
      lastUpdateTime = millis(); 
    }
  } else {
    // WiFi failed. Setup will use fallback secrets GPS values for lat/lon.
    // Display will show "Offline" and location based on "Secrets GPS (Fallback)"
    useGpsFromSecrets = true; // No WiFi, so we can't use IP Geo; mark as using secrets
    deviceLatitude = MY_LATITUDE;
    deviceLongitude = MY_LONGITUDE;
    locationDisplayStr = "Offline>Secrets";
    force_display_update = true; // Ensure displayInfo is called
  }
}

// --- Main Loop ---
void loop() {
  handleButtonPress();    
  handleModeButton();     

  unsigned long currentMillis = millis();
  bool needsDataFetch = dataJustUpdated; 
  if (!needsDataFetch && (WiFi.status() == WL_CONNECTED) && (currentMillis - lastUpdateTime >= UPDATE_INTERVAL_MS)) {
      Serial.println("Update interval reached.");
      needsDataFetch = true;
  }

  if (needsDataFetch) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Condition met for fetching new UV data.");
      String tempLocStr = locationDisplayStr; 
      if (tempLocStr.length() > 18) tempLocStr = tempLocStr.substring(0,15) + "...";
      displayMessage("Updating UV data...", tempLocStr, TFT_CYAN); // Inform user

      bool fetchSuccess = fetchUVData(); 

      if (fetchSuccess) {
        lastUpdateTime = currentMillis; 
        Serial.println("UV Data fetch successful.");
      } else {
        Serial.println("UV Data fetch failed. Displaying stale or placeholder data.");
      }
    } else {
      Serial.println("No WiFi connection. Cannot fetch UV data for periodic update.");
      initializeForecastData();    
      lastUpdateTimeStr = "Offline"; 
      dataJustUpdated = true; 
    }
  }

  if (force_display_update || dataJustUpdated) {
    displayInfo();
    force_display_update = false; 
    dataJustUpdated = false;    
  }
  delay(50); 
}


// --- Button Handling ---
void handleModeButton() {
    bool current_mode_button_state = digitalRead(BUTTON_MODE_PIN);

    if (current_mode_button_state == LOW && button_mode_last_state == HIGH) { 
        button_mode_hold_start_time = millis();
        button_mode_is_held = false; 
    } else if (current_mode_button_state == LOW && !button_mode_is_held) { 
        if (millis() - button_mode_hold_start_time > MODE_BUTTON_HOLD_TIME_MS) {
            useGpsFromSecrets = !useGpsFromSecrets; // Toggle the mode
            button_mode_is_held = true; 
            force_display_update = true; 
            dataJustUpdated = true;      // Signal that new weather data needs to be fetched

            if (useGpsFromSecrets) { // Now toggled TO GPS from secrets
                Serial.println("Location Mode: Switched to GPS from secrets.h");
                deviceLatitude = MY_LATITUDE;
                deviceLongitude = MY_LONGITUDE;
                locationDisplayStr = "Secrets GPS";
            } else { // Now toggled TO IP Geolocation
                Serial.println("Location Mode: Switched to IP Geolocation. Attempting IP location fetch...");
                locationDisplayStr = "IP Geo..."; 
                displayInfo(); 
                if (fetchLocationFromIp()) { 
                    Serial.println("IP Geolocation successful.");
                    // locationDisplayStr is updated inside fetchLocationFromIp
                } else {
                    Serial.println("IP Geolocation failed. Reverting to Secrets GPS for this cycle.");
                    // Fallback to secrets for this attempt if IP geo fails mid-operation
                    useGpsFromSecrets = true; // Mark as using secrets
                    deviceLatitude = MY_LATITUDE;
                    deviceLongitude = MY_LONGITUDE;
                    locationDisplayStr = "IP Fail>Secrets";
                }
            }
             Serial.printf("Location set to: %s (Lat: %.4f, Lon: %.4f)\n", locationDisplayStr.c_str(), deviceLatitude, deviceLongitude);
        }
    }
    button_mode_last_state = current_mode_button_state;
}

// ... (handleButtonPress, initializeForecastData, fetchLocationFromIp, displayMessage, connectToWiFi remain the same as previous complete version) ...
// Make sure to copy them from the previous full main.cpp if you don't have them. For brevity here.
// For example:
void handleButtonPress() {
    bool current_info_button_state = digitalRead(BUTTON_INFO_PIN);
    if (current_info_button_state == LOW && button_info_last_state == HIGH) {
        if (millis() - button_info_last_press_time > DEBOUNCE_TIME_MS) {
            showInfoOverlay = !showInfoOverlay;
            button_info_last_press_time = millis();
            force_display_update = true; 
            Serial.printf("Info Button pressed, showInfoOverlay: %s\n", showInfoOverlay ? "true" : "false");
        }
    }
    button_info_last_state = current_info_button_state;
}

void initializeForecastData() {
    Serial.println("Initializing forecast data to defaults.");
    for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
        hourlyUV[i] = -1.0f; 
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
    String url = "http://ip-api.com/json/?fields=status,message,lat,lon,city";
    Serial.print("Fetching IP Geolocation: "); Serial.println(url);
    http.begin(url);
    http.setTimeout(10000); 
    int httpCode = http.GET();
    Serial.print("IP Geolocation HTTP Code: "); Serial.println(httpCode);
    bool success = false;
    if (httpCode == HTTP_CODE_OK) { 
        String payload = http.getString();
        Serial.println("IP Geolocation Payload: " + payload);
        JsonDocument doc; 
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
  tft.setTextFont(2); 
  tft.setTextDatum(MC_DATUM); 
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
    WiFi.disconnect(true,true); 
    delay(100);
    bool connected = false;
    String connected_ssid = "";
    #if defined(WIFI_SSID_1) && defined(WIFI_PASS_1)
      if (strlen(WIFI_SSID_1) > 0 && strlen(WIFI_PASS_1) > 0) { 
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
          WiFi.disconnect(true,true); delay(100); 
        }
      } else {
         Serial.println("WIFI_SSID_1 or WIFI_PASS_1 is not defined or empty in secrets.h");
      }
    #endif
    #if defined(WIFI_SSID_2) && defined(WIFI_PASS_2)
      if (!connected && strlen(WIFI_SSID_2) > 0 && strlen(WIFI_PASS_2) > 0) { 
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
      } else if (!connected) { 
         Serial.println("WIFI_SSID_2 or WIFI_PASS_2 is not defined or empty in secrets.h, or WiFi 1 already connected.");
      }
    #endif

    Serial.println();
    if (connected) {
        Serial.println("WiFi connected!");
        Serial.print("SSID: "); Serial.println(connected_ssid);
        Serial.print("IP address: "); Serial.println(WiFi.localIP());
        #if defined(MY_GMT_OFFSET_SEC) && defined(MY_DAYLIGHT_OFFSET_SEC)
            Serial.println("Configuring time via NTP (using secrets.h fallback offsets initially)...");
            configTime(MY_GMT_OFFSET_SEC, MY_DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");
            struct tm timeinfo;
            if(!getLocalTime(&timeinfo, 10000)){ 
                Serial.println("Failed to obtain initial time from NTP.");
                lastUpdateTimeStr = "Time N/A";
            } else {
                Serial.println("Initial time configured via NTP.");
                char timeStr[10];
                strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo); 
                lastUpdateTimeStr = String(timeStr);
                Serial.print("Initial local time from ESP32: "); Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
            }
        #else
            lastUpdateTimeStr = "Time UTC?";
            Serial.println("Timezone offsets (MY_GMT_OFFSET_SEC, MY_DAYLIGHT_OFFSET_SEC) not defined in secrets.h. Time will be UTC initially.");
            configTime(0, 0, "pool.ntp.org", "time.nist.gov");
            struct tm timeinfo_utc;
            if(getLocalTime(&timeinfo_utc, 5000)){
                 char timeStr[10]; strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo_utc);
                 lastUpdateTimeStr = String(timeStr) + " UTC";
            }
        #endif
    } else {
        Serial.println("Could not connect to any configured WiFi network.");
        displayMessage("WiFi Connection Failed.", "Check credentials.", TFT_RED);
        lastUpdateTimeStr = "Offline"; 
    }
}


bool fetchUVData() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, cannot fetch UV data.");
    initializeForecastData(); 
    lastUpdateTimeStr = "Offline"; 
    dataJustUpdated = true;   
    return false;
  }

  HTTPClient http;
  String apiUrl = openMeteoUrl +
                  "?latitude=" + String(deviceLatitude, 4) + 
                  "&longitude=" + String(deviceLongitude, 4) +
                  "&hourly=uv_index&forecast_days=1&timezone=auto"; 

  Serial.println("----------------------------------------");
  Serial.print("Fetching UV Data from URL: "); Serial.println(apiUrl);
  http.begin(apiUrl);
  http.setTimeout(15000); 
  int httpCode = http.GET();
  Serial.print("Open-Meteo API HTTP Code: "); Serial.println(httpCode);

  bool success = false; 
  if (httpCode == HTTP_CODE_OK) { 
    String payload = http.getString();
    Serial.println("----------------------------------------");

    JsonDocument doc; 
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
      Serial.print(F("deserializeJson() for UV data failed: ")); Serial.println(error.c_str());
      initializeForecastData();
    } else {
      if (doc.containsKey("utc_offset_seconds")) {
        long api_utc_offset_sec = doc["utc_offset_seconds"].as<long>();
        const char* api_timezone_id = doc["timezone"].as<const char*>(); 
        const char* api_timezone_abbr = doc["timezone_abbreviation"].as<const char*>(); 
        
        Serial.printf("Open-Meteo API reports: UTC Offset %lds, TZ ID: %s (%s)\n", 
                      api_utc_offset_sec, 
                      api_timezone_id ? api_timezone_id : "N/A",
                      api_timezone_abbr ? api_timezone_abbr : "N/A");
        configTime(api_utc_offset_sec, 0, "pool.ntp.org", "time.nist.gov");
        Serial.println("ESP32 local time reconfigured using Open-Meteo offset.");
        delay(100); 
      } else {
        Serial.println("Open-Meteo API response did not include 'utc_offset_seconds'. Using existing ESP32 time settings.");
      }

      if (doc.containsKey("hourly") && doc["hourly"].containsKey("time") && doc["hourly"].containsKey("uv_index")) {
        JsonArray hourly_time_list = doc["hourly"]["time"].as<JsonArray>();
        JsonArray hourly_uv_list = doc["hourly"]["uv_index"].as<JsonArray>();
        struct tm timeinfo; 

        if (!getLocalTime(&timeinfo, 5000)) { 
            Serial.println("Failed to obtain ESP32 local time for forecast matching. UV data might be misaligned.");
            initializeForecastData(); 
        } else {
            int currentHourLocal = timeinfo.tm_hour; 
            char timeBuff[60]; 
            strftime(timeBuff, sizeof(timeBuff), "%Y-%m-%d %H:%M:%S (%A)", &timeinfo);
            Serial.printf("ESP32 Current Local Time for matching (post API TZ sync): %s (Hour: %02d)\n", timeBuff, currentHourLocal);

            int startIndex = -1;
            for (int i = 0; i < hourly_time_list.size(); ++i) {
                String api_time_str = hourly_time_list[i].as<String>(); 
                if (api_time_str.length() >= 13) { 
                    int api_hour = api_time_str.substring(11, 13).toInt();
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
                        if (uv_val_variant.isNull()) { 
                            hourlyUV[i] = 0.0f; 
                        } else {
                           hourlyUV[i] = uv_val_variant.as<float>();
                        }
                        if (hourlyUV[i] < 0) hourlyUV[i] = 0.0f; 

                        String api_t_str = hourly_time_list[startIndex + i].as<String>();
                        forecastHours[i] = api_t_str.substring(11, 13).toInt();
                    } else {
                        hourlyUV[i] = -1.0f; forecastHours[i] = -1; 
                    }
                }
                success = true; 
                Serial.println("Successfully populated hourly forecast data.");
            } else {
                Serial.println(">>> No suitable starting index found in API's hourly forecast for the ESP32's current local hour.");
                initializeForecastData(); 
            }
        }
      } else {
        Serial.println("Hourly UV data structure ('hourly', 'time', 'uv_index') not found in JSON response.");
        initializeForecastData();
      }
    }
    struct tm timeinfo_update;
    if(getLocalTime(&timeinfo_update, 1000)){ 
      char timeStr[10];
      strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo_update);
      lastUpdateTimeStr = String(timeStr);
      Serial.print("Refreshed 'lastUpdateTimeStr' to: "); Serial.println(lastUpdateTimeStr);
    } else {
      Serial.println("Could not get local time to update 'lastUpdateTimeStr' after data fetch attempt.");
    }
  } else { 
    Serial.print("Error on HTTP request to Open-Meteo. HTTP Code: "); Serial.println(httpCode);
    if (httpCode > 0) Serial.printf("  Error details: %s\n", http.errorToString(httpCode).c_str());
    else Serial.printf("  Error details: %s (likely network issue or timeout)\n", http.errorToString(httpCode).c_str());
    initializeForecastData(); 
    if (WiFi.status() != WL_CONNECTED) lastUpdateTimeStr = "Offline";
  }
  
  http.end();
  dataJustUpdated = true; 
  return success; 
}


void displayInfo() {
  tft.fillScreen(TFT_BLACK);
  int padding = 4;
  int top_y_offset = padding; 

  tft.setTextFont(2); 
  int info_font_height = tft.fontHeight(2); 
  int base_top_text_line_y = padding + info_font_height / 2;

  if (showInfoOverlay) {
    int current_info_y = base_top_text_line_y; 
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
    current_info_y += (info_font_height + padding); 
    tft.setTextDatum(TL_DATUM); 
    tft.setTextColor(TFT_SKYBLUE, TFT_BLACK); 
    String locText;
    if (useGpsFromSecrets) { // If explicitly using Secrets GPS
        locText = "Loc (GPS): " + String(deviceLatitude, 2) + ", " + String(deviceLongitude, 2);
    } else if (locationDisplayStr.startsWith("IP:")) { // If using IP and it was successful
        locText = locationDisplayStr; // Already "IP: City" or "IP: Unknown"
    } else if (locationDisplayStr.startsWith("IP Fail") || locationDisplayStr.startsWith("Offline")){ // Special states
        locText = "Loc: " + locationDisplayStr;
    }
     else { // Default if IP geo hasn't run or other state
        locText = "Loc: " + locationDisplayStr; // Show current status like "IP Geo..." or error
    }

    if(locText.length() > 32) locText = locText.substring(0,29) + "..."; 
    tft.drawString(locText, padding, current_info_y);
    top_y_offset = current_info_y + info_font_height / 2 + padding * 2; 
  } else { 
    if (WiFi.status() != WL_CONNECTED) {
      tft.setTextDatum(MC_DATUM); 
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.drawString("! No WiFi Connection !", tft.width() / 2, base_top_text_line_y);
      top_y_offset = base_top_text_line_y + info_font_height / 2 + padding * 2; 
    }
  }
  drawForecastGraph(top_y_offset);
}


void drawForecastGraph(int start_y_offset) {
    int padding = 2;

    // --- Font Size Definitions ---
    int first_uv_val_font = 6;
    int other_uv_val_font = 4;
    int hour_label_font = 2;

    // --- Calculate Text Heights ---
    tft.setTextFont(first_uv_val_font); int first_uv_text_h = tft.fontHeight();
    tft.setTextFont(other_uv_val_font); int other_uv_text_h = tft.fontHeight();
    tft.setTextFont(hour_label_font);   int hour_label_text_h = tft.fontHeight();

    // --- Y-Position Calculations for "glanceable" layout ---
    int first_uv_value_y = start_y_offset + padding + first_uv_text_h / 2;
    int other_uv_values_line_y = first_uv_value_y + (first_uv_text_h / 2) - (other_uv_text_h / 2);
    if (HOURLY_FORECAST_COUNT <= 1) {
        other_uv_values_line_y = first_uv_value_y;
    }
    int hour_label_y = tft.height() - padding - hour_label_text_h / 2;
    int graph_baseline_y = hour_label_y - hour_label_text_h / 2 - padding;

    // Max bar height: from baseline up to just below the start_y_offset
    int max_bar_pixel_height = graph_baseline_y - (start_y_offset + padding);
    // Ensure at least 1px per UV unit up to the new MAX_UV_FOR_FULL_SCALE if space is very tight
    if (max_bar_pixel_height < 10) max_bar_pixel_height = 10; // Changed from 8 to 10
    if (max_bar_pixel_height < 20 && tft.height() > 100) max_bar_pixel_height = 20; // Min 2px per unit if MAX_UV is 10


    // --- Calculate pixel chunk per UV unit for linear scaling ---
    const float MAX_UV_FOR_FULL_SCALE = 10.0f; // CHANGED: UV 10 is now max height
    float pixel_chunk_per_uv_unit = 0;
    if (MAX_UV_FOR_FULL_SCALE > 0) {
        pixel_chunk_per_uv_unit = max_bar_pixel_height / MAX_UV_FOR_FULL_SCALE;
    }


    // --- X-Position Calculations ---
    int graph_area_total_width = tft.width() - 2 * padding;
    int bar_slot_width = graph_area_total_width / HOURLY_FORECAST_COUNT;
    int bar_actual_width = bar_slot_width * 0.75;
    if (bar_actual_width < 4) bar_actual_width = 4;
    if (bar_actual_width > 30) bar_actual_width = 30;
    int graph_area_x_start = (tft.width() - (bar_slot_width * HOURLY_FORECAST_COUNT)) / 2 + padding;


    #if DEBUG_GRAPH_DRAWING
    if (true) {
        Serial.println("\n--- Graph Drawing Debug (Linear Chunked Scale UV10 Max) ---");
        Serial.printf("Screen H: %d, W: %d\n", tft.height(), tft.width());
        Serial.printf("start_y_offset: %d\n", start_y_offset);
        Serial.printf("Font Heights: UV1(F%d)=%d, UV_other(F%d)=%d, HourLabel(F%d)=%d\n", first_uv_val_font, first_uv_text_h, other_uv_val_font, other_uv_text_h, hour_label_font, hour_label_text_h);
        Serial.printf("Y Pos: UV1_val_Y=%d, UV_other_Y=%d, HourLabel_Y=%d\n", first_uv_value_y, other_uv_values_line_y, hour_label_y);
        Serial.printf("Y Pos: GraphBaseline_Y=%d\n", graph_baseline_y);
        Serial.printf("MaxBarPixelHeight (available for UV %.0f+): %d\n", MAX_UV_FOR_FULL_SCALE, max_bar_pixel_height);
        Serial.printf("PixelChunkPerUVUnit: %.2f\n", pixel_chunk_per_uv_unit);
        Serial.printf("Bar Slot W: %d, Actual W: %d, Area Start X: %d\n", bar_slot_width, bar_actual_width, graph_area_x_start);
    }
    #endif

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

        float uvVal = hourlyUV[i]; // Still needed for the original data
        int roundedUV = (uvVal >= -0.01f) ? round(uvVal) : -1; // This is what we'll use for scaling and color

        #if DEBUG_GRAPH_DRAWING
        Serial.printf("Bar %d: CenterX=%d, Hour=%d, UVRaw=%.2f, UVRounded=%d", i, bar_center_x, forecastHours[i], uvVal, roundedUV);
        #endif

        if (roundedUV != -1 && forecastHours[i] != -1) {
            // --- Bar Height Calculation using roundedUV and chunks ---
            float uv_for_height_calc = (float)roundedUV; // Use the integer roundedUV for scaling steps

            if (uv_for_height_calc < 0) uv_for_height_calc = 0; // Should already be handled by roundedUV logic
            if (uv_for_height_calc > MAX_UV_FOR_FULL_SCALE) {
                uv_for_height_calc = MAX_UV_FOR_FULL_SCALE; // Cap at 10 for height calculation
            }

            int bar_height = round(uv_for_height_calc * pixel_chunk_per_uv_unit);

            // Ensure minimum visibility for very low UV values if they would round to 0 pixels
            // This check is based on the original float uvVal to catch true 0 vs small float
            if (uvVal > 0 && uvVal < 0.5f && bar_height == 0) bar_height = 1; // Tiny bar for UV > 0 but < 0.5
            // If roundedUV is 1 or more, ensure at least a small visible bar if chunks are very small
            else if (roundedUV >= 1 && bar_height == 0 && pixel_chunk_per_uv_unit > 0) bar_height = 1; // Min 1px if roundedUV >=1 and calc is 0
            else if (roundedUV >= 1 && bar_height < 2 && pixel_chunk_per_uv_unit > 2) bar_height = 2;


            if (bar_height > max_bar_pixel_height) bar_height = max_bar_pixel_height; // Safety cap
            if (bar_height < 0) bar_height = 0;
            // --- End of Bar Height Calculation ---

            int bar_top_y = graph_baseline_y - bar_height;

            // YOUR LATEST UV COLOR SCALE
            uint16_t barColor;
            if (roundedUV < 1) {         // UV 0
                barColor = TFT_DARKGREY;
            } else if (roundedUV <= 3) { // UV 1-3
                barColor = TFT_GREEN;
            } else if (roundedUV <= 5) { // UV 4-5
                barColor = TFT_YELLOW;
            } else if (roundedUV <= 7) { // UV 6-7 (Condition changed to <= 7 based on common understanding)
                                         // If you strictly meant only UV 6 for dark orange, change back to roundedUV <= 6
                barColor = TFT_DARK_ORANGE; // Use your defined TFT_DARK_ORANGE or a hex value like 0xFC60
            } else {                     // UV 8+
                barColor = TFT_RED;
            }


            #if DEBUG_GRAPH_DRAWING
            Serial.printf(", UVforH=%.1f, BarH=%d, BarTopY=%d, BarColor=0x%X", uv_for_height_calc, bar_height, bar_top_y, barColor);
            #endif

            if (bar_height > 0) {
                tft.fillRect(bar_center_x - bar_actual_width / 2, bar_top_y, bar_actual_width, bar_height, barColor);
            }

            // --- Draw UV Value Text ---
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
            // Display the roundedUV value, as this is what the bar height and color are based on
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
    Serial.println("--- End of Graph Draw Cycle (Linear Chunked Scale UV10 Max) ---");
    #endif
}