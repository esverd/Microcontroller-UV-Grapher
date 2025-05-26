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
// const unsigned long UPDATE_INTERVAL_MS = 60 * 1000; // For faster testing
unsigned long lastUpdateTime = 0;
const int WIFI_CONNECTION_TIMEOUT_MS = 15000;

// --- Global Variables ---
TFT_eSPI tft = TFT_eSPI();
float currentUVIndex = -1.0;
String lastUpdateTimeStr = "Never";

const int HOURLY_FORECAST_COUNT = 6;
float hourlyUV[HOURLY_FORECAST_COUNT];
int forecastHours[HOURLY_FORECAST_COUNT];

// --- Function Declarations ---
void connectToWiFi();
bool fetchUVData();
void displayInfo();
void drawForecastGraph(); // Helper for the graph
void displayMessage(String msg_line1, String msg_line2 = "", int color = TFT_WHITE);
void initializeForecastData();

// --- Setup ---
void setup() {
  Serial.begin(115200);
  Serial.println("\nUV Index Monitor (Layout V2) Starting...");

  initializeForecastData();

  tft.init();
  tft.setRotation(1); // Landscape (USB left for typical TTGO T-Display)
                      // tft.width() = 240, tft.height() = 135
  
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
  displayInfo();
}

// --- Main Loop ---
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    displayMessage("WiFi Disconnected!", "Reconnecting...", TFT_RED);
    delay(2000);
    connectToWiFi();
    if (WiFi.status() == WL_CONNECTED) {
        fetchUVData();
        lastUpdateTime = millis();
    }
  }

  if (millis() - lastUpdateTime >= UPDATE_INTERVAL_MS) {
    if (WiFi.status() == WL_CONNECTED) {
      displayMessage("Updating UV data...", "", TFT_CYAN);
      if(fetchUVData()) {
        lastUpdateTime = millis();
      }
    } else {
      Serial.println("Cannot update UV data, WiFi not connected.");
      displayMessage("WiFi Offline", "Can't update UV", TFT_ORANGE);
      delay(2000);
    }
  }
  displayInfo();
  delay(2000); 
}


// --- Function Definitions ---

void initializeForecastData() {
    for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
        hourlyUV[i] = -1.0; 
        forecastHours[i] = -1;
    }
}

void displayMessage(String msg_line1, String msg_line2, int color) {
  // This function remains the same as your last working version
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
  // This function remains the same as your last working version (using secrets.h)
  // Make sure it includes NTP setup on successful connection.
    Serial.println("Connecting to WiFi using secrets.h values...");
    WiFi.mode(WIFI_STA);
    bool connected = false;
    String connected_ssid = "";

    #if defined(WIFI_SSID_1) && defined(WIFI_PASS_1)
        if (strlen(WIFI_SSID_1) > 0) {
            Serial.print("Attempting SSID: "); Serial.println(WIFI_SSID_1);
            displayMessage("Connecting to:", WIFI_SSID_1, TFT_YELLOW);
            WiFi.begin(WIFI_SSID_1, WIFI_PASS_1);
            unsigned long startTime = millis();
            while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_CONNECTION_TIMEOUT_MS) {
                delay(500); Serial.print(".");
            }
            if (WiFi.status() == WL_CONNECTED) {
                connected = true; connected_ssid = WIFI_SSID_1;
            } else { WiFi.disconnect(true); delay(100); }
        }
    #endif

    #if defined(WIFI_SSID_2) && defined(WIFI_PASS_2)
        if (!connected && strlen(WIFI_SSID_2) > 0) {
            Serial.print("\nAttempting SSID: "); Serial.println(WIFI_SSID_2);
            displayMessage("Connecting to:", WIFI_SSID_2, TFT_YELLOW);
            WiFi.begin(WIFI_SSID_2, WIFI_PASS_2);
            unsigned long startTime = millis();
            while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_CONNECTION_TIMEOUT_MS) {
                delay(500); Serial.print(".");
            }
            if (WiFi.status() == WL_CONNECTED) {
                connected = true; connected_ssid = WIFI_SSID_2;
            } else { WiFi.disconnect(true); delay(100); }
        }
    #endif

    Serial.println();
    if (connected) {
        Serial.println("WiFi connected!");
        Serial.print("SSID: "); Serial.println(connected_ssid);
        Serial.print("IP address: "); Serial.println(WiFi.localIP());
        displayMessage("WiFi Connected!", connected_ssid, TFT_GREEN);
        delay(1500);

        #if defined(MY_GMT_OFFSET_SEC) && defined(MY_DAYLIGHT_OFFSET_SEC)
            Serial.println("Configuring time via NTP using secrets.h timezone...");
            configTime(MY_GMT_OFFSET_SEC, MY_DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");
            struct tm timeinfo;
            if(!getLocalTime(&timeinfo, 5000)){
                Serial.println("Failed to obtain time from NTP.");
                lastUpdateTimeStr = "Time N/A";
            } else {
                Serial.println("Time configured via NTP.");
                char timeStr[20];
                strftime(timeStr, sizeof(timeStr), "%I:%M %p", &timeinfo);
                lastUpdateTimeStr = String(timeStr);
            }
        #else
            Serial.println("Timezone offsets not defined. Time might be UTC or incorrect.");
            lastUpdateTimeStr = "Time (UTC?)";
        #endif
    } else {
        Serial.println("Could not connect to any WiFi network from secrets.h.");
        displayMessage("WiFi Failed.", "Check secrets.h", TFT_RED);
    }
}

