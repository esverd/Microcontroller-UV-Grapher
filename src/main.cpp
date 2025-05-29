#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>      // ESP32 TFT Library
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "soc/rtc_cntl_reg.h" // For detailed ESP32 sleep wake reasons
#include "soc/soc.h"           // (as above)
#include <esp_sleep.h>       // For esp_sleep_get_wakeup_cause

#include "secrets.h" // Your WiFi credentials and other secrets

// --- Configuration & Pins ---
String openMeteoUrl = "https://api.open-meteo.com/v1/forecast";
const unsigned long UPDATE_INTERVAL_MS_NORMAL_MODE = 15 * 60 * 1000; // 15 minutes
const unsigned long SCREEN_ON_DURATION_LPM_MS = 60 * 1000;      // 1 minute screen on time in LPM
unsigned long lastDataFetchAttemptMs = 0; // Tracks time of last data fetch attempt
const int WIFI_CONNECTION_TIMEOUT_MS = 15000; // 15 seconds

#define TFT_BL 4 // TFT Backlight Control Pin (Confirm this matches your hardware)

#define BUTTON_PRIMARY_PIN 0    // Info (short), Location (long, if info on), Wake from LPM (short)
#define BUTTON_LP_TOGGLE_PIN 35 // Toggle Low Power Mode (long)

// --- Debugging Flags ---
#define DEBUG_SERIAL_GENERAL 1 // General operational serial prints
#define DEBUG_SERIAL_BUTTONS 1 // Detailed button state serial prints
#define DEBUG_SERIAL_POWERMODE 1  // Power mode transition serial prints
#define DEBUG_SERIAL_WIFI 1    // WiFi connection serial prints
#define DEBUG_SERIAL_HTTP 1    // HTTP request serial prints
#define DEBUG_SERIAL_JSON 0    // JSON parsing serial prints (can be verbose)
#define DEBUG_SERIAL_GRAPH 0 // Graph drawing coordinate serial prints

// --- Global Working Variables ---
TFT_eSPI tft = TFT_eSPI();
String lastUpdateTimeStr = "Never"; // For "Updated: HH:MM"

const int HOURLY_FORECAST_COUNT = 6;
float hourlyUV[HOURLY_FORECAST_COUNT];
int forecastHours[HOURLY_FORECAST_COUNT];

// Button processing constants
const uint16_t LONG_PRESS_MIN_MS = 1000;          // Min duration for a long press
const uint16_t SHORT_PRESS_MAX_MS = 700;         // Max duration for a short press to be valid on release
const uint16_t DEBOUNCE_DELAY_MS = 50;           // Debounce for initial press

// Display & Mode States
bool showInfoOverlay = false;
bool force_display_update = true; 
bool dataJustFetched = false;     

// Location Management
bool useGpsFromSecrets = false; 
float deviceLatitude = 0.0f;    
float deviceLongitude = 0.0f;   
String locationDisplayStr = "Initializing...";

// Low Power Mode (LPM) State
bool isLowPowerModeActive = false;      
unsigned long screenActiveUntilMs = 0;  
bool temporaryScreenWakeupActive = false; 

// RTC Persisted Data
RTC_DATA_ATTR uint32_t rtc_magic_cookie = 0; 
const uint32_t RTC_MAGIC_COOKIE_VALUE = 0xDEADBEEF;
RTC_DATA_ATTR bool rtc_isLowPowerModeActive = false;
RTC_DATA_ATTR bool rtc_hasValidData = false;
RTC_DATA_ATTR float rtc_hourlyUV[HOURLY_FORECAST_COUNT];
RTC_DATA_ATTR int rtc_forecastHours[HOURLY_FORECAST_COUNT];
RTC_DATA_ATTR char rtc_lastUpdateTimeStr[10];
RTC_DATA_ATTR char rtc_locationDisplayStr[32];
RTC_DATA_ATTR float rtc_deviceLatitude;
RTC_DATA_ATTR float rtc_deviceLongitude;
RTC_DATA_ATTR bool rtc_useGpsFromSecretsGlobal;

// Custom Colors
#define TFT_DARK_ORANGE 0xFC60 

// --- Function Declarations ---
void connectToWiFi(bool silent);
bool fetchUVData(bool silent);
void displayInfo();
void drawForecastGraph(int start_y_offset);
void displayMessage(const String& msg_line1, const String& msg_line2 = "", int color = TFT_WHITE, bool allowDisplay = true);
void initializeForecastData();
void handle_buttons();
bool fetchLocationFromIp(bool silent);
void enterDeepSleep(uint64_t sleep_duration_us, bool alsoEnableButtonWake);
uint64_t calculateSleepUntilNextHH05();
void turnScreenOn();
void turnScreenOff();
void saveStateToRTC();
void loadStateFromRTC();
void printWakeupReason();


// --- Power Management & Sleep ---
void turnScreenOn() {
    #if DEBUG_SERIAL_POWERMODE
    Serial.println("Screen: Turning ON");
    #endif
    #ifdef TFT_BL
        digitalWrite(TFT_BL, HIGH); 
    #endif
    tft.writecommand(0x29); 
    delay(20); 
}

void turnScreenOff() {
    #if DEBUG_SERIAL_POWERMODE
    Serial.println("Screen: Turning OFF");
    #endif
    #ifdef TFT_BL
        digitalWrite(TFT_BL, LOW); 
    #endif
    tft.writecommand(0x28); 
}

void enterDeepSleep(uint64_t sleep_duration_us, bool alsoEnableButtonWake) {
    saveStateToRTC(); 
    #if DEBUG_SERIAL_POWERMODE
    Serial.printf("Core: Entering deep sleep for ~%llu seconds. Button wake: %s\n",
                  sleep_duration_us / 1000000ULL, alsoEnableButtonWake ? "Enabled (GPIO0)" : "Disabled");
    #endif
    Serial.println("-------------------------------------\n");
    Serial.flush();

    turnScreenOff();

    esp_sleep_enable_timer_wakeup(sleep_duration_us);
    if (alsoEnableButtonWake) {
        esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0); 
    }
    esp_deep_sleep_start(); 
}

uint64_t calculateSleepUntilNextHH05() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 5000)) { 
        #if DEBUG_SERIAL_POWERMODE
        Serial.println("SleepCalc: Failed to get local time. Defaulting to 1 hour sleep.");
        #endif
        return 60UL * 60UL * 1000000ULL; 
    }

    int current_minute = timeinfo.tm_min;
    int current_second = timeinfo.tm_sec;
    long sleep_seconds; 

    if (current_minute < 5) {
        sleep_seconds = (5L - current_minute) * 60L - current_second;
    } else {
        sleep_seconds = (60L - current_minute + 5L) * 60L - current_second;
    }

    if (sleep_seconds <= 0) { 
        sleep_seconds = (60L * 60L) - current_second; 
        sleep_seconds += (5L*60L); 
    }
    if (sleep_seconds < 5) sleep_seconds = 5; 

    #if DEBUG_SERIAL_POWERMODE
    char time_buf[20];
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &timeinfo);
    Serial.printf("SleepCalc: Current time %s. Calculated sleep: %ld seconds.\n", time_buf, sleep_seconds);
    #endif
    return (uint64_t)sleep_seconds * 1000000ULL;
}

