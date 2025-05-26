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
// const unsigned long UPDATE_INTERVAL_MS = 60 * 1000; // For faster testing (1 minute)
unsigned long lastUpdateTime = 0;
const int WIFI_CONNECTION_TIMEOUT_MS = 15000;

// --- Global Variables ---
TFT_eSPI tft = TFT_eSPI();
float currentUVIndex = -1.0;
String lastUpdateTimeStr = "Never";

const int HOURLY_FORECAST_COUNT = 6;
float hourlyUV[HOURLY_FORECAST_COUNT];
int forecastHours[HOURLY_FORECAST_COUNT]; // Stores the hour (0-23) for the forecast slot

// --- Function Declarations ---
void connectToWiFi();
bool fetchUVData();
void displayInfo();
void displayMessage(String msg_line1, String msg_line2 = "", int color = TFT_WHITE);
void initializeForecastData();

// --- Setup ---
void setup() {
  Serial.begin(115200);
  Serial.println("\nUV Index Monitor (Landscape + Forecast) Starting...");

  initializeForecastData(); // Initialize forecast arrays

  tft.init();
  // Set to landscape. TFT_WIDTH=135, TFT_HEIGHT=240 originally.
  // Rotation 1: USB on the left. tft.width()=240, tft.height()=135
  // Rotation 3: USB on the right. tft.width()=240, tft.height()=135
  tft.setRotation(1); // Or 3, depending on preferred landscape orientation
  
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM); // Default to Middle Center for messages

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
  delay(2000); // Update display every 2 seconds
}

// --- Function Definitions ---

void initializeForecastData() {
    for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
        hourlyUV[i] = -1.0; // Indicates no data / error
        forecastHours[i] = -1;
    }
}

void displayMessage(String msg_line1, String msg_line2, int color) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.setTextFont(2); // Font size 2 for messages
  // For landscape (240x135), adjust centering
  tft.drawString(msg_line1, tft.width() / 2, tft.height() / 2 - 10);
  if (msg_line2 != "") {
    tft.drawString(msg_line2, tft.width() / 2, tft.height() / 2 + 10);
  }
  Serial.println(msg_line1 + " " + msg_line2);
}

