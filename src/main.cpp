#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h> // For parsing JSON

#include "secrets.h" // Include your secrets!

// --- Configuration (mostly from secrets.h now) ---
// Open-Meteo API endpoint
String openMeteoUrl = "https://api.open-meteo.com/v1/forecast";

// Update interval: 15 minutes (15 * 60 * 1000 milliseconds)
const unsigned long UPDATE_INTERVAL_MS = 15 * 60 * 1000;
// const unsigned long UPDATE_INTERVAL_MS = 30 * 1000; // For faster testing (30 seconds)
unsigned long lastUpdateTime = 0;
const int WIFI_CONNECTION_TIMEOUT_MS = 15000; // 15-second timeout for each WiFi attempt

// --- Global Variables ---
TFT_eSPI tft = TFT_eSPI();
float currentUVIndex = -1.0; // Variable to store UV index, -1.0 indicates no data yet
String lastUpdateTimeStr = "Never";

// --- Function Declarations ---
void connectToWiFi();
bool fetchUVData();
void displayInfo();
void displayMessage(String msg_line1, String msg_line2 = "", int color = TFT_WHITE);

// --- Setup ---
void setup() {
  Serial.begin(115200);
  Serial.println("\nUV Index Monitor Starting (with secrets.h)...");

  tft.init();
  tft.setRotation(0); // Adjust as needed (0 or 2 for portrait, 1 or 3 for landscape)
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);

  displayMessage("Connecting to WiFi...", "", TFT_YELLOW);
  connectToWiFi(); // This function now uses secrets.h and configures time

  if (WiFi.status() == WL_CONNECTED) {
    displayMessage("Fetching UV data...", "", TFT_CYAN);
    if (fetchUVData()) {
      lastUpdateTime = millis(); // Set initial update time
    }
  } else {
    // connectToWiFi already displays failure message
  }
  displayInfo();
}

// --- Main Loop ---
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    displayMessage("WiFi Disconnected!", "Reconnecting...", TFT_RED);
    delay(2000); // Show message for a bit
    connectToWiFi();
    if (WiFi.status() == WL_CONNECTED) {
        fetchUVData(); // Attempt to fetch data immediately after reconnect
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
  delay(1000); // Main loop delay
}

// --- Function Definitions ---

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

    // Try Network 1
    #if defined(WIFI_SSID_1) && defined(WIFI_PASS_1)
        if (strlen(WIFI_SSID_1) > 0) { // Check if SSID is not empty
            Serial.print("Attempting SSID: "); Serial.println(WIFI_SSID_1);
            displayMessage("Connecting to:", WIFI_SSID_1, TFT_YELLOW);
            WiFi.begin(WIFI_SSID_1, WIFI_PASS_1);
            unsigned long startTime = millis();
            while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_CONNECTION_TIMEOUT_MS) {
                delay(500); Serial.print(".");
            }
            if (WiFi.status() == WL_CONNECTED) {
                connected = true;
                connected_ssid = WIFI_SSID_1;
            } else {
                WiFi.disconnect(true); delay(100);
            }
        }
    #else
        // Serial.println("WIFI_SSID_1 or WIFI_PASS_1 not fully defined."); // Less verbose
    #endif

    // Try Network 2 if not yet connected
    #if defined(WIFI_SSID_2) && defined(WIFI_PASS_2)
        if (!connected && strlen(WIFI_SSID_2) > 0) { // Check if SSID is not empty
            Serial.print("\nAttempting SSID: "); Serial.println(WIFI_SSID_2);
            displayMessage("Connecting to:", WIFI_SSID_2, TFT_YELLOW);
            WiFi.begin(WIFI_SSID_2, WIFI_PASS_2);
            unsigned long startTime = millis();
            while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_CONNECTION_TIMEOUT_MS) {
                delay(500); Serial.print(".");
            }
            if (WiFi.status() == WL_CONNECTED) {
                connected = true;
                connected_ssid = WIFI_SSID_2;
            } else {
                WiFi.disconnect(true); delay(100);
            }
        }
    #endif

    // Add more blocks here for WIFI_SSID_3, WIFI_PASS_3 following the pattern above

    Serial.println();
    if (connected) {
        Serial.println("WiFi connected!");
        Serial.print("SSID: "); Serial.println(connected_ssid);
        Serial.print("IP address: "); Serial.println(WiFi.localIP());
        displayMessage("WiFi Connected!", connected_ssid, TFT_GREEN);
        delay(1500); // Show success message

        // Configure NTP time client using timezone info from secrets.h
        #if defined(MY_GMT_OFFSET_SEC) && defined(MY_DAYLIGHT_OFFSET_SEC)
            Serial.println("Configuring time via NTP using secrets.h timezone...");
            configTime(MY_GMT_OFFSET_SEC, MY_DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");
            struct tm timeinfo;
            if(!getLocalTime(&timeinfo, 5000)){ // Wait up to 5 seconds for time
                Serial.println("Failed to obtain time from NTP.");
                lastUpdateTimeStr = "Time N/A";
            } else {
                Serial.println("Time configured via NTP.");
                // Update lastUpdateTimeStr immediately if possible
                char timeStr[20];
                strftime(timeStr, sizeof(timeStr), "%I:%M %p", &timeinfo); // Shorter format: HH:MM AM/PM
                lastUpdateTimeStr = String(timeStr);
            }
        #else
            Serial.println("Timezone offsets not defined in secrets.h. Time might be UTC or incorrect.");
            lastUpdateTimeStr = "Time (UTC?)";
        #endif

    } else {
        Serial.println("Could not connect to any WiFi network defined in secrets.h.");
        displayMessage("WiFi Failed.", "Check secrets.h", TFT_RED);
        // No delay here, loop will handle next steps or retries
    }
}