bool fetchUVData() {
  // This function remains the same as your last working version.
  // Ensure it includes "&hourly=uv_index&forecast_days=1" (and optionally "&timezone=auto")
  // and populates currentUVIndex, hourlyUV[], and forecastHours[].
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Error: WiFi not connected.");
    currentUVIndex = -1.0; initializeForecastData(); return false;
  }

  HTTPClient http;
  // Added timezone=auto for better hourly forecast alignment
  String apiUrl = openMeteoUrl + "?latitude=" + String(MY_LATITUDE, 4) +
                  "&longitude=" + String(MY_LONGITUDE, 4) +
                  "&current=uv_index&hourly=uv_index&forecast_days=1&timezone=auto"; 

  Serial.print("Fetching URL: "); Serial.println(apiUrl);
  // displayMessage("Fetching UV...", "Hourly data...", TFT_CYAN); // displayMessage clears screen, better to update within displayInfo

  http.begin(apiUrl);
  int httpCode = http.GET();

  if (httpCode > 0) {
    String payload = http.getString();
    Serial.println("HTTP Code: " + String(httpCode));

    JsonDocument doc; 
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
      Serial.print(F("deserializeJson() failed: ")); Serial.println(error.c_str());
      currentUVIndex = -2.0; initializeForecastData(); http.end(); return false;
    }

    if (doc.containsKey("current") && doc["current"].containsKey("uv_index")) {
      currentUVIndex = doc["current"]["uv_index"].as<float>();
      Serial.print("Current UV Index: "); Serial.println(currentUVIndex);
    } else {
      Serial.println("Current UV Index data not found.");
      currentUVIndex = -3.0; 
    }
    
    if (doc.containsKey("hourly") && doc["hourly"].containsKey("time") && doc["hourly"].containsKey("uv_index")) {
        JsonArray hourly_time_list = doc["hourly"]["time"].as<JsonArray>();
        JsonArray hourly_uv_list = doc["hourly"]["uv_index"].as<JsonArray>();
        struct tm timeinfo;
        if (!getLocalTime(&timeinfo, 5000)) {
            Serial.println("Failed to obtain local time for forecast matching.");
            initializeForecastData();
        } else {
            int currentHourLocal = timeinfo.tm_hour; 
            Serial.printf("Current local hour for forecast matching: %02d\n", currentHourLocal);
            int startIndex = -1;
            for (int i = 0; i < hourly_time_list.size(); ++i) {
                String api_time_str = hourly_time_list[i].as<String>(); 
                if (api_time_str.length() >= 13) {
                    int api_hour = api_time_str.substring(11, 13).toInt();
                    if (api_hour >= currentHourLocal) {
                        startIndex = i;
                        Serial.printf("Found forecast starting at index %d, API hour %02d for local hour %02d\n", startIndex, api_hour, currentHourLocal);
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
                        Serial.printf("Forecast slot %d: Hour %02d, UV %.1f\n", i, forecastHours[i], hourlyUV[i]);
                    } else {
                        hourlyUV[i] = -1.0; forecastHours[i] = -1;
                        Serial.printf("Forecast slot %d: Not enough data\n", i);
                    }
                }
            } else {
                Serial.println("Could not find a suitable starting index in hourly forecast data for current local time.");
                initializeForecastData(); 
            }
        }
    } else {
      Serial.println("Hourly UV data not found in JSON response.");
      initializeForecastData(); 
    }
    struct tm timeinfo_update;
    if(getLocalTime(&timeinfo_update, 1000)){
      char timeStr[20];
      strftime(timeStr, sizeof(timeStr), "%I:%M %p", &timeinfo_update);
      lastUpdateTimeStr = String(timeStr);
    }
  } else {
    Serial.print("Error on HTTP request: "); Serial.println(HTTPClient::errorToString(httpCode).c_str());
    currentUVIndex = -4.0; initializeForecastData(); http.end(); return false;
  }
  http.end();
  return true;
}


