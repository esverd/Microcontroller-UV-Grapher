#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "secrets.h" // Your secrets

// --- Configuration ---
String openMeteoUrl = "https://api.open-meteo.com/v1/forecast";
const unsigned long UPDATE_INTERVAL_MS_NORMAL_MODE = 15 * 60 * 1000; // 15 minutes for normal mode
const int WIFI_CONNECTION_TIMEOUT_MS = 15000;
const unsigned long SCREEN_ON_DURATION_LPM_MS = 30 * 1000; // 30 second screen on time in LPM

// --- Debugging Flags ---
#define DEBUG_LPM 1             // Set to 1 to enable LPM specific logs
#define DEBUG_GRAPH_DRAWING 0   // Set to 1 to enable detailed graph drawing logs, 0 to disable
#define DEBUG_RTC 1             // Set to 1 to enable detailed RTC save/load logs

// --- Pins ---
#define BUTTON_INFO_PIN 0         // GPIO 0 for info overlay, location toggle, and primary wake from LPM
#define BUTTON_LP_TOGGLE_PIN 35   // GPIO 35 for toggling Low Power Mode
#define TFT_BL_PIN 4              // Backlight pin, defined in platformio.ini as -DTFT_BL=4

// --- Global Variables ---
TFT_eSPI tft = TFT_eSPI();
String lastUpdateTimeStr = "Never"; // Will be "HH:MM TZN" or "HH:MM"
float deviceLatitude = MY_LATITUDE;
float deviceLongitude = MY_LONGITUDE;
String locationDisplayStr = "Initializing...";
bool useGpsFromSecrets = false; // False = IP Geolocation (default), True = secrets.h

const int HOURLY_FORECAST_COUNT = 6;
float hourlyUV[HOURLY_FORECAST_COUNT];
int forecastHours[HOURLY_FORECAST_COUNT];

// Display State & Update Control
bool showInfoOverlay = false;
bool force_display_update = true;
bool dataJustFetched = false; 
unsigned long lastDataFetchAttemptMs = 0; 
bool isConnectingToWiFi = false; // Flag for "Connecting to WiFi..." message

// --- LPM State Variables ---
bool isLowPowerModeActive = false;    
unsigned long screenActiveUntilMs = 0; 
bool temporaryScreenWakeupActive = false; 

// --- Button Handling Variables ---
uint32_t button_info_last_press_time = 0;
bool button_info_last_state = HIGH;
uint32_t button_info_press_start_time = 0;
bool button_info_is_held = false;

uint32_t button_lp_last_press_time = 0;
bool button_lp_last_state = HIGH;
uint32_t button_lp_press_start_time = 0;
bool button_lp_is_held = false;

const uint16_t DEBOUNCE_TIME_MS = 50;
const uint16_t LONG_PRESS_TIME_MS = 1000; 

// --- RTC Memory Variables ---
RTC_DATA_ATTR uint32_t rtc_magic_cookie = 0;
RTC_DATA_ATTR bool rtc_isLowPowerModeActive = false;
RTC_DATA_ATTR bool rtc_hasValidData = false;
RTC_DATA_ATTR float rtc_hourlyUV[HOURLY_FORECAST_COUNT];
RTC_DATA_ATTR int rtc_forecastHours[HOURLY_FORECAST_COUNT];
RTC_DATA_ATTR char rtc_lastUpdateTimeStr_char[16]; 
RTC_DATA_ATTR char rtc_locationDisplayStr_char[32];
RTC_DATA_ATTR float rtc_deviceLatitude = MY_LATITUDE;
RTC_DATA_ATTR float rtc_deviceLongitude = MY_LONGITUDE;
RTC_DATA_ATTR bool rtc_useGpsFromSecretsGlobal = false;

#define RTC_MAGIC_VALUE 0xDEADBEEF

// --- Function Declarations ---
void turnScreenOn();
void turnScreenOff();
void saveStateToRTC();
void loadStateFromRTC();
uint64_t calculateSleepUntilNextHH05();
void enterDeepSleep(uint64_t duration_us, bool alsoEnableButtonWake);
void printWakeupReason();

void initializeForecastData(bool updateRTC = false);
void connectToWiFi(bool silent);
bool fetchLocationFromIp(bool silent);
bool fetchUVData(bool silent);

void displayMessage(String msg_line1, String msg_line2 = "", int color = TFT_WHITE, bool allowDisplay = true);
void displayInfo();
void drawForecastGraph(int start_y_offset);
void handle_buttons();

// --- Screen Control Functions ---
void turnScreenOn() {
    #if DEBUG_LPM
    Serial.println("Screen ON");
    #endif
    digitalWrite(TFT_BL_PIN, HIGH);
    tft.writecommand(TFT_DISPON);
}

void turnScreenOff() {
    #if DEBUG_LPM
    Serial.println("Screen OFF");
    #endif
    digitalWrite(TFT_BL_PIN, LOW);
    tft.writecommand(TFT_DISPOFF);
}

// --- RTC Memory Functions ---
void saveStateToRTC() {
    #if DEBUG_RTC
    Serial.printf("RTC SAVE: Attempting to save rtc_isLowPowerModeActive = %s\n", isLowPowerModeActive ? "true" : "false");
    #endif
    rtc_magic_cookie = RTC_MAGIC_VALUE;
    rtc_isLowPowerModeActive = isLowPowerModeActive; // This is the critical flag for LPM state
    rtc_useGpsFromSecretsGlobal = useGpsFromSecrets;

    if (rtc_hasValidData) { 
        for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
            rtc_hourlyUV[i] = hourlyUV[i]; 
            rtc_forecastHours[i] = forecastHours[i];
        }
        strncpy(rtc_lastUpdateTimeStr_char, lastUpdateTimeStr.c_str(), sizeof(rtc_lastUpdateTimeStr_char) - 1);
        rtc_lastUpdateTimeStr_char[sizeof(rtc_lastUpdateTimeStr_char) - 1] = '\0';
        strncpy(rtc_locationDisplayStr_char, locationDisplayStr.c_str(), sizeof(rtc_locationDisplayStr_char) - 1);
        rtc_locationDisplayStr_char[sizeof(rtc_locationDisplayStr_char) - 1] = '\0';
        rtc_deviceLatitude = deviceLatitude;
        rtc_deviceLongitude = deviceLongitude;
    }
    #if DEBUG_RTC
    Serial.printf("RTC SAVE COMPLETE: rtc_isLowPowerModeActive is now %s in RTC memory structure.\n", rtc_isLowPowerModeActive ? "true" : "false");
    Serial.printf("RTC Save: HasValidData: %s, UseGPSSecrets: %s\n", rtc_hasValidData ? "Yes" : "No", rtc_useGpsFromSecretsGlobal ? "Yes" : "No");
    #endif
}