bool fetchUVData() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Error: WiFi not connected. Cannot fetch UV data.");
    currentUVIndex = -1.0; // Error: No connection
    return false;
  }

  HTTPClient http;
  // Use MY_LATITUDE and MY_LONGITUDE from secrets.h
  String apiUrl = openMeteoUrl + "?latitude=" + String(MY_LATITUDE, 4) +
                  "&longitude=" + String(MY_LONGITUDE, 4) +
                  "&current=uv_index";

  Serial.print("Fetching URL: "); Serial.println(apiUrl);
  displayMessage("Fetching UV...", apiUrl.substring(0,25)+"...", TFT_CYAN); // Show partial URL

  http.begin(apiUrl);
  int httpCode = http.GET();

  if (httpCode > 0) {
    String payload = http.getString();
    Serial.println("HTTP Code: " + String(httpCode));

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
      Serial.print(F("deserializeJson() failed: ")); Serial.println(error.c_str());
      currentUVIndex = -2.0; // Error: JSON parsing
      http.end();
      return false;
    }

    if (doc.containsKey("current") && doc["current"].containsKey("uv_index")) {
      currentUVIndex = doc["current"]["uv_index"].as<float>();
      Serial.print("Current UV Index: "); Serial.println(currentUVIndex);

      struct tm timeinfo;
      if(getLocalTime(&timeinfo, 1000)){ // Quick check for time, don't wait long
        char timeStr[20];
        strftime(timeStr, sizeof(timeStr), "%I:%M %p", &timeinfo); // Format: HH:MM AM/PM
        lastUpdateTimeStr = String(timeStr);
      } else {
        // If getLocalTime fails here, it might mean NTP sync is still pending or failed.
        // lastUpdateTimeStr might remain "Time N/A" or "Time (UTC?)" from connectToWifi
        Serial.println("Could not get local time for update timestamp.");
      }
    } else {
      Serial.println("UV Index data not found in JSON response.");
      currentUVIndex = -3.0; // Error: Data not in JSON
      http.end();
      return false;
    }
  } else {
    Serial.print("Error on HTTP request: "); Serial.println(HTTPClient::errorToString(httpCode).c_str());
    currentUVIndex = -4.0; // Error: HTTP request failed
    http.end();
    return false;
  }

  http.end();
  return true;
}

void displayInfo() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM); // Top Left datum for status text
  tft.setTextFont(2);

  if (WiFi.status() == WL_CONNECTED) {
    tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
    tft.drawString("WiFi: " + WiFi.SSID(), 5, 5);
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("WiFi: Disconnected", 5, 5);
  }

  tft.setTextDatum(MC_DATUM); // Middle Center for main display
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextFont(4);
  tft.drawString("UV INDEX", tft.width() / 2, tft.height() / 2 - 50); // Adjusted y-pos

  if (currentUVIndex >= 0) {
    if (currentUVIndex < 3) tft.setTextColor(TFT_GREEN, TFT_BLACK);
    else if (currentUVIndex < 6) tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    else if (currentUVIndex < 8) tft.setTextColor(TFT_ORANGE, TFT_BLACK); // Using TFT_ORANGE
    else if (currentUVIndex < 11) tft.setTextColor(TFT_RED, TFT_BLACK);
    else tft.setTextColor(TFT_VIOLET, TFT_BLACK); // Using TFT_VIOLET (often same as MAGENTA)

    tft.setTextFont(7);
    tft.drawFloat(currentUVIndex, 1, tft.width() / 2, tft.height() / 2); // Display with 1 decimal
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setTextFont(2);
    String errorMsg = "Error: ";
    if(currentUVIndex == -1.0) errorMsg += "No Connection";
    else if(currentUVIndex == -2.0) errorMsg += "JSON Parse";
    else if(currentUVIndex == -3.0) errorMsg += "API Format";
    else if(currentUVIndex == -4.0) errorMsg += "HTTP Fail";
    else errorMsg += "Unknown";
    tft.drawString(errorMsg, tft.width() / 2, tft.height() / 2);
  }

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextFont(2);
  // Display "Last Update: HH:MM AM/PM" at the bottom center
  tft.setTextDatum(BC_DATUM); // Bottom Center datum
  tft.drawString("Last: " + lastUpdateTimeStr, tft.width() / 2, tft.height() - 5);
}