// --- NEW/UPDATED DISPLAY FUNCTIONS ---

void displayInfo() {
  tft.fillScreen(TFT_BLACK);
  int padding = 4; // General padding from screen edges

  // --- Top Row: WiFi Status & Last Update ---
  tft.setTextFont(2); 
  int top_row_y = padding + tft.fontHeight(2)/2 + 2; // Adjusted for font height and a bit of padding

  tft.setTextDatum(TL_DATUM); // Top-Left datum
  if (WiFi.status() == WL_CONNECTED) {
    tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
    // Shorten SSID if too long
    String ssid_str = WiFi.SSID();
    if (ssid_str.length() > 15) ssid_str = ssid_str.substring(0,12) + "...";
    tft.drawString("WiFi: " + ssid_str, padding, top_row_y);
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("WiFi: Off", padding, top_row_y);
  }
  
  tft.setTextDatum(TR_DATUM); // Top-Right datum
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(lastUpdateTimeStr, tft.width() - padding, top_row_y);

  // --- Middle Section: Current UV Index ---
  // Position this section slightly higher to give more space to graph
  int current_uv_label_y = top_row_y + 18; 
  int current_uv_value_y = current_uv_label_y + 20; 

  tft.setTextDatum(MC_DATUM); 
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextFont(2); 
  tft.drawString("CURRENT UV", tft.width() / 2, current_uv_label_y);

  if (currentUVIndex >= 0) {
    if (currentUVIndex < 3) tft.setTextColor(TFT_GREEN, TFT_BLACK);
    else if (currentUVIndex < 6) tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    else if (currentUVIndex < 8) tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    else if (currentUVIndex < 11) tft.setTextColor(TFT_RED, TFT_BLACK);
    else tft.setTextColor(TFT_VIOLET, TFT_BLACK);
    
    tft.setTextFont(5); // Reduced font size for current UV value (was 7)
    tft.drawFloat(currentUVIndex, 1, tft.width() / 2, current_uv_value_y);
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setTextFont(2);
    String errorMsg = "UV Error"; 
    if(currentUVIndex == -1.0) errorMsg = "No Conn";
    else if(currentUVIndex == -2.0) errorMsg = "JSON Err";
    else if(currentUVIndex == -3.0) errorMsg = "No Data";
    else if(currentUVIndex == -4.0) errorMsg = "HTTP Err";
    tft.drawString(errorMsg, tft.width()/2, current_uv_value_y);
  }

  // --- Bottom Section: Hourly Forecast Graph ---
  drawForecastGraph(); 
}