void loadStateFromRTC() {
    #if DEBUG_RTC
    Serial.println("RTC LOAD: Attempting to load state from RTC...");
    #endif
    if (rtc_magic_cookie == RTC_MAGIC_VALUE) {
        isLowPowerModeActive = rtc_isLowPowerModeActive; // Load the persisted LPM state
        useGpsFromSecrets = rtc_useGpsFromSecretsGlobal;
        #if DEBUG_RTC
        Serial.printf("RTC LOAD: Loaded rtc_isLowPowerModeActive = %s into working isLowPowerModeActive.\n", isLowPowerModeActive ? "true" : "false");
        #endif
        if (rtc_hasValidData) { 
            for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
                hourlyUV[i] = rtc_hourlyUV[i];
                forecastHours[i] = rtc_forecastHours[i];
            }
            lastUpdateTimeStr = String(rtc_lastUpdateTimeStr_char);
            locationDisplayStr = String(rtc_locationDisplayStr_char);
            deviceLatitude = rtc_deviceLatitude;
            deviceLongitude = rtc_deviceLongitude;
            dataJustFetched = true; 
            #if DEBUG_LPM
            Serial.println("Valid data loaded from RTC.");
            #endif
        } else {
            initializeForecastData(); 
            #if DEBUG_LPM
            Serial.println("RTC data marked as not valid, initialized working data to defaults.");
            #endif
        }
    } else { 
        #if DEBUG_RTC
        Serial.println("RTC LOAD: Magic cookie mismatch or first boot. Initializing RTC data to defaults.");
        #endif
        isLowPowerModeActive = false; // Default to Normal Mode on first boot/corruption
        rtc_isLowPowerModeActive = false;
        useGpsFromSecrets = false; 
        rtc_useGpsFromSecretsGlobal = false;
        initializeForecastData(true); 
        rtc_hasValidData = false;     
        strncpy(rtc_lastUpdateTimeStr_char, "Never", sizeof(rtc_lastUpdateTimeStr_char) -1);
        rtc_lastUpdateTimeStr_char[sizeof(rtc_lastUpdateTimeStr_char)-1] = '\0';
        lastUpdateTimeStr = "Never";
        strncpy(rtc_locationDisplayStr_char, "Initializing...", sizeof(rtc_locationDisplayStr_char)-1);
        rtc_locationDisplayStr_char[sizeof(rtc_locationDisplayStr_char)-1] = '\0';
        locationDisplayStr = "Initializing...";
        rtc_deviceLatitude = MY_LATITUDE; 
        rtc_deviceLongitude = MY_LONGITUDE;
        deviceLatitude = MY_LATITUDE;
        deviceLongitude = MY_LONGITUDE;
        saveStateToRTC(); // Save this initial default state
    }
    #if DEBUG_LPM
    Serial.printf("Loaded from RTC: LPM Active: %s, UseGPSSecrets: %s, RTCValidData: %s\n",
                  isLowPowerModeActive ? "Yes" : "No", 
                  useGpsFromSecrets ? "Yes" : "No",
                  rtc_hasValidData ? "Yes" : "No");
    #endif
}

// --- Deep Sleep Functions ---
uint64_t calculateSleepUntilNextHH05() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 5000)) {
        Serial.println("Failed to obtain time for sleep calculation. Sleeping for 1 hour as fallback.");
        return 60 * 60 * 1000000ULL;
    }
    #if DEBUG_LPM
    char timeBuff[30];
    strftime(timeBuff, sizeof(timeBuff), "%A, %B %d %Y %H:%M:%S", &timeinfo);
    Serial.printf("Current time for sleep calc: %s\n", timeBuff);
    #endif
    time_t now = mktime(&timeinfo);
    struct tm targetTimeInfo = timeinfo;
    targetTimeInfo.tm_min = 5;
    targetTimeInfo.tm_sec = 0;
    if (mktime(&targetTimeInfo) <= now) { 
        targetTimeInfo.tm_hour += 1; 
    }
    time_t targetTimeEpoch = mktime(&targetTimeInfo);
    uint64_t sleepDurationS = difftime(targetTimeEpoch, now);
    if (sleepDurationS <= 0 || sleepDurationS > (2 * 60 * 60)) { 
        Serial.printf("Unusual sleep duration calculated (%llu s). Defaulting to 1 hour.\n", sleepDurationS);
        return 60 * 60 * 1000000ULL;
    }
    #if DEBUG_LPM
    Serial.printf("Sleep duration: %llu seconds until next HH:05.\n", sleepDurationS);
    #endif
    return sleepDurationS * 1000000ULL;
}

void enterDeepSleep(uint64_t duration_us, bool alsoEnableButtonWake) {
    saveStateToRTC(); // Crucial: Save state (including LPM active flag) before sleeping
    turnScreenOff();
    Serial.printf("Entering deep sleep for %llu us (approx %.2f minutes).\n", duration_us, (double)duration_us / 1000000.0 / 60.0);
    esp_sleep_enable_timer_wakeup(duration_us);
    if (alsoEnableButtonWake) {
        Serial.println("Enabling GPIO0 (BUTTON_INFO_PIN) for wake-up from deep sleep (falling edge).");
        esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    }
    Serial.flush();
    esp_deep_sleep_start();
}

void printWakeupReason() {
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    Serial.print("Wakeup caused by: ");
    switch (wakeup_reason) {
        case ESP_SLEEP_WAKEUP_EXT0: Serial.println("External signal using RTC_IO (BUTTON_INFO_PIN)"); break;
        case ESP_SLEEP_WAKEUP_TIMER: Serial.println("Timer"); break;
        default: Serial.printf("Other event: %d (Possibly power-on reset)\n", wakeup_reason); break;
    }
}

void initializeForecastData(bool updateRTC) {
    Serial.println("Initializing forecast data to defaults.");
    for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
        hourlyUV[i] = -1.0f;
        forecastHours[i] = -1;
        if (updateRTC) {
            rtc_hourlyUV[i] = -1.0f;
            rtc_forecastHours[i] = -1;
        }
    }
}

