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
bool showInfoOverlay = false;
uint32_t button_last_press_time = 0;
bool button_last_state = HIGH; 
const uint16_t DEBOUNCE_TIME_MS = 70; 
bool force_display_update = true; 
bool dataJustUpdated = false; 

// --- Function Declarations ---
void connectToWiFi();
bool fetchUVData();
void displayInfo();
void drawForecastGraph(int start_y_offset); 
void displayMessage(String msg_line1, String msg_line2 = "", int color = TFT_WHITE);
void initializeForecastData();
void handleButtonPress();

// --- Setup ---
void setup() {
  Serial.begin(115200);
  Serial.println("\nUV Index Monitor (Layout V3.1 - Fixes) Starting...");

  pinMode(BUTTON_INFO_PIN, INPUT_PULLUP); 

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
  handleButtonPress();

  if (millis() - lastUpdateTime >= UPDATE_INTERVAL_MS) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Time to update UV data...");
      if(fetchUVData()) { 
        lastUpdateTime = millis();
      }
    } else {
      Serial.println("Cannot update UV data, WiFi not connected.");
      force_display_update = true; 
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
void handleButtonPress() {
    bool current_button_state = digitalRead(BUTTON_INFO_PIN);
    // Edge detection for press (HIGH to LOW transition)
    if (current_button_state == LOW && button_last_state == HIGH && (millis() - button_last_press_time > DEBOUNCE_TIME_MS)) {
        showInfoOverlay = !showInfoOverlay;
        button_last_press_time = millis(); // Record time of this press
        force_display_update = true; 
        Serial.printf("Button pressed, showInfoOverlay: %s\n", showInfoOverlay ? "true" : "false");
    }
    button_last_state = current_button_state;

    // Prevents continuous toggling if button is held down after the initial press
    if (current_button_state == LOW && (millis() - button_last_press_time > 500) && showInfoOverlay) {
         // If button is still held LOW long after the press that turned overlay ON,
         // you might want to do nothing or reset button_last_press_time to prevent quick re-toggle on release.
         // For now, the DEBOUNCE_TIME_MS on press should handle most cases.
    }
}

// --- Function Definitions ---

void initializeForecastData() {
    for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
        hourlyUV[i] = -1.0; 
        forecastHours[i] = -1;
    }
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
  String apiUrl = openMeteoUrl + "?latitude=" + String(MY_LATITUDE, 4) +
                  "&longitude=" + String(MY_LONGITUDE, 4) +
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
  int top_y_offset = padding; 

  tft.setTextFont(2); 
  int info_font_height = tft.fontHeight(2); // Font 2 height approx 16px
  int top_row_text_y = padding + info_font_height / 2 + 2;

  if (showInfoOverlay) {
    tft.setTextDatum(TL_DATUM);
    if (WiFi.status() == WL_CONNECTED) {
      tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
      String ssid_str = WiFi.SSID();
      if (ssid_str.length() > 16) ssid_str = ssid_str.substring(0,13) + "...";
      tft.drawString("WiFi: " + ssid_str, padding, top_row_text_y);
    } else {
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.drawString("WiFi: Offline", padding, top_row_text_y);
    }
    
    tft.setTextDatum(TR_DATUM); 
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("Upd: " + lastUpdateTimeStr, tft.width() - padding, top_row_text_y);
    top_y_offset = info_font_height + padding * 2 + 2; // Approx 16 + 8 + 2 = 26px
  } else {
    if (WiFi.status() != WL_CONNECTED) {
      tft.setTextDatum(MC_DATUM); 
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.drawString("! No WiFi Connection !", tft.width() / 2, top_row_text_y);
      top_y_offset = info_font_height + padding * 2 + 2; 
    }
    // If overlay is off AND WiFi is connected, top_y_offset remains `padding` (e.g. 4px).
    else {
      // If overlay is off AND WiFi is connected, graph can start very near the top
      top_y_offset = padding; // Minimal top padding, graph starts almost immediately
    }
  }
  drawForecastGraph(top_y_offset); 
}