// --- RTC State Management ---
void saveStateToRTC() {
    rtc_isLowPowerModeActive = isLowPowerModeActive;
    rtc_useGpsFromSecretsGlobal = useGpsFromSecrets;
    rtc_deviceLatitude = deviceLatitude;
    rtc_deviceLongitude = deviceLongitude;

    if (rtc_hasValidData) { 
        for(int i=0; i<HOURLY_FORECAST_COUNT; ++i) {
            rtc_hourlyUV[i] = hourlyUV[i];
            rtc_forecastHours[i] = forecastHours[i];
        }
        lastUpdateTimeStr.toCharArray(rtc_lastUpdateTimeStr, sizeof(rtc_lastUpdateTimeStr));
        locationDisplayStr.toCharArray(rtc_locationDisplayStr, sizeof(rtc_locationDisplayStr));
    }
    
    #if DEBUG_SERIAL_POWERMODE
    Serial.printf("RTC Save: LPM=%d, UseSecrets=%d, ValidData=%d, Lat=%.2f, Lon=%.2f, LastUpd=%s, LocStr=%s\n",
                  rtc_isLowPowerModeActive, rtc_useGpsFromSecretsGlobal, rtc_hasValidData,
                  rtc_deviceLatitude, rtc_deviceLongitude, rtc_lastUpdateTimeStr, rtc_locationDisplayStr);
    #endif
}

void loadStateFromRTC() {
    if (rtc_magic_cookie != RTC_MAGIC_COOKIE_VALUE) {
        #if DEBUG_SERIAL_POWERMODE
        Serial.println("RTC: Magic cookie mismatch/not set. Initializing RTC variables to defaults.");
        #endif
        rtc_magic_cookie = RTC_MAGIC_COOKIE_VALUE; 
        rtc_isLowPowerModeActive = false;          
        rtc_hasValidData = false;
        rtc_useGpsFromSecretsGlobal = false;       
        rtc_deviceLatitude = MY_LATITUDE;          
        rtc_deviceLongitude = MY_LONGITUDE;        
        strcpy(rtc_lastUpdateTimeStr, "Never");
        strcpy(rtc_locationDisplayStr, "Init...");
        for(int i=0; i<HOURLY_FORECAST_COUNT; ++i) { rtc_hourlyUV[i] = -1.0f; rtc_forecastHours[i] = -1; }
        saveStateToRTC(); 
    }

    isLowPowerModeActive = rtc_isLowPowerModeActive;
    useGpsFromSecrets = rtc_useGpsFromSecretsGlobal;
    deviceLatitude = rtc_deviceLatitude;
    deviceLongitude = rtc_deviceLongitude;
    lastUpdateTimeStr = String(rtc_lastUpdateTimeStr);
    locationDisplayStr = String(rtc_locationDisplayStr);

    if (rtc_hasValidData) {
        for(int i=0; i<HOURLY_FORECAST_COUNT; ++i) {
            hourlyUV[i] = rtc_hourlyUV[i];
            forecastHours[i] = rtc_forecastHours[i];
        }
        #if DEBUG_SERIAL_GENERAL
        Serial.println("RTC: Loaded valid forecast data from RTC.");
        #endif
    } else {
        #if DEBUG_SERIAL_GENERAL
        Serial.println("RTC: No valid forecast data in RTC or uninitialized.");
        #endif
        initializeForecastData(); 
    }
    #if DEBUG_SERIAL_POWERMODE
    Serial.printf("RTC Load: LPM=%d, UseSecrets=%d, ValidData=%d, Lat=%.2f, Lon=%.2f, LastUpd=%s, LocStr=%s\n",
                  isLowPowerModeActive, useGpsFromSecrets, rtc_hasValidData,
                  deviceLatitude, deviceLongitude, lastUpdateTimeStr.c_str(), locationDisplayStr.c_str());
    #endif
}

void printWakeupReason(){
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  #if DEBUG_SERIAL_POWERMODE
  switch(wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0 : Serial.println("Wakeup cause: EXT0 (GPIO0)"); break;
    case ESP_SLEEP_WAKEUP_EXT1 : Serial.println("Wakeup cause: EXT1"); break;
    case ESP_SLEEP_WAKEUP_TIMER : Serial.println("Wakeup cause: Timer"); break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD : Serial.println("Wakeup cause: Touchpad"); break;
    case ESP_SLEEP_WAKEUP_ULP : Serial.println("Wakeup cause: ULP program"); break;
    default : Serial.printf("Wakeup cause: Other/PowerOn (Reason: %d)\n",wakeup_reason); break;
  }
  #endif
}