// --- Setup ---
void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 2000);
    Serial.println("\nUV Index Monitor Starting Up...");

    pinMode(BUTTON_INFO_PIN, INPUT_PULLUP);
    pinMode(BUTTON_LP_TOGGLE_PIN, INPUT_PULLUP);
    pinMode(TFT_BL_PIN, OUTPUT);

    printWakeupReason();
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    
    loadStateFromRTC(); // Load persisted state FIRST, this sets isLowPowerModeActive
    #if DEBUG_RTC
    Serial.printf("SETUP: After loadStateFromRTC(), isLowPowerModeActive = %s\n", isLowPowerModeActive ? "true" : "false");
    #endif

    tft.init();
    tft.setRotation(1);
    tft.setTextDatum(MC_DATUM);

    if (isLowPowerModeActive) { // Check the loaded state
        if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
            #if DEBUG_LPM
            Serial.println("LPM: Timer Wake-up. Silent refresh cycle.");
            #endif
            temporaryScreenWakeupActive = false;
            turnScreenOff(); 
            connectToWiFi(true); 
            if (WiFi.status() == WL_CONNECTED) {
                #if DEBUG_LPM
                Serial.printf("LPM Silent: Using Lat: %.4f, Lon: %.4f (Source: %s)\n",
                              deviceLatitude, deviceLongitude, useGpsFromSecrets ? "Secrets" : "IP Geo (RTC)");
                #endif
                fetchUVData(true); 
            } else {
                #if DEBUG_LPM
                Serial.println("LPM Silent: No WiFi, cannot fetch UV data.");
                #endif
            }
            enterDeepSleep(calculateSleepUntilNextHH05(), true);
        } else if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) { 
            #if DEBUG_LPM
            Serial.println("LPM: Button Wake-up (GPIO 0). Temporary screen on. No automatic data refresh.");
            #endif
            temporaryScreenWakeupActive = true;
            screenActiveUntilMs = millis() + SCREEN_ON_DURATION_LPM_MS;
            turnScreenOn();
            tft.fillScreen(TFT_BLACK);
            if (rtc_hasValidData) { 
                force_display_update = true;
            } else {
                displayMessage("LPM: No data yet", "Hourly update pending", TFT_YELLOW, true);
            }
        } else { // Power-on reset or other wake reason while rtc_isLowPowerModeActive was true
            #if DEBUG_LPM
            Serial.println("LPM: Non-timer/button wake (e.g. Power-On/Reset) while RTC indicated LPM was active. Forcing LPM cycle.");
            #endif
            temporaryScreenWakeupActive = false; 
            turnScreenOn(); 
            displayMessage("LPM Active", "Initializing...", TFT_BLUE, true);
            delay(1500);
            enterDeepSleep(calculateSleepUntilNextHH05(), true); 
        }
    } else { // Normal Mode (isLowPowerModeActive is false)
        #if DEBUG_LPM
        Serial.println("Normal Mode Startup.");
        #endif
        temporaryScreenWakeupActive = false;
        turnScreenOn();
        tft.fillScreen(TFT_BLACK);

        if (rtc_hasValidData && wakeup_reason != ESP_SLEEP_WAKEUP_UNDEFINED) { 
             #if DEBUG_LPM
            Serial.println("Normal Mode: Resuming with RTC data available.");
            #endif
            force_display_update = true; 
        }
        
        displayMessage("Connecting to WiFi...", "", TFT_YELLOW, true);
        connectToWiFi(false); 

        if (WiFi.status() == WL_CONNECTED) {
            if (useGpsFromSecrets) { 
                deviceLatitude = MY_LATITUDE;
                deviceLongitude = MY_LONGITUDE;
                locationDisplayStr = "Secrets GPS";
                Serial.println("Normal Mode: Using GPS from secrets.h");
            } else { 
                displayMessage("Fetching IP Location...", "", TFT_SKYBLUE, true);
                if (fetchLocationFromIp(false)) { 
                    Serial.println("Normal Mode: IP Geolocation successful.");
                } else {
                    Serial.println("Normal Mode: IP Geolocation failed. Using GPS from secrets.h as fallback.");
                    useGpsFromSecrets = true; 
                    deviceLatitude = MY_LATITUDE;
                    deviceLongitude = MY_LONGITUDE;
                    locationDisplayStr = "IP Fail>Secrets";
                }
            }
            String currentStatusForDisplay = locationDisplayStr;
            if (currentStatusForDisplay.length() > 18) currentStatusForDisplay = currentStatusForDisplay.substring(0,15) + "...";
            displayMessage("Fetching UV data...", currentStatusForDisplay, TFT_CYAN, true);
            if (fetchUVData(false)) { 
                lastDataFetchAttemptMs = millis(); 
            }
        } else { 
            locationDisplayStr = "Offline>Secrets"; 
            useGpsFromSecrets = true; 
            deviceLatitude = MY_LATITUDE;
            deviceLongitude = MY_LONGITUDE;
            initializeForecastData(); 
            lastUpdateTimeStr = "Offline";
        }
        force_display_update = true; 
    }
}

// --- Main Loop ---
void loop() {
    handle_buttons(); 

    if (isLowPowerModeActive) {
        if (temporaryScreenWakeupActive) {
            if (millis() >= screenActiveUntilMs) {
                #if DEBUG_LPM
                Serial.println("LPM: Screen on-time expired. Going back to deep sleep.");
                #endif
                temporaryScreenWakeupActive = false;
                enterDeepSleep(calculateSleepUntilNextHH05(), true);
            }
        } else { 
            #if DEBUG_LPM
            Serial.println("LPM: Active, screen off. Fallback to deep sleep from loop.");
            #endif
            enterDeepSleep(calculateSleepUntilNextHH05(), true);
        }
    } else { 
        unsigned long currentMillis = millis();
        if ((WiFi.status() == WL_CONNECTED) && (currentMillis - lastDataFetchAttemptMs >= UPDATE_INTERVAL_MS_NORMAL_MODE)) {
            Serial.println("Normal Mode: Update interval reached.");
            String tempLocStr = locationDisplayStr;
            if (tempLocStr.length() > 18) tempLocStr = tempLocStr.substring(0,15) + "...";
            displayMessage("Updating UV data...", tempLocStr, TFT_CYAN, true); 
            if (fetchUVData(false)) { 
                lastDataFetchAttemptMs = currentMillis; 
            } else {
                Serial.println("Normal Mode: UV Data fetch failed for periodic update.");
            }
        } else if (WiFi.status() != WL_CONNECTED && (currentMillis - lastDataFetchAttemptMs >= 60000)) { 
             Serial.println("Normal Mode: No WiFi. Attempting reconnect...");
             connectToWiFi(false); 
             if(WiFi.status() == WL_CONNECTED) { 
                lastDataFetchAttemptMs = 0; 
             } else {
                lastDataFetchAttemptMs = currentMillis; 
                lastUpdateTimeStr = "Offline";
                initializeForecastData(); 
                dataJustFetched = true; 
             }
        }
    }

    if (force_display_update || dataJustFetched) {
        if (!isLowPowerModeActive || temporaryScreenWakeupActive) { 
            displayInfo();
        }
        force_display_update = false;
        dataJustFetched = false; 
    }
    delay(50); 
}