void connectToWiFi() {
    // (This function remains largely the same as your previous version using secrets.h)
    // Ensure NTP configuration is within the successful connection block.
    Serial.println("Connecting to WiFi using secrets.h values...");
    WiFi.mode(WIFI_STA);
    bool connected = false;
    String connected_ssid = "";

    // Try Network 1
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

    // Try Network 2
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
    // Add more network blocks if needed

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
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Error: WiFi not connected.");
    currentUVIndex = -1.0; initializeForecastData(); return false;
  }

  HTTPClient http;
  // Add "hourly=uv_index" to the API request
  String apiUrl = openMeteoUrl + "?latitude=" + String(MY_LATITUDE, 4) +
                  "&longitude=" + String(MY_LONGITUDE, 4) +
                  "&current=uv_index&hourly=uv_index&forecast_days=1"; // Get 1 day of forecast

  Serial.print("Fetching URL: "); Serial.println(apiUrl);
  displayMessage("Fetching UV...", "Hourly data...", TFT_CYAN);

  http.begin(apiUrl);
  int httpCode = http.GET();

  if (httpCode > 0) {
    String payload = http.getString();
    Serial.println("HTTP Code: " + String(httpCode));
    // Serial.println("Payload: " + payload); // Uncomment for full JSON debugging

    JsonDocument doc; // Adjust size if necessary, but defaults should be fine for this
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
      Serial.print(F("deserializeJson() failed: ")); Serial.println(error.c_str());
      currentUVIndex = -2.0; initializeForecastData(); http.end(); return false;
    }

    // Current UV Index
    if (doc.containsKey("current") && doc["current"].containsKey("uv_index")) {
      currentUVIndex = doc["current"]["uv_index"].as<float>();
      Serial.print("Current UV Index: "); Serial.println(currentUVIndex);
    } else {
      Serial.println("Current UV Index data not found.");
      currentUVIndex = -3.0; // Mark as error but continue for hourly if possible
    }
    
    // Hourly UV Index Forecast
    if (doc.containsKey("hourly") && doc["hourly"].containsKey("time") && doc["hourly"].containsKey("uv_index")) {
        JsonArray hourly_time_list = doc["hourly"]["time"].as<JsonArray>();
        JsonArray hourly_uv_list = doc["hourly"]["uv_index"].as<JsonArray>();

        struct tm timeinfo;
        if (!getLocalTime(&timeinfo, 5000)) {
            Serial.println("Failed to obtain local time for forecast matching.");
            initializeForecastData(); // Clear forecast on time error
        } else {
            int currentHourLocal = timeinfo.tm_hour; // Local current hour (0-23)
            Serial.printf("Current local hour for forecast matching: %02d\n", currentHourLocal);

            int startIndex = -1;
            for (int i = 0; i < hourly_time_list.size(); ++i) {
                String api_time_str = hourly_time_list[i].as<String>(); // e.g., "2024-05-26T14:00" (UTC by default from OpenMeteo)
                if (api_time_str.length() >= 13) {
                    int api_hour = api_time_str.substring(11, 13).toInt();
                    // Note: OpenMeteo hourly forecast 'time' is in UTC by default.
                    // We need to compare against UTC hour or ensure API returns local time (by adding timezone to API URL)
                    // For simplicity, let's assume `configTime` has set the ESP32's time correctly
                    // and we can find a match. A more robust solution would involve timezone conversion or asking API for local time.
                    // The `getLocalTime` gives us broken-down local time.
                    // If API provides UTC, we must compare `api_hour` (UTC) with `timeinfo.tm_hour` (Local) carefully.
                    // Quick fix: assume currentHourLocal is what we need to find in the API's list if the API times were also local.
                    // Better: Use timezone parameter in API: &timezone=auto

                    // Let's find the first hour in the forecast that is >= current local hour.
                    // This relies on the forecast_days=1 returning enough data past the current time.
                    if (api_hour >= currentHourLocal) {
                         // This simplistic match assumes API times are somewhat aligned with local day start.
                         // Or that the forecast list is long enough to find current local hour.
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
                        if (hourlyUV[i] < 0) hourlyUV[i] = 0; // Sanitize: UV can't be negative
                        String api_t_str = hourly_time_list[startIndex + i].as<String>();
                        forecastHours[i] = api_t_str.substring(11, 13).toInt();
                        Serial.printf("Forecast slot %d: Hour %02d, UV %.1f\n", i, forecastHours[i], hourlyUV[i]);
                    } else {
                        hourlyUV[i] = -1.0; // Not enough data in forecast
                        forecastHours[i] = -1;
                        Serial.printf("Forecast slot %d: Not enough data\n", i);
                    }
                }
            } else {
                Serial.println("Could not find a suitable starting index in hourly forecast data for current local time.");
                initializeForecastData(); // Clear previous forecast
            }
        }
    } else {
      Serial.println("Hourly UV data not found in JSON response.");
      initializeForecastData(); // Clear previous forecast
    }

    // Update last update time string
    struct tm timeinfo_update;
    if(getLocalTime(&timeinfo_update, 1000)){
      char timeStr[20];
      strftime(timeStr, sizeof(timeStr), "%I:%M %p", &timeinfo_update);
      lastUpdateTimeStr = String(timeStr);
    } else {
      // lastUpdateTimeStr might keep old value or "Time N/A"
    }

  } else {
    Serial.print("Error on HTTP request: "); Serial.println(HTTPClient::errorToString(httpCode).c_str());
    currentUVIndex = -4.0; initializeForecastData(); http.end(); return false;
  }

  http.end();
  return true;
}