// --- Setup ---
void setup() {
  Serial.begin(115200);
  delay(1500); 
  Serial.println("\n\n#####################################");
  Serial.println("UV Index Monitor Booting...");
  Serial.println("#####################################");

  #ifdef TFT_BL
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, LOW); 
  #endif

  pinMode(BUTTON_PRIMARY_PIN, INPUT_PULLUP);
  pinMode(BUTTON_LP_TOGGLE_PIN, INPUT_PULLUP);

  printWakeupReason();
  loadStateFromRTC(); 

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  bool perform_silent_update_and_sleep = false;
  bool start_normal_interactive_session = false;
  bool start_lpm_temporary_interactive_session = false;

  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) { 
    if (isLowPowerModeActive) {
      start_lpm_temporary_interactive_session = true;
    } else {
      #if DEBUG_SERIAL_POWERMODE
      Serial.println("Setup: Woken by Primary Button but LPM was OFF. Starting Normal Mode.");
      #endif
      start_normal_interactive_session = true;
    }
  } else if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
    if (isLowPowerModeActive) {
      perform_silent_update_and_sleep = true;
    } else {
      #if DEBUG_SERIAL_POWERMODE
      Serial.println("Setup: Warning! Timer wake while LPM is OFF. Defaulting to Normal Mode.");
      #endif
      start_normal_interactive_session = true;
    }
  } else { 
    if (isLowPowerModeActive) {
      #if DEBUG_SERIAL_POWERMODE
      Serial.println("Setup: Initial boot, but RTC indicates LPM was ON. Starting LPM cycle (silent update).");
      #endif
      perform_silent_update_and_sleep = true;
    } else {
      #if DEBUG_SERIAL_POWERMODE
      Serial.println("Setup: Initial boot or reset. LPM is OFF. Starting Normal Mode.");
      #endif
      start_normal_interactive_session = true;
    }
  }

  if (start_lpm_temporary_interactive_session) {
    #if DEBUG_SERIAL_POWERMODE
    Serial.println("Setup Action: LPM Temporary Interactive Session");
    #endif
    temporaryScreenWakeupActive = true; 
    screenActiveUntilMs = millis() + SCREEN_ON_DURATION_LPM_MS; 
    
    turnScreenOn();
    tft.init(); tft.setRotation(1); tft.fillScreen(TFT_BLACK); tft.setTextDatum(MC_DATUM);
    
    if (rtc_hasValidData) { 
        force_display_update = true;
        #if DEBUG_SERIAL_GENERAL
        Serial.println("Setup: Displaying cached RTC data for LPM temp wake.");
        #endif
    } else {
        initializeForecastData(); 
        force_display_update = true;
    }
    displayMessage("Waking up...", "", TFT_WHITE, true); 
    
    connectToWiFi(true); 
    if (WiFi.status() == WL_CONNECTED) {
      if (fetchUVData(true)) { 
        lastDataFetchAttemptMs = millis(); 
        #if DEBUG_SERIAL_GENERAL
        Serial.println("Setup: Fresh data fetched during LPM temp wake.");
        #endif
      }
    }
    force_display_update = true; 

  } else if (start_normal_interactive_session) {
    #if DEBUG_SERIAL_POWERMODE
    Serial.println("Setup Action: Normal Interactive Session");
    #endif
    isLowPowerModeActive = false; 
    temporaryScreenWakeupActive = false;
    
    turnScreenOn();
    tft.init(); tft.setRotation(1); tft.fillScreen(TFT_BLACK); tft.setTextDatum(MC_DATUM);

    if (rtc_hasValidData) { 
        force_display_update = true;
        #if DEBUG_SERIAL_GENERAL
        Serial.println("Setup: Displaying cached RTC data for Normal Mode start.");
        #endif
    } else {
        initializeForecastData();
        force_display_update = true;
    }
    
    displayMessage("Connecting WiFi...", "", TFT_YELLOW, true);
    connectToWiFi(false); 

    if (WiFi.status() == WL_CONNECTED) {
        if (!useGpsFromSecrets) { 
            displayMessage("Fetching IP Loc...", "", TFT_SKYBLUE, true);
            if (fetchLocationFromIp(false)) {
                // locationDisplayStr updated
            } else { 
                useGpsFromSecrets = true; 
                deviceLatitude = MY_LATITUDE; deviceLongitude = MY_LONGITUDE;
                locationDisplayStr = "IP Fail>Secrets";
            }
        } else { 
             deviceLatitude = MY_LATITUDE; deviceLongitude = MY_LONGITUDE; 
             locationDisplayStr = "Secrets GPS";
        }
        rtc_useGpsFromSecretsGlobal = useGpsFromSecrets; 
        rtc_deviceLatitude = deviceLatitude;
        rtc_deviceLongitude = deviceLongitude;
        locationDisplayStr.toCharArray(rtc_locationDisplayStr, sizeof(rtc_locationDisplayStr));

        String currentStatusForDisplay = locationDisplayStr; 
        if (currentStatusForDisplay.length() > 18) currentStatusForDisplay = currentStatusForDisplay.substring(0,15) + "...";
        displayMessage("Fetching UV data...", currentStatusForDisplay, TFT_CYAN, true);
        if (fetchUVData(false)) {
            lastDataFetchAttemptMs = millis();
        }
    } else { 
        locationDisplayStr = "Offline"; 
        locationDisplayStr.toCharArray(rtc_locationDisplayStr, sizeof(rtc_locationDisplayStr)); 
        if (!rtc_hasValidData) initializeForecastData(); 
    }
    force_display_update = true; 

  } else if (perform_silent_update_and_sleep) {
    #if DEBUG_SERIAL_POWERMODE
    Serial.println("Setup Action: Silent Update & Sleep");
    #endif
    connectToWiFi(true); 
    if (WiFi.status() == WL_CONNECTED) {
      if (!rtc_useGpsFromSecretsGlobal) { 
          fetchLocationFromIp(true); 
      } else { 
          deviceLatitude = rtc_deviceLatitude; 
          deviceLongitude = rtc_deviceLongitude;
      }
      if(fetchUVData(true)) { 
          #if DEBUG_SERIAL_GENERAL
          Serial.println("Setup: Silent data fetch successful.");
          #endif
      } else {
          #if DEBUG_SERIAL_GENERAL
          Serial.println("Setup: Silent data fetch failed.");
          #endif
      }
    } else {
        #if DEBUG_SERIAL_GENERAL
        Serial.println("Setup: No WiFi for silent update.");
        #endif
    }
    enterDeepSleep(calculateSleepUntilNextHH05(), true); 
  }
}


// --- Main Loop ---
void loop() {
  unsigned long currentMillis = millis();
  handle_buttons(); 

  if (isLowPowerModeActive) {
    if (temporaryScreenWakeupActive) {
      if (currentMillis >= screenActiveUntilMs) { 
        #if DEBUG_SERIAL_POWERMODE
        Serial.println("Loop: LPM Screen timeout. Entering deep sleep.");
        #endif
        temporaryScreenWakeupActive = false;
        enterDeepSleep(calculateSleepUntilNextHH05(), true);
      }
      if (dataJustFetched && WiFi.status() == WL_CONNECTED) {
          // Data was fetched (likely due to location change by button press)
          // displayInfo will be called if force_display_update is true
      }
    } else {
      #if DEBUG_SERIAL_POWERMODE
      Serial.println("Loop: LPM Active but screen not temp active. Forcing deep sleep.");
      #endif
      enterDeepSleep(calculateSleepUntilNextHH05(), true);
    }
  } else { // Normal Mode
    if (dataJustFetched || (WiFi.status() == WL_CONNECTED && (currentMillis - lastDataFetchAttemptMs >= UPDATE_INTERVAL_MS_NORMAL_MODE))) {
      if (WiFi.status() == WL_CONNECTED) {
        #if DEBUG_SERIAL_GENERAL
        Serial.printf("Loop: Normal mode data update. dataJustFetched: %d, IntervalPassed: %s\n",
            dataJustFetched, (currentMillis - lastDataFetchAttemptMs >= UPDATE_INTERVAL_MS_NORMAL_MODE) ? "YES":"NO");
        #endif
        String tempLocStr = locationDisplayStr;
        if (tempLocStr.length() > 18) tempLocStr = tempLocStr.substring(0,15) + "...";
        // Pass 'true' for allowDisplay to ensure message shows
        displayMessage("Updating UV data...", tempLocStr, TFT_CYAN, true); 
        
        if (fetchUVData(false)) { 
          lastDataFetchAttemptMs = millis(); 
        } else {
          lastDataFetchAttemptMs = millis(); 
        }
      } else { 
        if (currentMillis - lastDataFetchAttemptMs >= UPDATE_INTERVAL_MS_NORMAL_MODE) { 
            #if DEBUG_SERIAL_GENERAL
            Serial.println("Loop: Normal mode, no WiFi, re-initializing data due to interval.");
            #endif
            initializeForecastData(); 
            lastUpdateTimeStr = "Offline";
            dataJustFetched = true; 
            lastDataFetchAttemptMs = millis();
        }
      }
      force_display_update = true; 
    }
  }

  if (force_display_update || dataJustFetched) {
    if (isLowPowerModeActive && !temporaryScreenWakeupActive) {
        // Screen is OFF in LPM, do not draw
    } else {
        displayInfo(); 
    }
    force_display_update = false;
    dataJustFetched = false; 
  }
  delay(50); 
}