// --- Consolidated Button Handling ---
void handle_buttons() {
    unsigned long current_millis = millis();

    // BUTTON_INFO_PIN (GPIO 0)
    bool current_info_button_state = digitalRead(BUTTON_INFO_PIN);
    if (current_info_button_state == LOW && button_info_last_state == HIGH && (current_millis - button_info_last_press_time > DEBOUNCE_TIME_MS)) {
        button_info_press_start_time = current_millis;
        button_info_is_held = false;
    } else if (current_info_button_state == LOW && !button_info_is_held) { 
        if (current_millis - button_info_press_start_time > LONG_PRESS_TIME_MS) { 
            if (showInfoOverlay) { 
                useGpsFromSecrets = !useGpsFromSecrets;
                rtc_useGpsFromSecretsGlobal = useGpsFromSecrets; 
                Serial.printf("Location Mode Toggled (Long Press): %s\n", useGpsFromSecrets ? "Secrets GPS" : "IP Geolocation");

                bool connectionAttempted = false;
                if (WiFi.status() != WL_CONNECTED) {
                    Serial.println("Location toggle: WiFi disconnected, attempting connect...");
                    displayMessage("Connecting to WiFi...", "", TFT_YELLOW, true); // Show connecting message
                    connectToWiFi(false); // Attempt non-silent connect
                    connectionAttempted = true;
                }

                if (useGpsFromSecrets) { // Switched to Secrets GPS
                    deviceLatitude = MY_LATITUDE; deviceLongitude = MY_LONGITUDE;
                    locationDisplayStr = "Secrets GPS";
                    // Proceed to fetch UV data if WiFi is connected
                } else { // Switched to IP Geolocation
                    locationDisplayStr = "IP Geo..."; 
                    force_display_update = true; displayInfo(); // Update display to show "IP Geo..."
                    
                    if (WiFi.status() == WL_CONNECTED) {
                        if (!fetchLocationFromIp(false)) { 
                            locationDisplayStr = "IP Fail>Secrets"; 
                            useGpsFromSecrets = true; rtc_useGpsFromSecretsGlobal = true; 
                            deviceLatitude = MY_LATITUDE; deviceLongitude = MY_LONGITUDE;
                        }
                    } else { // WiFi still not connected after attempt (or was already off and attempt failed)
                         locationDisplayStr = "IP (NoNet)";
                         useGpsFromSecrets = true; rtc_useGpsFromSecretsGlobal = true; 
                         deviceLatitude = MY_LATITUDE; deviceLongitude = MY_LONGITUDE;
                         locationDisplayStr = "IP Fail>Secrets"; // Fallback display
                         Serial.println("Location toggle: WiFi connection failed for IP Geo. Reverted to Secrets.");
                    }
                }
                
                if (WiFi.status() == WL_CONNECTED) {
                    displayMessage("Fetching UV data...", locationDisplayStr.substring(0,18), TFT_CYAN, true);
                    if (fetchUVData(false)) { 
                        if (!isLowPowerModeActive) lastDataFetchAttemptMs = current_millis; 
                    }
                } else { 
                     Serial.println("Location toggle: WiFi still disconnected. Cannot fetch UV data.");
                     lastUpdateTimeStr = "Offline"; 
                     initializeForecastData(); 
                }

                if (isLowPowerModeActive && temporaryScreenWakeupActive) {
                    screenActiveUntilMs = current_millis + SCREEN_ON_DURATION_LPM_MS; 
                }
                force_display_update = true; 
            }
            button_info_is_held = true; button_info_last_press_time = current_millis;
        }
    } else if (current_info_button_state == HIGH && button_info_last_state == LOW && (current_millis - button_info_last_press_time > DEBOUNCE_TIME_MS)) { 
        if (!button_info_is_held) { 
            showInfoOverlay = !showInfoOverlay;
            Serial.printf("Info Button Short Press, showInfoOverlay: %s\n", showInfoOverlay ? "true" : "false");
            if (isLowPowerModeActive && temporaryScreenWakeupActive) {
                screenActiveUntilMs = current_millis + SCREEN_ON_DURATION_LPM_MS; 
            }
            force_display_update = true;
        }
        button_info_last_press_time = current_millis; button_info_is_held = false;
    }
    button_info_last_state = current_info_button_state;

    // BUTTON_LP_TOGGLE_PIN (GPIO 35)
    bool current_lp_button_state = digitalRead(BUTTON_LP_TOGGLE_PIN);
    if (current_lp_button_state == LOW && button_lp_last_state == HIGH && (current_millis - button_lp_last_press_time > DEBOUNCE_TIME_MS)) {
        button_lp_press_start_time = current_millis; button_lp_is_held = false;
    } else if (current_lp_button_state == LOW && !button_lp_is_held) { 
        if (current_millis - button_lp_press_start_time > LONG_PRESS_TIME_MS) { 
            isLowPowerModeActive = !isLowPowerModeActive; 
            // rtc_isLowPowerModeActive will be updated in saveStateToRTC() before sleep or immediately if turning off
            Serial.printf("LPM Toggled (Long Press): %s\n", isLowPowerModeActive ? "ON" : "OFF");
            
            if (isLowPowerModeActive) { // Turning LPM ON
                temporaryScreenWakeupActive = false; 
                rtc_isLowPowerModeActive = true; // Update RTC variable immediately
                saveStateToRTC(); // Save state before showing message and sleeping
                turnScreenOn(); 
                displayMessage("Low Power Mode: ON", "Sleeping...", TFT_BLUE, true); delay(2000);
                enterDeepSleep(calculateSleepUntilNextHH05(), true); 
            } else { // Turning LPM OFF
                temporaryScreenWakeupActive = false; 
                rtc_isLowPowerModeActive = false; // Update RTC variable immediately
                saveStateToRTC(); // Save the new "Normal Mode" state
                turnScreenOn(); 
                displayMessage("Low Power Mode: OFF", "", TFT_GREEN, true); 
                delay(1500); 

                Serial.println("Exiting LPM: Attempting WiFi connection and data refresh...");
                connectToWiFi(false); 
                if (WiFi.status() == WL_CONNECTED) {
                    displayMessage("Fetching data...", "", TFT_CYAN, true); 
                    if (fetchUVData(false)) { 
                        Serial.println("Data refreshed after exiting LPM.");
                    } else {
                        Serial.println("Failed to refresh data after exiting LPM.");
                    }
                } else {
                    Serial.println("No WiFi after exiting LPM. Data not refreshed.");
                    lastUpdateTimeStr = "Offline"; 
                    initializeForecastData();    
                }
                
                lastDataFetchAttemptMs = millis(); 
                force_display_update = true; 
            }
            button_lp_is_held = true; button_lp_last_press_time = current_millis;
        }
    } else if (current_lp_button_state == HIGH && button_lp_last_state == LOW && (current_millis - button_lp_last_press_time > DEBOUNCE_TIME_MS)) { 
        button_lp_last_press_time = current_millis; button_lp_is_held = false;
    }
    button_lp_last_state = current_lp_button_state;
}