void displayInfo() {
  tft.fillScreen(TFT_BLACK); // Clear screen

  // --- Top Row: WiFi Status & Last Update ---
  tft.setTextFont(2); // Font size 2
  tft.setTextDatum(TL_DATUM); // Top-Left datum
  if (WiFi.status() == WL_CONNECTED) {
    tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
    tft.drawString("WiFi: " + WiFi.SSID(), 5, 2);
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("WiFi: Disconnected", 5, 2);
  }
  
  tft.setTextDatum(TR_DATUM); // Top-Right datum
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("Last: " + lastUpdateTimeStr, tft.width() - 5, 2);

  // --- Middle Section: Current UV Index ---
  tft.setTextDatum(MC_DATUM); // Middle-Center datum
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextFont(2); // Smaller font for "CURRENT UV" label
  tft.drawString("CURRENT UV", tft.width() / 2, 25); // Y-pos adjusted for landscape

  if (currentUVIndex >= 0) {
    if (currentUVIndex < 3) tft.setTextColor(TFT_GREEN, TFT_BLACK);
    else if (currentUVIndex < 6) tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    else if (currentUVIndex < 8) tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    else if (currentUVIndex < 11) tft.setTextColor(TFT_RED, TFT_BLACK);
    else tft.setTextColor(TFT_VIOLET, TFT_BLACK);
    tft.setTextFont(7); // Large font for UV value
    tft.drawFloat(currentUVIndex, 1, tft.width() / 2, 50); // Y-pos adjusted
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setTextFont(2);
    String errorMsg = "UV Error"; // Simplified error for this spot
    if(currentUVIndex == -1.0) errorMsg = "No Conn";
    else if(currentUVIndex == -2.0) errorMsg = "JSON Err";
    else if(currentUVIndex == -3.0) errorMsg = "No Data";
    else if(currentUVIndex == -4.0) errorMsg = "HTTP Err";
    tft.drawString(errorMsg, tft.width()/2, 50);
  }

  // --- Bottom Section: Hourly Forecast Graph ---
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.drawString("UV Forecast (next 6h)", tft.width()/2, 80);

  int graphX = 15;      // Starting X for graph area
  int graphY = tft.height() - 45; // Starting Y for graph area (baseline of bars)
  int graphWidth = tft.width() - 30; // Width of graph area
  int graphBarWidth = graphWidth / (HOURLY_FORECAST_COUNT + 1); // Width of each bar + space
  int maxGraphBarHeight = 30; // Max height for a UV bar

  // Draw baseline for graph (optional)
  // tft.drawFastHLine(graphX, graphY + 1, graphWidth, TFT_DARKGREY);

  for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
    int barX = graphX + i * graphBarWidth + (graphBarWidth / 4);
    int currentBarWidth = graphBarWidth / 2;

    if (hourlyUV[i] >= 0 && forecastHours[i] != -1) {
      float uvVal = hourlyUV[i];
      if (uvVal > 12) uvVal = 12; // Cap UV for graph scaling (max typical UV is ~11-12)
      int barHeight = (int)( (uvVal / 12.0f) * maxGraphBarHeight );
      if (barHeight < 1 && uvVal > 0) barHeight = 1; // Min height for visible positive UV

      // Bar color based on UV
      if (uvVal < 3) tft.fillRect(barX, graphY - barHeight, currentBarWidth, barHeight, TFT_GREEN);
      else if (uvVal < 6) tft.fillRect(barX, graphY - barHeight, currentBarWidth, barHeight, TFT_YELLOW);
      else if (uvVal < 8) tft.fillRect(barX, graphY - barHeight, currentBarWidth, barHeight, TFT_ORANGE);
      else if (uvVal < 11) tft.fillRect(barX, graphY - barHeight, currentBarWidth, barHeight, TFT_RED);
      else tft.fillRect(barX, graphY - barHeight, currentBarWidth, barHeight, TFT_VIOLET);

      // Hour label
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setTextFont(1); // Smallest font for hour label
      tft.setTextDatum(MC_DATUM);
      String hourStr = String(forecastHours[i]); // Display as 0-23 hour
      // Convert to AM/PM if desired (more complex string formatting)
      // Example: if (forecastHours[i] == 0) hourStr = "12A"; else if (forecastHours[i] == 12) hourStr = "12P"; 
      //          else if (forecastHours[i] > 12) hourStr = String(forecastHours[i]-12) + "P"; 
      //          else hourStr = String(forecastHours[i]) + "A";
      tft.drawString(hourStr, barX + currentBarWidth / 2, graphY + 8); // Below bar
    } else {
      // No data for this slot, maybe draw a small marker or nothing
      tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
      tft.setTextFont(1);
      tft.setTextDatum(MC_DATUM);
      tft.drawString("-", barX + currentBarWidth/2, graphY + 8);
    }
  }
}