// --- Combined Button Handler ---
void handle_buttons() {
    unsigned long currentMillis = millis();
    bool primary_button_physical_state = (digitalRead(BUTTON_PRIMARY_PIN) == LOW);
    bool lp_toggle_button_physical_state = (digitalRead(BUTTON_LP_TOGGLE_PIN) == LOW);

    // Static variables for button state machines
    static bool primary_button_last_state = HIGH; 
    static unsigned long primary_press_time = 0;
    static bool primary_is_long_press_candidate = false;
    static bool primary_long_press_action_taken = false; // Renamed from _achieved

    static bool lp_button_last_state = HIGH;
    static unsigned long lp_press_time = 0;
    static bool lp_is_long_press_candidate = false;
    static bool lp_long_press_action_taken = false; // Renamed from _achieved


    // --- BUTTON_PRIMARY_PIN Debounce and Press/Hold Logic ---
    if (primary_button_physical_state != primary_button_last_state) { 
        delay(DEBOUNCE_DELAY_MS); // Use defined constant
        primary_button_physical_state = (digitalRead(BUTTON_PRIMARY_PIN) == LOW); 
    }

    if (primary_button_physical_state && !primary_button_last_state) { 
        primary_press_time = currentMillis;
        primary_is_long_press_candidate = true;  
        primary_long_press_action_taken = false; 
        #if DEBUG_SERIAL_BUTTONS
        Serial.println("Button Primary: Pressed");
        #endif
    } else if (!primary_button_physical_state && primary_button_last_state) { 
        #if DEBUG_SERIAL_BUTTONS
        Serial.print("Button Primary: Released. ");
        #endif
        if (primary_is_long_press_candidate && !primary_long_press_action_taken) { 
            if ((currentMillis - primary_press_time) < SHORT_PRESS_MAX_MS) { // Use defined constant
                #if DEBUG_SERIAL_BUTTONS
                Serial.println("Action: Short Press (Info Overlay)");
                #endif
                showInfoOverlay = !showInfoOverlay;
                force_display_update = true;
                if (temporaryScreenWakeupActive) screenActiveUntilMs = currentMillis + SCREEN_ON_DURATION_LPM_MS;
            } else {
                #if DEBUG_SERIAL_BUTTONS
                Serial.println("Action: Ignored (release too late for short press).");
                #endif
            }
        }
        primary_is_long_press_candidate = false; 
    }

    if (primary_is_long_press_candidate && primary_button_physical_state && !primary_long_press_action_taken) { 
        if (currentMillis - primary_press_time >= LONG_PRESS_MIN_MS) { // Use defined constant
            #if DEBUG_SERIAL_BUTTONS
            Serial.println("Button Primary: Action -> Long Press (Location Toggle)");
            #endif
            if (showInfoOverlay) { 
                useGpsFromSecrets = !useGpsFromSecrets;
                rtc_useGpsFromSecretsGlobal = useGpsFromSecrets;
                if (useGpsFromSecrets) {
                    deviceLatitude = MY_LATITUDE; deviceLongitude = MY_LONGITUDE; locationDisplayStr = "Secrets GPS";
                } else { 
                    locationDisplayStr = "IP Geo...";
                }
                rtc_deviceLatitude = deviceLatitude;
                rtc_deviceLongitude = deviceLongitude;
                locationDisplayStr.toCharArray(rtc_locationDisplayStr, sizeof(rtc_locationDisplayStr));

                if (!useGpsFromSecrets && (temporaryScreenWakeupActive || !isLowPowerModeActive)) {
                    bool canDisplayMessages = (temporaryScreenWakeupActive || !isLowPowerModeActive);
                    if(canDisplayMessages) {force_display_update = true; displayInfo();} 
                    fetchLocationFromIp(!canDisplayMessages); 
                    rtc_deviceLatitude = deviceLatitude; rtc_deviceLongitude = deviceLongitude; 
                    locationDisplayStr.toCharArray(rtc_locationDisplayStr, sizeof(rtc_locationDisplayStr));
                }
                dataJustFetched = true; 
                force_display_update = true;
                #if DEBUG_SERIAL_GENERAL
                Serial.printf("Location mode toggled to: %s\n", useGpsFromSecrets ? "Secrets GPS" : "IP Geolocation");
                #endif
            } else {
                 #if DEBUG_SERIAL_BUTTONS
                 Serial.println("Button Primary: Long press for location ignored (Info overlay OFF).");
                 #endif
            }
            primary_long_press_action_taken = true; 
            if (temporaryScreenWakeupActive) screenActiveUntilMs = currentMillis + SCREEN_ON_DURATION_LPM_MS;
        }
    }
    primary_button_last_state = primary_button_physical_state;


    // --- BUTTON_LP_TOGGLE_PIN Debounce and Press/Hold Logic ---
    if (lp_toggle_button_physical_state != lp_button_last_state) { 
        delay(DEBOUNCE_DELAY_MS); // Use defined constant
        lp_toggle_button_physical_state = (digitalRead(BUTTON_LP_TOGGLE_PIN) == LOW); 
    }

    if (lp_toggle_button_physical_state && !lp_button_last_state) { 
        lp_press_time = currentMillis;
        lp_is_long_press_candidate = true; 
        lp_long_press_action_taken = false;
        #if DEBUG_SERIAL_BUTTONS
        Serial.println("Button LP Toggle: Pressed");
        #endif
    } else if (!lp_toggle_button_physical_state && lp_button_last_state) { 
        lp_is_long_press_candidate = false;
        #if DEBUG_SERIAL_BUTTONS
        Serial.println("Button LP Toggle: Released");
        #endif
    }

    if (lp_is_long_press_candidate && lp_toggle_button_physical_state && !lp_long_press_action_taken) { 
        if (currentMillis - lp_press_time >= LONG_PRESS_MIN_MS) { // Use defined constant
            #if DEBUG_SERIAL_BUTTONS
            Serial.println("Button LP Toggle: Action -> Long Press (Toggle LPM)");
            #endif
            isLowPowerModeActive = !isLowPowerModeActive; 
            rtc_isLowPowerModeActive = isLowPowerModeActive; 

            if (isLowPowerModeActive) { 
                temporaryScreenWakeupActive = false;
                
                turnScreenOn(); 
                displayMessage("Low Power Mode: ON", "Entering sleep...", TFT_BLUE, true);
                delay(2000); 
                
                saveStateToRTC(); 
                enterDeepSleep(calculateSleepUntilNextHH05(), true); 
            } else { 
                temporaryScreenWakeupActive = false;
                turnScreenOn(); 
                #if DEBUG_SERIAL_POWERMODE
                Serial.println("Switched to Normal Mode. Screen ON, regular updates will resume.");
                #endif
                displayMessage("Low Power Mode: OFF", "", TFT_GREEN, true);
                delay(1500);
                dataJustFetched = true; 
                force_display_update = true; 
            }
            lp_long_press_action_taken = true; 
        }
    }
    lp_button_last_state = lp_toggle_button_physical_state;
}