// --- Display Functions ---
void displayMessage(String msg_line1, String msg_line2, int color, bool allowDisplay) {
    if (!allowDisplay && !(isLowPowerModeActive && temporaryScreenWakeupActive)) {
        Serial.println("DisplayMessage Skipped: " + msg_line1 + " " + msg_line2);
        return;
    }
    tft.fillScreen(TFT_BLACK); tft.setTextColor(color, TFT_BLACK); tft.setTextFont(2); tft.setTextDatum(MC_DATUM);
    int16_t width = tft.width(); int16_t height = tft.height();
    tft.drawString(msg_line1, width / 2, height / 2 - (msg_line2 != "" ? 10 : 0));
    if (msg_line2 != "") tft.drawString(msg_line2, width / 2, height / 2 + 10);
    Serial.println("Displaying Message: " + msg_line1 + " " + msg_line2);
}

void displayInfo() {
    if (isLowPowerModeActive && !temporaryScreenWakeupActive) {
        #if DEBUG_LPM
        Serial.println("displayInfo call skipped: LPM active and screen not temporarily on.");
        #endif
        return;
    }
    tft.fillScreen(TFT_BLACK); int padding = 4; int top_y_offset = padding;
    tft.setTextFont(2); int info_font_height = tft.fontHeight(2); int base_top_text_line_y = padding + info_font_height / 2;

    if (showInfoOverlay) {
        int current_info_y = base_top_text_line_y; tft.setTextDatum(TL_DATUM);
        tft.setTextColor(isLowPowerModeActive ? TFT_ORANGE : TFT_GREEN, TFT_BLACK);
        tft.drawString(isLowPowerModeActive ? "LPM: ON" : "LPM: OFF", padding, current_info_y);
        current_info_y += (info_font_height + padding);

        if (WiFi.status() == WL_CONNECTED) {
            tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK); String ssid_str = WiFi.SSID();
            if (ssid_str.length() > 16) ssid_str = ssid_str.substring(0, 13) + "...";
            tft.drawString("WiFi: " + ssid_str, padding, current_info_y);
        } else if (isConnectingToWiFi) { // New condition for "Connecting..."
             tft.setTextColor(TFT_YELLOW, TFT_BLACK);
             tft.drawString("WiFi: Connecting...", padding, current_info_y);
        }
         else { // WiFi not connected and not actively trying
            tft.setTextColor(TFT_RED, TFT_BLACK); tft.drawString("WiFi: Offline", padding, current_info_y);
        }
        tft.setTextDatum(TR_DATUM); tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        String timeToDisplay = lastUpdateTimeStr;
        if (timeToDisplay.length() > 12) timeToDisplay = timeToDisplay.substring(0,9) + "...";
        tft.drawString("Upd: " + timeToDisplay, tft.width() - padding, current_info_y);
        current_info_y += (info_font_height + padding);

        tft.setTextDatum(TL_DATUM); tft.setTextColor(TFT_SKYBLUE, TFT_BLACK);
        String locText = "Loc: " + locationDisplayStr + (useGpsFromSecrets ? " (Sec)" : " (IP)");
        if (locText.length() > 30) locText = locText.substring(0, 27) + "...";
        tft.drawString(locText, padding, current_info_y);
        top_y_offset = current_info_y + info_font_height / 2 + padding * 2;
    } else { 
        if (WiFi.status() != WL_CONNECTED && !isConnectingToWiFi) { // Not connected and not trying
            tft.setTextDatum(MC_DATUM); tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.drawString("! No WiFi Connection !", tft.width() / 2, base_top_text_line_y);
            top_y_offset = base_top_text_line_y + info_font_height / 2 + padding * 2;
        } else if (isConnectingToWiFi) { // Actively trying to connect
             tft.setTextDatum(MC_DATUM); tft.setTextColor(TFT_YELLOW, TFT_BLACK);
             tft.drawString("Connecting to WiFi...", tft.width()/2, base_top_text_line_y);
             top_y_offset = base_top_text_line_y + info_font_height / 2 + padding * 2;
        }

        if (isLowPowerModeActive && temporaryScreenWakeupActive) {
            tft.setTextDatum(TR_DATUM); tft.setTextColor(TFT_ORANGE, TFT_BLACK);
            tft.drawString("LPM", tft.width() - padding, base_top_text_line_y);
        }
    }
    drawForecastGraph(top_y_offset);
}