void drawForecastGraph() {
    int padding = 4;
    // Y position for the "UV Forecast" title, below the current UV section
    int forecast_title_y = 72; // Adjusted based on current UV taking less space

    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextFont(2); // Font for "UV Forecast" title
    tft.drawString("UV Forecast", tft.width() / 2, forecast_title_y);

    // Graph layout parameters
    int graph_area_x_start = padding + 10;
    int graph_area_width = tft.width() - 2 * (padding + 10);
    
    int value_text_height = 8; // Approx height for UV value text (Font 1)
    int hour_text_height = 8;  // Approx height for hour label text (Font 1)
    
    // Y position for the top of the text showing UV value (above the bar)
    int uv_value_on_bar_y = forecast_title_y + 10 + value_text_height / 2;
    
    // Max height for the bars themselves
    int max_bar_pixel_height = 25; // Max height of a bar in pixels
                                   // tft.height() = 135. Title Y ~72. Text above bar Y ~86.
                                   // Leaves from ~86 to ~135-padding for bar+hour label.
                                   // Bar max 25px. Hour label Y will be below bar.
                                   // This should fit.

    // Y position of the baseline where bars "sit"
    int graph_baseline_y = uv_value_on_bar_y + value_text_height / 2 + max_bar_pixel_height + 2; // +2 for small gap

    // Y position for hour labels below the bars
    int hour_label_y = graph_baseline_y + hour_text_height / 2 + 2; // +2 for gap

    int bar_slot_width = graph_area_width / HOURLY_FORECAST_COUNT;
    int bar_actual_width = bar_slot_width * 0.65; // Make bars a bit narrower than their slot for spacing

    for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
        int bar_center_x = graph_area_x_start + (i * bar_slot_width) + (bar_slot_width / 2);

        // Draw Hour Label
        tft.setTextFont(1); // Small font for labels
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        if (forecastHours[i] != -1) {
            tft.drawString(String(forecastHours[i]), bar_center_x, hour_label_y);
        } else {
            tft.drawString("-", bar_center_x, hour_label_y);
        }

        // Draw Bar and UV Value Text
        if (hourlyUV[i] >= 0 && forecastHours[i] != -1) {
            float uvVal = hourlyUV[i];
            if (uvVal > 15) uvVal = 15; // Cap UV for graph scaling (adjust if higher UV expected)
            
            int bar_height = (int)((uvVal / 15.0f) * max_bar_pixel_height);
            if (bar_height < 0) bar_height = 0;
            if (bar_height == 0 && uvVal > 0) bar_height = 1; // Min 1px height if UV > 0

            int bar_top_y = graph_baseline_y - bar_height;

            // Bar color
            uint16_t barColor = TFT_GREEN;
            if (uvVal < 3) barColor = TFT_GREEN;
            else if (uvVal < 6) barColor = TFT_YELLOW;
            else if (uvVal < 8) barColor = TFT_ORANGE;
            else if (uvVal < 11) barColor = TFT_RED;
            else barColor = TFT_VIOLET;
            
            tft.fillRect(bar_center_x - bar_actual_width / 2, bar_top_y, bar_actual_width, bar_height, barColor);

            // UV Value Text (above the bar)
            tft.setTextColor(TFT_WHITE, TFT_BLACK); // White text for UV value
            tft.setTextFont(1); 
            tft.setTextDatum(MC_DATUM);
            // Position text just above where the bar top would be, or slightly higher if bar is very short
            int uv_text_y_pos = (bar_height > (value_text_height-2)) ? bar_top_y - value_text_height/2 -1 : graph_baseline_y - value_text_height/2 -1;
             if (bar_height == 0) uv_text_y_pos = graph_baseline_y - value_text_height/2 -1 -2; // If bar is 0 height, place text where bar would start slightly higher.
            tft.drawString(String(uvVal, 1), bar_center_x, uv_value_on_bar_y); // Using fixed Y for alignment
            
        } else {
            // If no data, placeholder for UV value text (or nothing)
            tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
            tft.setTextFont(1);
            tft.setTextDatum(MC_DATUM);
            tft.drawString("-", bar_center_x, uv_value_on_bar_y);
        }
    }
}