// --- Utility, HTTP, Display Functions ---

void initializeForecastData() {
    for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
        hourlyUV[i] = -1.0f;
        forecastHours[i] = -1;
    }
}

bool fetchLocationFromIp(bool silent) {
    if (WiFi.status() != WL_CONNECTED) {
        #if DEBUG_SERIAL_HTTP
        if (!silent) Serial.println("fetchLocIP: No WiFi");
        #endif
        locationDisplayStr = "IP (NoNet)"; return false;
    }
    HTTPClient http;
    String url = "http://ip-api.com/json/?fields=status,message,lat,lon,city";
    #if DEBUG_SERIAL_HTTP
    if (!silent) { Serial.print("fetchLocIP: GET "); Serial.println(url); }
    #endif
    http.begin(url); http.setTimeout(10000);
    int httpCode = http.GET();
    #if DEBUG_SERIAL_HTTP
    if (!silent) { Serial.printf("fetchLocIP: HTTP Code: %d\n", httpCode); }
    #endif
    bool success = false;
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        #if DEBUG_SERIAL_JSON
        if (!silent) Serial.printf("fetchLocIP: Payload: %s\n", payload.c_str());
        #endif
        JsonDocument doc; DeserializationError error = deserializeJson(doc, payload);
        if (error) {
            #if DEBUG_SERIAL_JSON
            if (!silent) { Serial.printf("fetchLocIP: JSON Err: %s\n", error.c_str()); }
            #endif
            locationDisplayStr = "IP (JSON Err)";
        } else {
            if (doc["status"] == "success") {
                deviceLatitude = doc["lat"].as<float>(); deviceLongitude = doc["lon"].as<float>();
                String city = doc["city"].as<String>();
                locationDisplayStr = "IP: " + (city.length() > 0 ? city : "Unknown");
                success = true;
            } else { locationDisplayStr = "IP (API Err)"; }
        }
    } else { locationDisplayStr = String("IP (HTTP ")+String(httpCode)+")"; }
    http.end();
    #if DEBUG_SERIAL_GENERAL
    if (!silent) { Serial.printf("fetchLocIP: Result: %s. LocStr: %s\n", success?"OK":"FAIL", locationDisplayStr.c_str());}
    #endif
    return success;
}

void displayMessage(const String& msg_line1, const String& msg_line2, int color, bool allowDisplay) {
  if (!allowDisplay) {
      #if DEBUG_SERIAL_GENERAL
      Serial.println("DisplayMessage suppressed by allowDisplay=false: " + msg_line1);
      #endif
      return;
  }
  if (isLowPowerModeActive && !temporaryScreenWakeupActive && msg_line1 != "Low Power Mode: ON") { // Allow LPM ON message
      #if DEBUG_SERIAL_GENERAL
      Serial.println("DisplayMessage suppressed by LPM state (not temp wake): " + msg_line1);
      #endif
      return;
  }

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.setTextFont(2); tft.setTextDatum(MC_DATUM);
  tft.drawString(msg_line1, tft.width() / 2, tft.height() / 2 - (msg_line2 != "" ? 10 : 0) );
  if (msg_line2 != "") {
    tft.drawString(msg_line2, tft.width() / 2, tft.height() / 2 + 10);
  }
  #if DEBUG_SERIAL_GENERAL
  Serial.println("DisplayMessage (Displayed): " + msg_line1 + " | " + msg_line2);
  #endif
}