void drawForecastGraph(int start_y_offset) {
    int padding = 2; int first_uv_val_font = 6; int other_uv_val_font = 4; int hour_label_font = 2;
    tft.setTextFont(first_uv_val_font); int first_uv_text_h = tft.fontHeight();
    tft.setTextFont(other_uv_val_font); int other_uv_text_h = tft.fontHeight();
    tft.setTextFont(hour_label_font);   int hour_label_text_h = tft.fontHeight();
    int first_uv_value_y = start_y_offset + padding + first_uv_text_h / 2;
    int other_uv_values_line_y = first_uv_value_y + (first_uv_text_h / 2) - (other_uv_text_h / 2);
    if (HOURLY_FORECAST_COUNT <= 1) other_uv_values_line_y = first_uv_value_y;
    int hour_label_y = tft.height() - padding - hour_label_text_h / 2;
    int graph_baseline_y = hour_label_y - hour_label_text_h / 2 - padding;
    int max_bar_pixel_height = graph_baseline_y - (start_y_offset + padding);
    if (max_bar_pixel_height < 10) max_bar_pixel_height = 10;
    if (max_bar_pixel_height < 20 && tft.height() > 100) max_bar_pixel_height = 20;
    const float MAX_UV_FOR_FULL_SCALE = 10.0f; float pixel_chunk_per_uv_unit = 0;
    if (MAX_UV_FOR_FULL_SCALE > 0) pixel_chunk_per_uv_unit = max_bar_pixel_height / MAX_UV_FOR_FULL_SCALE;
    int graph_area_total_width = tft.width() - 2 * padding; int bar_slot_width = graph_area_total_width / HOURLY_FORECAST_COUNT;
    int bar_actual_width = bar_slot_width * 0.75; if (bar_actual_width < 4) bar_actual_width = 4; if (bar_actual_width > 30) bar_actual_width = 30;
    int graph_area_x_start = (tft.width() - (bar_slot_width * HOURLY_FORECAST_COUNT)) / 2 + padding;

    for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
        int bar_center_x = graph_area_x_start + (i * bar_slot_width) + (bar_slot_width / 2);
        tft.setTextFont(hour_label_font); tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.setTextDatum(MC_DATUM);
        if (forecastHours[i] != -1) tft.drawString(String(forecastHours[i]), bar_center_x, hour_label_y);
        else tft.drawString("-", bar_center_x, hour_label_y);
        float uvVal = hourlyUV[i]; int roundedUV = (uvVal >= -0.01f) ? round(uvVal) : -1;

        if (roundedUV != -1 && forecastHours[i] != -1) {
            float uv_for_height_calc = (float)roundedUV; if (uv_for_height_calc < 0) uv_for_height_calc = 0;
            if (uv_for_height_calc > MAX_UV_FOR_FULL_SCALE) uv_for_height_calc = MAX_UV_FOR_FULL_SCALE;
            int bar_height = round(uv_for_height_calc * pixel_chunk_per_uv_unit);
            if (uvVal > 0 && uvVal < 0.5f && bar_height == 0) bar_height = 1;
            else if (roundedUV >= 1 && bar_height == 0 && pixel_chunk_per_uv_unit > 0) bar_height = 1;
            else if (roundedUV >= 1 && bar_height < 2 && pixel_chunk_per_uv_unit > 2) bar_height = 2;
            if (bar_height > max_bar_pixel_height) bar_height = max_bar_pixel_height; if (bar_height < 0) bar_height = 0;
            int bar_top_y = graph_baseline_y - bar_height; uint16_t barColor;
            if (roundedUV < 1) barColor = TFT_DARKGREY; else if (roundedUV <= 3) barColor = TFT_GREEN;
            else if (roundedUV <= 5) barColor = TFT_YELLOW; else if (roundedUV <= 7) barColor = 0xFC60; 
            else barColor = TFT_RED;
            if (bar_height > 0) tft.fillRect(bar_center_x - bar_actual_width / 2, bar_top_y, bar_actual_width, bar_height, barColor);

            tft.setTextDatum(MC_DATUM); String uvText = String(roundedUV); int current_uv_text_y;
            uint16_t outlineColor = TFT_BLACK; uint16_t foregroundColor = TFT_WHITE;
            if (i == 0) { tft.setTextFont(first_uv_val_font); current_uv_text_y = first_uv_value_y; }
            else { tft.setTextFont(other_uv_val_font); current_uv_text_y = other_uv_values_line_y; }
            tft.setTextColor(outlineColor); 
            tft.drawString(uvText, bar_center_x - 1, current_uv_text_y -1); tft.drawString(uvText, bar_center_x + 1, current_uv_text_y -1);
            tft.drawString(uvText, bar_center_x - 1, current_uv_text_y +1); tft.drawString(uvText, bar_center_x + 1, current_uv_text_y +1);
            tft.drawString(uvText, bar_center_x - 1, current_uv_text_y);    tft.drawString(uvText, bar_center_x + 1, current_uv_text_y);
            tft.drawString(uvText, bar_center_x, current_uv_text_y - 1);    tft.drawString(uvText, bar_center_x, current_uv_text_y + 1);
            tft.setTextColor(foregroundColor, TFT_BLACK); tft.drawString(uvText, bar_center_x, current_uv_text_y);
        } else { 
            int placeholder_y; int placeholder_font;
            if (i == 0) { placeholder_font = first_uv_val_font; placeholder_y = first_uv_value_y; }
            else { placeholder_font = other_uv_val_font; placeholder_y = other_uv_values_line_y; }
            tft.setTextFont(placeholder_font); tft.setTextColor(TFT_DARKGREY, TFT_BLACK); tft.setTextDatum(MC_DATUM);
            tft.drawString("-", bar_center_x, placeholder_y);
        }
    }
    #if DEBUG_GRAPH_DRAWING
    Serial.println("--- End of Graph Draw Cycle ---");
    #endif
}