void drawForecastGraph(int start_y_offset) {
    int padding = 2; // Screen edge padding

    // --- Font Size Definitions ---
    int first_uv_val_font = 6;   // Prominent font for the first (leftmost) UV value
    int other_uv_val_font = 4;   // Larger font for the next 5 UV values
    int hour_label_font = 2;     // Readable font for all hour labels below bars

    // --- Calculate Text Heights (crucial for precise layout) ---
    tft.setTextFont(first_uv_val_font);
    int first_uv_text_h = tft.fontHeight();

    tft.setTextFont(other_uv_val_font);
    int other_uv_text_h = tft.fontHeight();

    tft.setTextFont(hour_label_font);
    int hour_label_text_h = tft.fontHeight();

    // --- Y-Position Calculations for Graph Elements ---
    // Y for the large, first UV value (broken out, near the top of graph area)
    int first_uv_value_y = start_y_offset + padding + first_uv_text_h / 2;

    // Y for the line of the other 5 UV values (positioned below the first large one)
    int other_uv_values_line_y = first_uv_value_y + first_uv_text_h / 2 + padding + other_uv_text_h / 2;
    if (HOURLY_FORECAST_COUNT <= 1) { 
        other_uv_values_line_y = first_uv_value_y; 
    }

    // Hour labels are at the very bottom.
    int hour_label_y = tft.height() - padding - hour_label_text_h / 2;

    // Baseline for the bars.
    int graph_baseline_y = hour_label_y - hour_label_text_h / 2 - padding;

    // Max bar height calculation.
    int bottom_of_uv_texts = (HOURLY_FORECAST_COUNT > 1) ? (other_uv_values_line_y + other_uv_text_h / 2) : (first_uv_value_y + first_uv_text_h / 2);
    int max_bar_pixel_height = graph_baseline_y - (bottom_of_uv_texts + padding);
    
    if (max_bar_pixel_height > tft.height() * 0.50) max_bar_pixel_height = tft.height() * 0.50; // Cap at 50% of screen height
    if (max_bar_pixel_height < 15) max_bar_pixel_height = 15; 

    // --- X-Position Calculations ---
    int graph_area_total_width = tft.width() - 2 * padding;
    int bar_slot_width = graph_area_total_width / HOURLY_FORECAST_COUNT;
    int bar_actual_width = bar_slot_width * 0.80; 
    if (bar_actual_width < 5) bar_actual_width = 5;
    // Calculate the starting X to center the entire block of bars
    int graph_block_width = bar_slot_width * HOURLY_FORECAST_COUNT;
    int graph_area_x_start = (tft.width() - graph_block_width) / 2;


    // --- Drawing Loop for Each Forecast Slot ---
    for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
        // Calculate center X for the current bar AND its associated text labels
        int bar_center_x = graph_area_x_start + (i * bar_slot_width) + (bar_slot_width / 2);

        // --- Draw Hour Label ---
        tft.setTextFont(hour_label_font); 
        tft.setTextColor(TFT_WHITE, TFT_BLACK); // Opaque background for clarity
        tft.setTextDatum(MC_DATUM);
        if (forecastHours[i] != -1) {
            tft.drawString(String(forecastHours[i]), bar_center_x, hour_label_y);
        } else {
            tft.drawString("-", bar_center_x, hour_label_y);
        }

        // --- Prepare Bar and UV Value Data ---
        float uvVal = hourlyUV[i]; 
        int roundedUV = -1;       

        if (uvVal >= -0.01f) { 
            roundedUV = round(uvVal); 
        }

        // --- Draw Bar (if data is valid) ---
        if (roundedUV != -1 && forecastHours[i] != -1) { 
            float max_uv_for_scaling = 12.0f; 
            float uvValForBarScaling;
            if (uvVal > max_uv_for_scaling) uvValForBarScaling = max_uv_for_scaling;
            else uvValForBarScaling = (uvVal < 0) ? 0 : uvVal; 
            
            int bar_height = (int)((uvValForBarScaling / max_uv_for_scaling) * max_bar_pixel_height);
            if (bar_height < 0) bar_height = 0;
            if (bar_height == 0 && roundedUV == 0 && uvVal >= 0) bar_height = 1; 
            else if (bar_height == 0 && roundedUV > 0) bar_height = 2;


            int bar_top_y = graph_baseline_y - bar_height;

            uint16_t barColor = TFT_DARKGREY; 
            if (roundedUV < 1) barColor = TFT_DARKGREY;
            else if (roundedUV < 3) barColor = TFT_GREEN;  
            else if (roundedUV < 6) barColor = TFT_YELLOW; 
            else if (roundedUV < 8) barColor = TFT_ORANGE; 
            else if (roundedUV < 11) barColor = TFT_RED;   
            else barColor = TFT_VIOLET;                    

            tft.fillRect(bar_center_x - bar_actual_width / 2, bar_top_y, bar_actual_width, bar_height, barColor);

            // --- Draw UV Value Text ---
            uint16_t uvTextColor = TFT_WHITE;
            uint16_t uvTextBgColor = TFT_BLACK; // Explicit black background for numbers

            if (barColor == TFT_YELLOW || barColor == TFT_ORANGE) {
                uvTextColor = TFT_BLACK; 
            }
            
            int current_uv_text_y;
            if (i == 0) { 
                tft.setTextFont(first_uv_val_font);
                current_uv_text_y = first_uv_value_y;
            } else { 
                tft.setTextFont(other_uv_val_font);
                current_uv_text_y = other_uv_values_line_y;
            }
            tft.setTextColor(uvTextColor, uvTextBgColor); // Set text color WITH OPAQUE BACKGROUND
            tft.setTextDatum(MC_DATUM);
            tft.drawString(String(roundedUV), bar_center_x, current_uv_text_y);
            
        } else { 
            // No valid data, draw placeholders for UV value text
            int placeholder_y;
            if (i == 0) {
                tft.setTextFont(first_uv_val_font);
                placeholder_y = first_uv_value_y;
            } else {
                tft.setTextFont(other_uv_val_font);
                placeholder_y = other_uv_values_line_y;
            }
            tft.setTextColor(TFT_DARKGREY, TFT_BLACK); // Opaque background
            tft.setTextDatum(MC_DATUM);
            tft.drawString("-", bar_center_x, placeholder_y); 
        }
    }
}