void connectToWiFi(bool silent) {
    #if DEBUG_SERIAL_WIFI
    Serial.println("connectToWiFi: Entered.");
    if (!silent) { Serial.println("connectToWiFi: Connecting to WiFi (verbose)...");}
    #endif

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true, true); 
    delay(200); 

    bool connected = false;
    String connected_ssid = "";

    #if defined(WIFI_SSID_1)
        #if DEBUG_SERIAL_WIFI
        if (!silent) Serial.printf("connectToWiFi: Secrets Check - WIFI_SSID_1 = [%s], Length: %d\n", WIFI_SSID_1, strlen(WIFI_SSID_1));
        #endif
    #else
        #if DEBUG_SERIAL_WIFI
        if (!silent) Serial.println("connectToWiFi: Secrets Check - WIFI_SSID_1 is not defined by #define.");
        #endif
    #endif
    #if defined(WIFI_PASS_1)
        #if DEBUG_SERIAL_WIFI
        if (!silent) Serial.printf("connectToWiFi: Secrets Check - WIFI_PASS_1 is defined. Length: %d\n", strlen(WIFI_PASS_1));
        #endif
    #else
        #if DEBUG_SERIAL_WIFI
        if (!silent) Serial.println("connectToWiFi: Secrets Check - WIFI_PASS_1 is not defined by #define.");
        #endif
    #endif

    #if defined(WIFI_SSID_1) && defined(WIFI_PASS_1)
      if (strlen(WIFI_SSID_1) > 0 && strlen(WIFI_PASS_1) > 0) {
        if (!silent) { 
            #if DEBUG_SERIAL_WIFI 
            Serial.print("connectToWiFi: Attempting SSID 1: '"); Serial.print(WIFI_SSID_1); Serial.println("'"); 
            #endif
        }
        WiFi.begin(WIFI_SSID_1, WIFI_PASS_1);
        unsigned long startTime = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_CONNECTION_TIMEOUT_MS) {
          delay(500); 
          if (!silent) { #if DEBUG_SERIAL_WIFI Serial.print("."); #endif }
        }
        if (WiFi.status() == WL_CONNECTED) {
          connected = true; connected_ssid = WIFI_SSID_1;
          if (!silent) {#if DEBUG_SERIAL_WIFI Serial.print("\nconnectToWiFi: Connected to SSID 1: "); Serial.println(WIFI_SSID_1); #endif}
        } else {
          if (!silent) { #if DEBUG_SERIAL_WIFI Serial.println("\nconnectToWiFi: Failed to connect to SSID 1."); #endif}
          WiFi.disconnect(true, true); delay(100);
        }
      } else {
         if (!silent) { #if DEBUG_SERIAL_WIFI Serial.println("connectToWiFi: Skipping WiFi 1 - SSID or Password empty in secrets.h (runtime check)."); #endif}
      }
    #else 
        if (!silent) { #if DEBUG_SERIAL_WIFI Serial.println("connectToWiFi: Skipping WiFi 1 - Not defined in secrets.h (compile time check)."); #endif}
    #endif

    #if defined(WIFI_SSID_2) && defined(WIFI_PASS_2)
      if (!connected && strlen(WIFI_SSID_2) > 0 && strlen(WIFI_PASS_2) > 0) {
        if (!silent) { 
            #if DEBUG_SERIAL_WIFI 
            Serial.print("connectToWiFi: Attempting SSID 2: '"); Serial.print(WIFI_SSID_2); Serial.println("'"); 
            #endif
        }
        WiFi.begin(WIFI_SSID_2, WIFI_PASS_2);
        unsigned long startTime = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_CONNECTION_TIMEOUT_MS) {
          delay(500); 
          if (!silent) { #if DEBUG_SERIAL_WIFI Serial.print("."); #endif }
        }
        if (WiFi.status() == WL_CONNECTED) {
          connected = true; connected_ssid = WIFI_SSID_2;
           if (!silent) {#if DEBUG_SERIAL_WIFI Serial.print("\nconnectToWiFi: Connected to SSID 2: "); Serial.println(WIFI_SSID_2); #endif}
        } else {
          if (!silent) { #if DEBUG_SERIAL_WIFI Serial.println("\nconnectToWiFi: Failed to connect to SSID 2."); #endif}
          WiFi.disconnect(true,true); delay(100);
        }
      } else if (!connected && !silent){ // Only print if we didn't connect on 1 and this one is invalid/empty
          #if DEBUG_SERIAL_WIFI 
          if(strlen(WIFI_SSID_2) == 0 || strlen(WIFI_PASS_2) == 0) Serial.println("connectToWiFi: Skipping WiFi 2 - SSID or Password empty (runtime check).");
          #endif
      }
    #elif !connected 
       if (!silent) { #if DEBUG_SERIAL_WIFI Serial.println("connectToWiFi: Skipping WiFi 2 - Not defined in secrets.h (compile time check)."); #endif}
    #endif

    #if DEBUG_SERIAL_WIFI
    if (connected && !silent) Serial.println(); 
    #endif

    if (connected) {
        if (!silent) {
            #if DEBUG_SERIAL_WIFI
            Serial.println("connectToWiFi: WiFi successfully connected!");
            Serial.print("  SSID: "); Serial.println(connected_ssid);
            Serial.print("  IP address: "); Serial.println(WiFi.localIP());
            #endif
        }
        #if defined(MY_GMT_OFFSET_SEC) && defined(MY_DAYLIGHT_OFFSET_SEC)
            if (!silent) { #if DEBUG_SERIAL_GENERAL Serial.println("connectToWiFi: Configuring time via NTP (initial using secrets.h offsets)..."); #endif }
            configTime(MY_GMT_OFFSET_SEC, MY_DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");
            struct tm timeinfo;
            if(!getLocalTime(&timeinfo, 10000)){ 
                if (!silent) { #if DEBUG_SERIAL_GENERAL Serial.println("connectToWiFi: Failed to obtain initial time from NTP."); #endif }
                lastUpdateTimeStr = "Time N/A";
            } else {
                if (!silent) { #if DEBUG_SERIAL_GENERAL Serial.println("connectToWiFi: Initial time configured via NTP."); #endif }
                char timeStrLocal[10]; strftime(timeStrLocal, sizeof(timeStrLocal), "%H:%M", &timeinfo);
                lastUpdateTimeStr = String(timeStrLocal);
                #if DEBUG_SERIAL_GENERAL 
                if (!silent) {char fullTimeBuf[30]; strftime(fullTimeBuf, sizeof(fullTimeBuf), "%Y-%m-%d %H:%M:%S", &timeinfo); Serial.printf("connectToWiFi: Initial local time: %s\n", fullTimeBuf); }
                #endif
            }
        #else 
            lastUpdateTimeStr = "Time UTC?";
            if (!silent) { #if DEBUG_SERIAL_GENERAL Serial.println("connectToWiFi: Timezone offsets (MY_GMT_OFFSET_SEC, MY_DAYLIGHT_OFFSET_SEC) not defined in secrets.h. Time will be UTC initially."); #endif }
            configTime(0, 0, "pool.ntp.org", "time.nist.gov"); 
        #endif
    } else { 
        if (!silent) { #if DEBUG_SERIAL_WIFI Serial.println("\nconnectToWiFi: Could not connect to any configured WiFi network."); #endif }
        lastUpdateTimeStr = "Offline";
    }
}

bool fetchUVData(bool silent) {
  if (WiFi.status() != WL_CONNECTED) {
    #if DEBUG_SERIAL_HTTP
    if (!silent) Serial.println("fetchUVData: No WiFi to fetch data.");
    #endif
    initializeForecastData(); lastUpdateTimeStr = "Offline"; dataJustFetched = true;
    rtc_hasValidData = false; 
    return false;
  }

  HTTPClient http;
  String apiUrl = openMeteoUrl + "?latitude=" + String(deviceLatitude, 4) + 
                  "&longitude=" + String(deviceLongitude, 4) +
                  "&hourly=uv_index&forecast_days=1&timezone=auto"; 

  #if DEBUG_SERIAL_HTTP
  if (!silent) { Serial.println("----------------------------------------"); Serial.print("fetchUVData: GET "); Serial.println(apiUrl); }
  #endif
  http.begin(apiUrl); http.setTimeout(15000);
  int httpCode = http.GET();
  #if DEBUG_SERIAL_HTTP
  if (!silent) { Serial.printf("fetchUVData: HTTP Code: %d\n", httpCode); }
  #endif

  bool data_parsed_successfully = false;
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    #if DEBUG_SERIAL_JSON 
    if (!silent) Serial.printf("fetchUVData: Payload received (first 200 chars): %s\n", payload.substring(0, min(200, (int)payload.length())).c_str() ); 
    #endif
    JsonDocument doc; 
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
      #if DEBUG_SERIAL_JSON
      if (!silent) { Serial.printf("fetchUVData: JSON Err: %s\n", error.c_str());}
      #endif
      initializeForecastData(); rtc_hasValidData = false;
    } else {
      if (doc.containsKey("utc_offset_seconds")) {
        long api_utc_offset_sec = doc["utc_offset_seconds"].as<long>();
        #if DEBUG_SERIAL_GENERAL
        if (!silent) { const char* tz_id = doc["timezone"] | "N/A"; Serial.printf("fetchUVData: API reports UTC Offset: %ld s (%s). Reconfiguring ESP32 time.\n", api_utc_offset_sec, tz_id); }
        #endif
        configTime(api_utc_offset_sec, 0, "pool.ntp.org", "time.nist.gov");
        delay(50); 
      } else {
         #if DEBUG_SERIAL_GENERAL
         if (!silent) { Serial.println("fetchUVData: API response no utc_offset_seconds. Using existing ESP32 time."); }
         #endif
      }

      if (doc.containsKey("hourly") && doc["hourly"].is<JsonObject>() &&
          doc["hourly"].containsKey("time") && doc["hourly"]["time"].is<JsonArray>() &&
          doc["hourly"].containsKey("uv_index") && doc["hourly"]["uv_index"].is<JsonArray>()) {
        JsonArray hourly_time_list = doc["hourly"]["time"].as<JsonArray>();
        JsonArray hourly_uv_list = doc["hourly"]["uv_index"].as<JsonArray>();
        struct tm timeinfo;
        if (!getLocalTime(&timeinfo, 5000)) { 
            #if DEBUG_SERIAL_GENERAL
            if (!silent) { Serial.println("fetchUVData: Failed to get ESP32 local time for forecast matching."); }
            #endif
            initializeForecastData(); rtc_hasValidData = false;
        } else {
            int currentHourLocal = timeinfo.tm_hour;
            #if DEBUG_SERIAL_GENERAL && !silent
            char time_buf[30]; strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
            Serial.printf("fetchUVData: ESP32 Local Time for matching: %s (Hour: %02d)\n", time_buf, currentHourLocal);
            #endif
            int startIndex = -1; 
            for (int k = 0; k < hourly_time_list.size(); ++k) { 
                String api_time_str = hourly_time_list[k].as<String>(); 
                if (api_time_str.length()>=13) { 
                    if (api_time_str.substring(11,13).toInt() >= currentHourLocal) {
                        startIndex=k; 
                        #if DEBUG_SERIAL_GENERAL && !silent
                        Serial.printf("fetchUVData: Forecast match found. StartIndex=%d (API Hour %s >= Local Hour %d)\n", startIndex, api_time_str.substring(11,13).c_str(), currentHourLocal);
                        #endif
                        break;
                    }
                }
            }

            if (startIndex != -1) {
                for (int k=0; k<HOURLY_FORECAST_COUNT; ++k) {  
                    if(startIndex+k < hourly_uv_list.size() && startIndex+k < hourly_time_list.size()){ 
                        JsonVariant uv_json_val = hourly_uv_list[startIndex + k];
                        hourlyUV[k] = uv_json_val.isNull() ? 0.0f : uv_json_val.as<float>(); 
                        if(hourlyUV[k]<0) hourlyUV[k]=0; 
                        forecastHours[k] = hourly_time_list[startIndex+k].as<String>().substring(11,13).toInt();
                    } else {hourlyUV[k]=-1.0f; forecastHours[k]=-1;}
                }
                data_parsed_successfully = true; 
                if (!silent) { #if DEBUG_SERIAL_GENERAL Serial.println("fetchUVData: Successfully populated hourly forecast data arrays."); #endif }
                
                rtc_hasValidData = true; 
                for(int k=0; k<HOURLY_FORECAST_COUNT; ++k) { rtc_hourlyUV[k] = hourlyUV[k]; rtc_forecastHours[k] = forecastHours[k]; }
                rtc_deviceLatitude = deviceLatitude;    
                rtc_deviceLongitude = deviceLongitude;  
            } else { 
                initializeForecastData(); rtc_hasValidData = false; 
                if (!silent) { #if DEBUG_SERIAL_GENERAL Serial.println("fetchUVData: No suitable start index in API forecast for current local hour."); #endif }
            }
        }
      } else { 
          initializeForecastData(); rtc_hasValidData = false; 
          if (!silent) { #if DEBUG_SERIAL_JSON Serial.println("fetchUVData: Hourly data structure (hourly/time/uv_index) missing in JSON."); #endif}
      }
    }
    struct tm timeinfo_update; 
    if(getLocalTime(&timeinfo_update, 1000)){ 
        char ts[10]; strftime(ts,sizeof(ts),"%H:%M",&timeinfo_update); 
        lastUpdateTimeStr=String(ts); 
        lastUpdateTimeStr.toCharArray(rtc_lastUpdateTimeStr, sizeof(rtc_lastUpdateTimeStr)); 
    } else { 
        #if DEBUG_SERIAL_GENERAL
        if(!silent) { Serial.println("fetchUVData: Could not get local time for lastUpdateTimeStr."); }
        #endif
    }
    
    locationDisplayStr.toCharArray(rtc_locationDisplayStr, sizeof(rtc_locationDisplayStr));

  } else { 
    #if DEBUG_SERIAL_HTTP
    if(!silent) { Serial.printf("fetchUVData: HTTP Error %d\n", httpCode); }
    #endif
    initializeForecastData(); rtc_hasValidData = false; 
    if (WiFi.status() != WL_CONNECTED) lastUpdateTimeStr = "Offline"; 
  }
  http.end();
  dataJustFetched = true; 
  return data_parsed_successfully; 
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
        tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK); String s=WiFi.SSID(); if(s.length()>16)s=s.substring(0,13)+"..."; tft.drawString("WiFi:"+s,padding,current_info_y);
    } else { 
        tft.setTextColor(TFT_RED, TFT_BLACK); tft.drawString("WiFi: Offline", padding, current_info_y);
    }
    tft.setTextDatum(TR_DATUM); tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("Upd:" + lastUpdateTimeStr, tft.width() - padding, current_info_y);

    current_info_y += (info_font_height + padding);
    tft.setTextDatum(TL_DATUM); tft.setTextColor(TFT_SKYBLUE, TFT_BLACK);
    String locTextToDisplay = locationDisplayStr; 
    if (locTextToDisplay.length() > 18) locTextToDisplay = locTextToDisplay.substring(0, 18) + ".."; 
    tft.drawString(locTextToDisplay, padding, current_info_y);

    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(isLowPowerModeActive ? TFT_ORANGE : TFT_DARKGREEN, TFT_BLACK);
    tft.drawString(isLowPowerModeActive ? "LPM:ON" : "LPM:OFF", tft.width() - padding, current_info_y);

    top_y_offset = current_info_y + info_font_height + padding; 
  } else { 
    if (WiFi.status() != WL_CONNECTED) { 
        tft.setTextDatum(MC_DATUM); tft.setTextColor(TFT_RED,TFT_BLACK); tft.drawString("! No WiFi Connection !", tft.width()/2, base_top_text_line_y);
        top_y_offset = base_top_text_line_y + info_font_height/2 + padding;
    }
  }
  drawForecastGraph(top_y_offset);
}

void drawForecastGraph(int start_y_offset) {
    int padding = 2;
    int first_uv_val_font = 6; int other_uv_val_font = 4; int hour_label_font = 2;
    tft.setTextFont(first_uv_val_font); int first_uv_text_h = tft.fontHeight();
    tft.setTextFont(other_uv_val_font); int other_uv_text_h = tft.fontHeight();
    tft.setTextFont(hour_label_font);   int hour_label_text_h = tft.fontHeight();

    int first_uv_value_y = start_y_offset + padding + first_uv_text_h / 2;
    int other_uv_values_line_y = first_uv_value_y + (first_uv_text_h / 2) - (other_uv_text_h / 2);
    if (HOURLY_FORECAST_COUNT <= 1) other_uv_values_line_y = first_uv_value_y;
    
    int hour_label_y = tft.height() - padding - hour_label_text_h / 2;
    int graph_baseline_y = hour_label_y - hour_label_text_h / 2 - padding;
    int max_bar_pixel_height = graph_baseline_y - (start_y_offset + padding);
    
    const float MAX_UV_FOR_FULL_SCALE = 10.0f;
    if (max_bar_pixel_height < (int)MAX_UV_FOR_FULL_SCALE) max_bar_pixel_height = (int)MAX_UV_FOR_FULL_SCALE; 
    if (max_bar_pixel_height < 20 && tft.height() > 100) max_bar_pixel_height = 20; 


    float pixel_chunk_per_uv_unit = (MAX_UV_FOR_FULL_SCALE > 0) ? (max_bar_pixel_height / MAX_UV_FOR_FULL_SCALE) : 0;

    int graph_area_total_width = tft.width()-2*padding; int bar_slot_width = graph_area_total_width/HOURLY_FORECAST_COUNT;
    int bar_actual_width = bar_slot_width*0.75; if(bar_actual_width<4)bar_actual_width=4; if(bar_actual_width>30)bar_actual_width=30;
    int graph_area_x_start = (tft.width()-(bar_slot_width*HOURLY_FORECAST_COUNT))/2 + padding;

    #if DEBUG_SERIAL_GRAPH 
        Serial.println("\n--- Graph Drawing Debug ---");
        Serial.printf("start_y_offset: %d, max_bar_pixel_height: %d, chunk_per_uv: %.2f\n", start_y_offset, max_bar_pixel_height, pixel_chunk_per_uv_unit);
    #endif

    for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
        int bar_center_x = graph_area_x_start + (i * bar_slot_width) + (bar_slot_width / 2);
        tft.setTextFont(hour_label_font); tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.setTextDatum(MC_DATUM);
        if (forecastHours[i]!=-1) tft.drawString(String(forecastHours[i]),bar_center_x,hour_label_y); else tft.drawString("-",bar_center_x,hour_label_y);

        float uvVal = hourlyUV[i];
        int roundedUV = (uvVal >= -0.01f) ? round(uvVal) : -1;

        #if DEBUG_SERIAL_GRAPH
        Serial.printf("Bar %d: UVRaw=%.1f, UVRounded=%d", i, uvVal, roundedUV);
        #endif

        if (roundedUV != -1 && forecastHours[i] != -1) {
            float uv_for_height_calc = (float)roundedUV;
            if (uv_for_height_calc < 0) uv_for_height_calc = 0;
            if (uv_for_height_calc > MAX_UV_FOR_FULL_SCALE) uv_for_height_calc = MAX_UV_FOR_FULL_SCALE;
            int bar_height = round(uv_for_height_calc * pixel_chunk_per_uv_unit);
            
            if (uvVal > 0 && uvVal < 0.5f && bar_height == 0) bar_height = 1;
            else if (roundedUV >= 1 && bar_height == 0 && pixel_chunk_per_uv_unit > 0) bar_height = 1; 
            else if (roundedUV >= 1 && bar_height < 2 && pixel_chunk_per_uv_unit >= 2) bar_height = 2; 
            
            if (bar_height > max_bar_pixel_height) bar_height = max_bar_pixel_height;
            if (bar_height < 0) bar_height = 0;
            int bar_top_y = graph_baseline_y - bar_height;

            uint16_t barColor; 
            if(roundedUV<1)barColor=TFT_DARKGREY; else if(roundedUV<=3)barColor=TFT_GREEN; 
            else if(roundedUV<=5)barColor=TFT_YELLOW; else if(roundedUV<=7)barColor=TFT_DARK_ORANGE; 
            else barColor=TFT_RED; 

            if (bar_height > 0) tft.fillRect(bar_center_x - bar_actual_width/2, bar_top_y, bar_actual_width, bar_height, barColor);

            String uvTextStr = String(roundedUV);
            int text_x_pos = bar_center_x; int text_y_pos;
            if (i == 0) { tft.setTextFont(first_uv_val_font); text_y_pos = first_uv_value_y; }
            else { tft.setTextFont(other_uv_val_font); text_y_pos = other_uv_values_line_y; }
            
            tft.setTextDatum(MC_DATUM); 
            
            // Draw outline (black)
            tft.setTextColor(TFT_BLACK); 
            tft.drawString(uvTextStr, text_x_pos - 1, text_y_pos); 
            tft.drawString(uvTextStr, text_x_pos + 1, text_y_pos);
            tft.drawString(uvTextStr, text_x_pos, text_y_pos - 1); 
            tft.drawString(uvTextStr, text_x_pos, text_y_pos + 1);
            // Optional diagonals
            // tft.drawString(uvTextStr, text_x_pos - 1, text_y_pos - 1); tft.drawString(uvTextStr, text_x_pos + 1, text_y_pos - 1);
            // tft.drawString(uvTextStr, text_x_pos - 1, text_y_pos + 1); tft.drawString(uvTextStr, text_x_pos + 1, text_y_pos + 1);

            // Draw main text (white)
            tft.setTextColor(TFT_WHITE); 
            tft.drawString(uvTextStr, text_x_pos, text_y_pos);
            
            #if DEBUG_SERIAL_GRAPH
            Serial.printf(", BarH=%d, Color=0x%X\n", bar_height, barColor);
            #endif
        } else { 
            int ph_y, ph_f; if(i==0){ph_f=first_uv_val_font;ph_y=first_uv_value_y;}else{ph_f=other_uv_val_font;ph_y=other_uv_values_line_y;}
            tft.setTextFont(ph_f); tft.setTextColor(TFT_DARKGREY,TFT_BLACK); tft.setTextDatum(MC_DATUM); tft.drawString("-",bar_center_x,ph_y);
            #if DEBUG_SERIAL_GRAPH
            Serial.println(", --> Placeholder");
            #endif
        }
    }
    #if DEBUG_SERIAL_GRAPH
    Serial.println("--- End of Graph Draw ---");
    #endif
}