// --- Network and Data Fetching Functions ---
void connectToWiFi(bool silent) {
    isConnectingToWiFi = true; // Set flag at the beginning
    if (!silent) {
        Serial.println("Connecting to WiFi using secrets.h values...");
        // displayMessage("Connecting to WiFi...", "", TFT_YELLOW, true); // This will be handled by displayInfo now
        force_display_update = true; // Trigger displayInfo to show "Connecting..."
    }
    #if DEBUG_LPM
    else Serial.println("LPM Silent: Connecting to WiFi...");
    #endif

    WiFi.mode(WIFI_STA); WiFi.disconnect(true,true); delay(100);
    bool connected = false; String connected_ssid = "";

    #if defined(WIFI_SSID_1) && defined(WIFI_PASS_1)
      if (strlen(WIFI_SSID_1) > 0 && strlen(WIFI_PASS_1) > 0) {
        if (!silent) {Serial.print("Attempting SSID: "); Serial.println(WIFI_SSID_1);}
        WiFi.begin(WIFI_SSID_1, WIFI_PASS_1); unsigned long startTime = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_CONNECTION_TIMEOUT_MS) {
          delay(100); if (!silent && (millis() - startTime) % 1000 < 100) Serial.print(".");
        }
        if (WiFi.status() == WL_CONNECTED) {connected = true; connected_ssid = WIFI_SSID_1;}
        else {if (!silent) Serial.println("\nFailed to connect to SSID 1."); WiFi.disconnect(true,true); delay(100);}
      }
    #endif
    #if defined(WIFI_SSID_2) && defined(WIFI_PASS_2)
      if (!connected && strlen(WIFI_SSID_2) > 0 && strlen(WIFI_PASS_2) > 0) {
        if (!silent) {Serial.print("\nAttempting SSID: "); Serial.println(WIFI_SSID_2);}
        WiFi.begin(WIFI_SSID_2, WIFI_PASS_2); unsigned long startTime = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_CONNECTION_TIMEOUT_MS) {
          delay(100); if (!silent && (millis() - startTime) % 1000 < 100) Serial.print(".");
        }
        if (WiFi.status() == WL_CONNECTED) {connected = true; connected_ssid = WIFI_SSID_2;}
        else {if (!silent) Serial.println("\nFailed to connect to SSID 2."); WiFi.disconnect(true,true); delay(100);}
      }
    #endif

    isConnectingToWiFi = false; // Reset flag after attempts
    force_display_update = true; // Trigger displayInfo to show final status (Connected/Offline)

    if (connected) {
        if (!silent) {
            Serial.println("\nWiFi connected!"); Serial.print("SSID: "); Serial.println(connected_ssid);
            Serial.print("IP address: "); Serial.println(WiFi.localIP());
        }
        #if DEBUG_LPM
        else Serial.printf("LPM Silent: WiFi connected to %s, IP: %s\n", connected_ssid.c_str(), WiFi.localIP().toString().c_str());
        #endif
        
        if (!silent) Serial.println("Configuring time via NTP (UTC)...");
        configTime(0, 0, "pool.ntp.org", "time.nist.gov"); 
        
        struct tm timeinfo;
        if(!getLocalTime(&timeinfo, 10000)){ 
            if (!silent) Serial.println("Failed to obtain initial time from NTP.");
        } else {
            if (!silent) Serial.println("Initial time configured via NTP (UTC).");
        }
    } else { 
        if (!silent) {
            Serial.println("\nCould not connect to any configured WiFi network.");
            // displayMessage("WiFi Connection Failed.", "Check credentials.", TFT_RED, true); // displayInfo will show "Offline"
        }
        #if DEBUG_LPM
        else Serial.println("LPM Silent: WiFi connection failed.");
        #endif
    }
}

bool fetchLocationFromIp(bool silent) {
    if (WiFi.status() != WL_CONNECTED) {
        if (!silent) Serial.println("Cannot fetch IP location: WiFi not connected.");
        #if DEBUG_LPM
        else Serial.println("LPM Silent: Cannot fetch IP location, no WiFi.");
        #endif
        locationDisplayStr = "IP (NoNet)"; 
        return false;
    }
    HTTPClient http; String url = "http://ip-api.com/json/?fields=status,message,lat,lon,city";
    if (!silent) {Serial.print("Fetching IP Geolocation: "); Serial.println(url);}
    #if DEBUG_LPM
    else Serial.println("LPM Silent: Fetching IP Geolocation...");
    #endif
    http.begin(url); http.setTimeout(10000); int httpCode = http.GET();
    if (!silent) {Serial.print("IP Geolocation HTTP Code: "); Serial.println(httpCode);}
    #if DEBUG_LPM
    else Serial.printf("LPM Silent: IP Geo HTTP Code: %d\n", httpCode);
    #endif

    bool success = false;
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        if (!silent) Serial.println("IP Geolocation Payload: " + payload);
        JsonDocument doc; DeserializationError error = deserializeJson(doc, payload);
        if (error) {
            if (!silent) {Serial.print(F("deserializeJson() for IP Geo failed: ")); Serial.println(error.c_str());}
            #if DEBUG_LPM
            else Serial.printf("LPM Silent: IP Geo JSON deserialize failed: %s\n", error.c_str());
            #endif
            locationDisplayStr = "IP (JSON Err)";
        } else {
            if (!doc["status"].isNull() && strcmp(doc["status"], "success") == 0) {
                deviceLatitude = doc["lat"].as<float>(); deviceLongitude = doc["lon"].as<float>();
                const char* city = doc["city"];
                if (city) locationDisplayStr = String("IP: ") + city; else locationDisplayStr = "IP: Unknown";
                
                if (!silent) Serial.printf("IP Geo Location: Lat=%.4f, Lon=%.4f, City=%s\n", deviceLatitude, deviceLongitude, city ? city : "N/A");
                #if DEBUG_LPM
                else Serial.printf("LPM Silent: IP Geo Success: Lat=%.2f Lon=%.2f City=%s\n", deviceLatitude, deviceLongitude, city ? city : "N/A");
                #endif
                
                rtc_deviceLatitude = deviceLatitude; rtc_deviceLongitude = deviceLongitude;
                strncpy(rtc_locationDisplayStr_char, locationDisplayStr.c_str(), sizeof(rtc_locationDisplayStr_char) - 1);
                rtc_locationDisplayStr_char[sizeof(rtc_locationDisplayStr_char) - 1] = '\0';
                success = true;
            } else { 
                locationDisplayStr = String("IP (API Err)");
            }
        }
    } else { 
        locationDisplayStr = String("IP (HTTP Err)");
    }
    http.end(); return success;
}

bool fetchUVData(bool silent) {
    if (WiFi.status() != WL_CONNECTED) {
        if (!silent) Serial.println("WiFi not connected, cannot fetch UV data.");
        #if DEBUG_LPM
        else Serial.println("LPM Silent: No WiFi, cannot fetch UV data.");
        #endif
        initializeForecastData(true); 
        lastUpdateTimeStr = "Offline";
        strncpy(rtc_lastUpdateTimeStr_char, "Offline", sizeof(rtc_lastUpdateTimeStr_char)-1);
        rtc_lastUpdateTimeStr_char[sizeof(rtc_lastUpdateTimeStr_char)-1] = '\0';
        rtc_hasValidData = false; 
        dataJustFetched = true; 
        return false;
    }

    HTTPClient http; String apiUrl = openMeteoUrl + "?latitude=" + String(deviceLatitude, 4) +
                    "&longitude=" + String(deviceLongitude, 4) + "&hourly=uv_index&forecast_days=1&timezone=auto";
    if (!silent) {Serial.print("Fetching UV Data from URL: "); Serial.println(apiUrl);}
    #if DEBUG_LPM
    else Serial.println("LPM Silent: Fetching UV data...");
    #endif
    http.begin(apiUrl); http.setTimeout(15000); int httpCode = http.GET();
    if (!silent) {Serial.print("Open-Meteo API HTTP Code: "); Serial.println(httpCode);}
    #if DEBUG_LPM
    else Serial.printf("LPM Silent: Open-Meteo HTTP Code: %d\n", httpCode);
    #endif

    bool success = false;
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString(); JsonDocument doc; DeserializationError error = deserializeJson(doc, payload);
        if (error) {
            if (!silent) {Serial.print(F("deserializeJson() for UV data failed: ")); Serial.println(error.c_str());}
            #if DEBUG_LPM
            else Serial.printf("LPM Silent: UV JSON deserialize failed: %s\n", error.c_str());
            #endif
            initializeForecastData(true); rtc_hasValidData = false;
        } else { 
            if (!doc["utc_offset_seconds"].isNull()) {
                long api_utc_offset_sec = doc["utc_offset_seconds"].as<long>();
                configTime(api_utc_offset_sec, 0, "pool.ntp.org", "time.nist.gov"); 
                if (!silent) Serial.println("ESP32 local time reconfigured using Open-Meteo offset.");
                #if DEBUG_LPM
                else Serial.println("LPM Silent: ESP32 time reconfigured from API offset.");
                #endif
                delay(100); 
            }

            if (!doc["hourly"].isNull() && !doc["hourly"]["time"].isNull() && !doc["hourly"]["uv_index"].isNull()) {
                JsonArray hourly_time_list = doc["hourly"]["time"].as<JsonArray>();
                JsonArray hourly_uv_list = doc["hourly"]["uv_index"].as<JsonArray>();
                struct tm timeinfo; 
                if (!getLocalTime(&timeinfo, 5000)) { 
                    if (!silent) Serial.println("Failed to obtain ESP32 local time for forecast matching.");
                    #if DEBUG_LPM
                    else Serial.println("LPM Silent: Failed to get local time for forecast matching.");
                    #endif
                    initializeForecastData(true); rtc_hasValidData = false;
                } else {
                    int currentHourLocal = timeinfo.tm_hour; int startIndex = -1;
                    for (int i = 0; i < hourly_time_list.size(); ++i) {
                        String api_time_str = hourly_time_list[i].as<String>();
                        if (api_time_str.length() >= 13) {
                            int api_hour = api_time_str.substring(11, 13).toInt();
                            if (api_hour >= currentHourLocal) {startIndex = i; break;}
                        }
                    }
                    if (startIndex != -1) { 
                        for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
                            if (startIndex + i < hourly_uv_list.size() && startIndex + i < hourly_time_list.size()) {
                                JsonVariant uv_val_variant = hourly_uv_list[startIndex + i];
                                hourlyUV[i] = uv_val_variant.isNull() ? 0.0f : uv_val_variant.as<float>();
                                if (hourlyUV[i] < 0) hourlyUV[i] = 0.0f; 
                                rtc_hourlyUV[i] = hourlyUV[i]; 
                                String api_t_str = hourly_time_list[startIndex + i].as<String>();
                                forecastHours[i] = api_t_str.substring(11, 13).toInt(); 
                                rtc_forecastHours[i] = forecastHours[i]; 
                            } else { 
                                hourlyUV[i] = -1.0f; forecastHours[i] = -1; 
                                rtc_hourlyUV[i] = -1.0f; rtc_forecastHours[i] = -1;
                            }
                        }
                        success = true; 
                        rtc_hasValidData = true; 
                        if (!silent) Serial.println("Successfully populated hourly forecast data.");
                        #if DEBUG_LPM
                        else Serial.println("LPM Silent: Successfully populated hourly forecast data to RAM & RTC.");
                        #endif
                    } else { 
                        if (!silent) Serial.println("No suitable starting forecast index found.");
                        #if DEBUG_LPM
                        else Serial.println("LPM Silent: No suitable forecast index.");
                        #endif
                        initializeForecastData(true); rtc_hasValidData = false; 
                    }
                }
            } else { 
                if (!silent) Serial.println("Hourly UV data structure not found in JSON.");
                 #if DEBUG_LPM
                else Serial.println("LPM Silent: Hourly UV data structure not found.");
                #endif
                initializeForecastData(true); rtc_hasValidData = false;
            }
        }
        struct tm timeinfo_update;
        if(getLocalTime(&timeinfo_update, 1000)){ 
            char timeStrBuffer[16]; 
            const char* tz_abbr = doc["timezone_abbreviation"].isNull() ? nullptr : doc["timezone_abbreviation"].as<const char*>();
            
            if (tz_abbr && strlen(tz_abbr) > 0 && strlen(tz_abbr) < 5) { 
                snprintf(timeStrBuffer, sizeof(timeStrBuffer), "%02d:%02d %s", timeinfo_update.tm_hour, timeinfo_update.tm_min, tz_abbr);
            } else {
                strftime(timeStrBuffer, sizeof(timeStrBuffer), "%H:%M", &timeinfo_update); 
            }
            lastUpdateTimeStr = String(timeStrBuffer);
            strncpy(rtc_lastUpdateTimeStr_char, lastUpdateTimeStr.c_str(), sizeof(rtc_lastUpdateTimeStr_char)-1);
            rtc_lastUpdateTimeStr_char[sizeof(rtc_lastUpdateTimeStr_char)-1] = '\0';
        } else { 
            lastUpdateTimeStr = "Time Err";
            strncpy(rtc_lastUpdateTimeStr_char, "Time Err", sizeof(rtc_lastUpdateTimeStr_char)-1);
            rtc_lastUpdateTimeStr_char[sizeof(rtc_lastUpdateTimeStr_char)-1] = '\0';
        }
    } else { 
        initializeForecastData(true); rtc_hasValidData = false;
        if (WiFi.status() != WL_CONNECTED) { 
            lastUpdateTimeStr = "Offline";
            strncpy(rtc_lastUpdateTimeStr_char, "Offline", sizeof(rtc_lastUpdateTimeStr_char)-1);
            rtc_lastUpdateTimeStr_char[sizeof(rtc_lastUpdateTimeStr_char)-1] = '\0';
        } else { 
            lastUpdateTimeStr = "API Err";
            strncpy(rtc_lastUpdateTimeStr_char, "API Err", sizeof(rtc_lastUpdateTimeStr_char)-1);
            rtc_lastUpdateTimeStr_char[sizeof(rtc_lastUpdateTimeStr_char)-1] = '\0';
        }
    }
    http.end(); 
    dataJustFetched = true; 
    return success;
}
