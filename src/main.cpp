#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <EEPROM.h> // Added for EEPROM
#include "secrets.h" // Your secrets

// --- Configuration ---
String openMeteoUrl = "https://api.open-meteo.com/v1/forecast";
// const unsigned long UPDATE_INTERVAL_MS_NORMAL_MODE = 15 * 60 * 1000; // OLD - REMOVED
const int WIFI_CONNECTION_TIMEOUT_MS = 15000;
const unsigned long SCREEN_ON_DURATION_LPM_MS = 30 * 1000; // 30 second screen on time in LPM
// const byte SILENT_REFRESH_TARGET_MINUTE = 5; // OLD - REMOVED

// --- New Scheduling Configuration ---
const byte REFRESH_TARGET_MINUTE = 5;         // Base minute of the hour for the first refresh slot (0-59)
const byte UPDATES_PER_HOUR_NORMAL_MODE = 4;  // e.g., 4 for every 15 mins, 2 for every 30 mins, 1 for hourly
const byte UPDATES_PER_HOUR_LPM = 1;          // e.g., 1 for hourly (at REFRESH_TARGET_MINUTE)

// --- EEPROM Configuration ---
#define EEPROM_SIZE 1          // Size for EEPROM (1 byte for LPM flag)
#define LPM_FLAG_EEPROM_ADDR 0 // EEPROM address for LPM flag

// --- Debugging Flags ---
#define DEBUG_LPM 1             // Set to 1 to enable LPM specific logs
#define DEBUG_GRAPH_DRAWING 0   // Set to 1 to enable detailed graph drawing logs, 0 to disable
#define DEBUG_PERSISTENCE 1     // Set to 1 to enable detailed EEPROM/RTC save/load logs
#define DEBUG_SCHEDULING 1      // Set to 1 to enable detailed scheduling logs

// --- Pins ---
#define BUTTON_INFO_PIN 0         // GPIO 0 for info overlay, location toggle, and primary wake from LPM
#define BUTTON_LP_TOGGLE_PIN 35   // GPIO 35 for toggling Low Power Mode
#define TFT_BL_PIN 4              // Backlight pin, defined in platformio.ini as -DTFT_BL=4

// --- Global Variables ---
TFT_eSPI tft = TFT_eSPI();
String lastUpdateTimeStr = "Never";
float deviceLatitude = MY_LATITUDE;
float deviceLongitude = MY_LONGITUDE;
String locationDisplayStr = "Initializing...";
bool useGpsFromSecrets = false;

const int HOURLY_FORECAST_COUNT = 6;
float hourlyUV[HOURLY_FORECAST_COUNT];
int forecastHours[HOURLY_FORECAST_COUNT];

// Display State & Update Control
bool showInfoOverlay = false;
bool force_display_update = true;
bool dataJustFetched = false;
unsigned long lastDataFetchAttemptMs = 0; // Still used for WiFi reconnect timing if disconnected
bool isConnectingToWiFi = false;

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
RTC_DATA_ATTR bool rtc_hasValidData = false;
RTC_DATA_ATTR float rtc_hourlyUV[HOURLY_FORECAST_COUNT];
RTC_DATA_ATTR int rtc_forecastHours[HOURLY_FORECAST_COUNT];
RTC_DATA_ATTR char rtc_lastUpdateTimeStr_char[16];
RTC_DATA_ATTR char rtc_locationDisplayStr_char[32];
RTC_DATA_ATTR float rtc_deviceLatitude = MY_LATITUDE;
RTC_DATA_ATTR float rtc_deviceLongitude = MY_LONGITUDE;
RTC_DATA_ATTR bool rtc_useGpsFromSecretsGlobal = false;
// RTC_DATA_ATTR unsigned long rtc_nextUpdateTimeEpochNormalMode = 0; // Not using RTC for this, will recalc in setup

#define RTC_MAGIC_VALUE 0xDEADBEEF

// --- Global variable for Normal Mode next update time ---
unsigned long nextUpdateEpochNormalMode = 0; // Stores the epoch time for the next scheduled update in normal mode


// --- Function Declarations ---
void turnScreenOn();
void turnScreenOff();
void savePersistentState();
void loadPersistentState();

// struct for scheduling results
struct NextUpdateTimeDetails {
    uint64_t sleepDurationUs;   // For deep sleep (LPM)
    time_t nextUpdateEpoch;     // Epoch time of the next scheduled update
    bool updateNow;             // For normal mode: true if an update is due at the current check
};
NextUpdateTimeDetails calculateNextUpdateTimeDetails(const struct tm& currentTimeInfo, byte updatesPerHour, byte targetStartMinute, bool isNormalModeCheck);
// uint64_t calculateSleepUntilNextTargetMinute(); // OLD - REMOVED
void enterDeepSleep(uint64_t duration_us, bool alsoEnableButtonWake);
void printWakeupReason();

void initializeForecastData(bool updateRTC = false);
void connectToWiFi(bool silent);
bool fetchLocationFromIp(bool silent);
bool fetchUVData(bool silent); // Returns true on successful data parse, false otherwise

void displayMessage(String msg_line1, String msg_line2 = "", int color = TFT_WHITE, bool allowDisplay = true);
void displayInfo();
void drawForecastGraph(int start_y_offset);
void handle_buttons();
void performDataFetchSequence(bool silent); // Helper for common fetch logic

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

// --- EEPROM & RTC Memory Functions ---
void savePersistentState() {
    #if DEBUG_PERSISTENCE
    Serial.println("PERSISTENCE SAVE: Attempting to save state...");
    Serial.printf("PERSISTENCE SAVE: Saving isLowPowerModeActive = %s to EEPROM Addr %d\n", isLowPowerModeActive ? "true" : "false", LPM_FLAG_EEPROM_ADDR);
    #endif
    EEPROM.write(LPM_FLAG_EEPROM_ADDR, isLowPowerModeActive ? 1 : 0);
    if (EEPROM.commit()) {
        #if DEBUG_PERSISTENCE
        Serial.println("PERSISTENCE SAVE: EEPROM commit successful for LPM flag.");
        #endif
    } else {
        #if DEBUG_PERSISTENCE
        Serial.println("PERSISTENCE SAVE: EEPROM commit FAILED for LPM flag.");
        #endif
    }

    rtc_magic_cookie = RTC_MAGIC_VALUE;
    rtc_useGpsFromSecretsGlobal = useGpsFromSecrets;

    if (rtc_hasValidData) { // Only save valid data to RTC
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
    #if DEBUG_PERSISTENCE
    Serial.printf("PERSISTENCE SAVE (RTC part): HasValidData: %s, UseGPSSecrets: %s\n", rtc_hasValidData ? "Yes" : "No", rtc_useGpsFromSecretsGlobal ? "Yes" : "No");
    #endif
}

void loadPersistentState() {
    #if DEBUG_PERSISTENCE
    Serial.println("PERSISTENCE LOAD: Attempting to load state...");
    #endif

    byte lpmFlagFromEEPROM = EEPROM.read(LPM_FLAG_EEPROM_ADDR);
    if (lpmFlagFromEEPROM == 0 || lpmFlagFromEEPROM == 1) { // Check if EEPROM has a valid boolean
        isLowPowerModeActive = (bool)lpmFlagFromEEPROM;
        #if DEBUG_PERSISTENCE
        Serial.printf("PERSISTENCE LOAD: Loaded isLowPowerModeActive = %s from EEPROM.\n", isLowPowerModeActive ? "true" : "false");
        #endif
    } else { // EEPROM uninitialized or corrupted for this flag
        #if DEBUG_PERSISTENCE
        Serial.printf("PERSISTENCE LOAD: EEPROM LPM flag uninitialized (read: 0x%X). Defaulting to LPM OFF.\n", lpmFlagFromEEPROM);
        #endif
        isLowPowerModeActive = false; // Default to normal mode
        EEPROM.write(LPM_FLAG_EEPROM_ADDR, 0); // Initialize EEPROM
        EEPROM.commit();
    }
    
    if (rtc_magic_cookie == RTC_MAGIC_VALUE) {
        useGpsFromSecrets = rtc_useGpsFromSecretsGlobal;
        if (rtc_hasValidData) { // Check if RTC data was marked as valid before sleep
            for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
                hourlyUV[i] = rtc_hourlyUV[i];
                forecastHours[i] = rtc_forecastHours[i];
            }
            lastUpdateTimeStr = String(rtc_lastUpdateTimeStr_char);
            locationDisplayStr = String(rtc_locationDisplayStr_char);
            deviceLatitude = rtc_deviceLatitude;
            deviceLongitude = rtc_deviceLongitude;
            dataJustFetched = true; // Mark that we have data (from RTC) to display
            #if DEBUG_PERSISTENCE
            Serial.println("PERSISTENCE LOAD: Valid data loaded from RTC.");
            #endif
        } else {
            initializeForecastData(); // Initialize working data to defaults if RTC was not valid
            #if DEBUG_PERSISTENCE
            Serial.println("PERSISTENCE LOAD: RTC data marked as not valid, initialized working data to defaults.");
            #endif
        }
    } else { // RTC magic cookie mismatch (e.g., first boot, power loss without proper shutdown)
        #if DEBUG_PERSISTENCE
        Serial.println("PERSISTENCE LOAD: RTC magic cookie mismatch. Initializing RTC data to defaults.");
        #endif
        useGpsFromSecrets = false; // Default GPS preference
        initializeForecastData(true); // Initialize working data AND RTC data to defaults
        rtc_hasValidData = false;     // Mark RTC data as not valid
        strncpy(rtc_lastUpdateTimeStr_char, "Never", sizeof(rtc_lastUpdateTimeStr_char) -1);
        rtc_lastUpdateTimeStr_char[sizeof(rtc_lastUpdateTimeStr_char)-1] = '\0';
        lastUpdateTimeStr = "Never";
        strncpy(rtc_locationDisplayStr_char, "Initializing...", sizeof(rtc_locationDisplayStr_char)-1);
        rtc_locationDisplayStr_char[sizeof(rtc_locationDisplayStr_char)-1] = '\0';
        locationDisplayStr = "Initializing...";
        rtc_deviceLatitude = MY_LATITUDE; // Default to secrets
        rtc_deviceLongitude = MY_LONGITUDE;
        deviceLatitude = MY_LATITUDE;
        deviceLongitude = MY_LONGITUDE;
        rtc_magic_cookie = RTC_MAGIC_VALUE; // Set magic cookie for next time
    }
    #if DEBUG_LPM
    Serial.printf("Loaded Persistent State: LPM Active: %s, UseGPSSecrets: %s, RTCValidData: %s\n",
                  isLowPowerModeActive ? "Yes" : "No",
                  useGpsFromSecrets ? "Yes" : "No",
                  rtc_hasValidData ? "Yes" : "No");
    #endif
}


// --- Scheduling and Deep Sleep Functions ---
NextUpdateTimeDetails calculateNextUpdateTimeDetails(const struct tm& currentTimeInfo, byte updatesPerHour, byte targetStartMinute, bool isNormalModeCheck) {
    NextUpdateTimeDetails result;
    result.sleepDurationUs = 15 * 60 * 1000000ULL; // Default to 15 min sleep as a fallback
    result.nextUpdateEpoch = 0;
    result.updateNow = false;

    time_t nowEpoch = mktime(const_cast<struct tm*>(&currentTimeInfo));

    if (updatesPerHour == 0 || updatesPerHour > 60) {
        #if DEBUG_SCHEDULING
        Serial.printf("SCHED ERR: Invalid updatesPerHour (%d). Defaulting to 1.\n", updatesPerHour);
        #endif
        updatesPerHour = 1; 
    }
    if (targetStartMinute >= 60) {
        #if DEBUG_SCHEDULING
        Serial.printf("SCHED ERR: Invalid targetStartMinute (%d). Defaulting to 0.\n", targetStartMinute);
        #endif
        targetStartMinute = 0;
    }

    int intervalMinutes = 60 / updatesPerHour;
    time_t foundNextEpoch = 0;

    // Iterate for up to 2 hours ahead to find the next slot
    for (int h_offset = 0; h_offset < 2; ++h_offset) {
        for (int i = 0; i < updatesPerHour; ++i) {
            struct tm candidateTimeStruct = currentTimeInfo; // Base for date part
            candidateTimeStruct.tm_hour = currentTimeInfo.tm_hour + h_offset; // Add hour offset
            candidateTimeStruct.tm_min = targetStartMinute + (i * intervalMinutes);
            candidateTimeStruct.tm_sec = 0;

            // mktime normalizes the date/time (handles hour > 23, day/month/year rollovers)
            time_t candidateEpoch = mktime(&candidateTimeStruct);

            if (candidateEpoch > nowEpoch) {
                if (foundNextEpoch == 0 || candidateEpoch < foundNextEpoch) {
                    foundNextEpoch = candidateEpoch;
                }
            } else if (isNormalModeCheck && (candidateEpoch == nowEpoch || (nowEpoch - candidateEpoch < intervalMinutes * 60 && nowEpoch - candidateEpoch < 30 ) )) {
                // If it's normal mode and the slot is *now* or very recently passed (within a safety margin, e.g. few seconds, or less than current interval and less than 30s)
                // This helps catch slots if loop execution took a moment.
                result.updateNow = true;
                // We still need to find the *next* slot for scheduling *after* this immediate update.
                // The loop will continue, and `foundNextEpoch` will get the next valid future slot.
            }
        }
        // If we found a future slot, and we are not in the 'updateNow' scenario, this is our target.
        if (foundNextEpoch != 0 && !result.updateNow) {
            break;
        }
        // If 'updateNow' is true, we need to ensure `foundNextEpoch` is indeed for the slot *after* the current one.
        // The loop structure should handle this by continuing to search for the *next* `candidateEpoch > nowEpoch`.
        if (foundNextEpoch != 0 && result.updateNow && foundNextEpoch > nowEpoch) {
            break; 
        }
    }

    if (result.updateNow && isNormalModeCheck) {
        // If updateNow is true, `foundNextEpoch` should be the one *after* the current event.
        // If `foundNextEpoch` is still 0 or not greater than `nowEpoch` after the loops,
        // it means the 'updateNow' slot was the last one in our 2-hour search window.
        // We need to calculate one more step ahead for the next schedule.
        if (foundNextEpoch == 0 || foundNextEpoch <= nowEpoch) {
            struct tm nextSlotTimeCalc = currentTimeInfo;
            // Advance by at least one interval from current time to find the next logical point
            time_t tempNowPlusInterval = nowEpoch + (intervalMinutes * 60);
            nextSlotTimeCalc = *localtime(&tempNowPlusInterval); 
            
            bool slotFoundForNext = false;
            for(int h_calc = 0; h_calc < 2; ++h_calc) { // Search again starting from advanced time
                for (int i_calc = 0; i_calc < updatesPerHour; ++i_calc) {
                    struct tm tempCalc = currentTimeInfo; // Base for date
                    tempCalc.tm_hour = nextSlotTimeCalc.tm_hour + h_calc; // Use hour from advanced time + offset
                    tempCalc.tm_min = targetStartMinute + (i_calc * intervalMinutes);
                    tempCalc.tm_sec = 0;
                    time_t calcEpoch = mktime(&tempCalc);
                    if (calcEpoch > nowEpoch) { // Must be strictly after current time
                         if (foundNextEpoch == 0 || calcEpoch < foundNextEpoch || (foundNextEpoch <= nowEpoch && calcEpoch > foundNextEpoch) ){
                            foundNextEpoch = calcEpoch;
                            slotFoundForNext = true;
                         }
                    }
                }
                if(slotFoundForNext && (foundNextEpoch > nowEpoch)) break;
            }
             if (!slotFoundForNext || foundNextEpoch <= nowEpoch) { // Absolute fallback if still no future slot
                struct tm fallbackNext = currentTimeInfo;
                fallbackNext.tm_hour +=1;
                fallbackNext.tm_min = targetStartMinute;
                fallbackNext.tm_sec = 0;
                foundNextEpoch = mktime(&fallbackNext);
             }
        }
        result.nextUpdateEpoch = foundNextEpoch;
        result.sleepDurationUs = 0; // Not used for sleep if updateNow is true for Normal Mode
    } else if (foundNextEpoch != 0) {
        result.nextUpdateEpoch = foundNextEpoch;
        result.sleepDurationUs = (uint64_t)difftime(result.nextUpdateEpoch, nowEpoch) * 1000000ULL;
    } else {
        // Fallback: No future slot found in the 2-hour window. Schedule for targetStartMinute of the next hour.
        struct tm fallbackTime = currentTimeInfo;
        fallbackTime.tm_hour += 1;
        fallbackTime.tm_min = targetStartMinute;
        fallbackTime.tm_sec = 0;
        result.nextUpdateEpoch = mktime(&fallbackTime);
        if (result.nextUpdateEpoch <= nowEpoch) { // If next hour's target is still not in future (e.g. current time is 23:59, target is 00:05)
            fallbackTime.tm_hour +=1; // Advance one more hour
            result.nextUpdateEpoch = mktime(&fallbackTime);
        }
        result.sleepDurationUs = (uint64_t)difftime(result.nextUpdateEpoch, nowEpoch) * 1000000ULL;
        #if DEBUG_SCHEDULING
        Serial.println("SCHED WARN: No specific update slot found in 2h search, defaulting to next available target minute.");
        #endif
    }
    
    // Sanity check sleep duration
    if (result.sleepDurationUs == 0 && !result.updateNow) {
        #if DEBUG_SCHEDULING
        Serial.println("SCHED ERR: Calculated sleep duration is 0 when not updating now. Defaulting to interval.");
        #endif
        result.sleepDurationUs = (uint64_t)intervalMinutes * 60 * 1000000ULL;
        if(result.sleepDurationUs == 0 && updatesPerHour > 0) result.sleepDurationUs = 60 * 1000000ULL; // Min 1 minute if interval was calculated as 0
        else if(result.sleepDurationUs == 0) result.sleepDurationUs = 60 * 60 * 1000000ULL; // Absolute fallback 1 hour
        
        result.nextUpdateEpoch = nowEpoch + (result.sleepDurationUs / 1000000ULL);
    }
    
    // ESP32 max timer sleep is high with uint64_t, but practically, cap it.
    uint64_t practicalMaxSleepUs = 3LL * 60 * 60 * 1000000; // 3 hours
    if (!isNormalModeCheck && result.sleepDurationUs > practicalMaxSleepUs) { // Only cap for LPM sleep
        #if DEBUG_SCHEDULING
        Serial.printf("SCHED WARN: LPM Sleep duration %llu us too long. Capping to ~%d min.\n", result.sleepDurationUs, intervalMinutes > 0 ? intervalMinutes : 60);
        #endif
        result.sleepDurationUs = (uint64_t)(intervalMinutes > 0 ? intervalMinutes : 60) * 60 * 1000000ULL;
        if(result.sleepDurationUs == 0 || result.sleepDurationUs > practicalMaxSleepUs) result.sleepDurationUs = 60*60*1000000ULL; // Fallback 1h
        result.nextUpdateEpoch = nowEpoch + (result.sleepDurationUs / 1000000ULL);
    }

    #if DEBUG_SCHEDULING
        char timeBuff[30], nextTimeBuff[30];
        strftime(timeBuff, sizeof(timeBuff), "%F %T", &currentTimeInfo);
        time_t net = result.nextUpdateEpoch;
        strftime(nextTimeBuff, sizeof(nextTimeBuff), "%F %T", localtime(&net));
        Serial.printf("SCHED: Now: %s, TargetStartMin: %d, Updates/Hr: %d, isNormalChk: %d\n", timeBuff, targetStartMinute, updatesPerHour, isNormalModeCheck);
        Serial.printf("SCHED: Result: updateNow: %s, nextEpoch: %lu (%s), sleepUs: %llu (%.2f min)\n",
                      result.updateNow ? "Y" : "N", (unsigned long)result.nextUpdateEpoch, nextTimeBuff, result.sleepDurationUs, (double)result.sleepDurationUs / 60000000.0);
    #endif
    return result;
}


void enterDeepSleep(uint64_t duration_us, bool alsoEnableButtonWake) {
    savePersistentState();
    turnScreenOff();
    Serial.printf("Entering deep sleep for %llu us (approx %.2f minutes).\n", duration_us, (double)duration_us / 1000000.0 / 60.0);
    esp_sleep_enable_timer_wakeup(duration_us);
    if (alsoEnableButtonWake) {
        Serial.println("Enabling GPIO0 (BUTTON_INFO_PIN) for wake-up from deep sleep (falling edge).");
        esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0); // Wake on LOW level for GPIO0
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
        hourlyUV[i] = -1.0f; // Use -1 to indicate no data / placeholder
        forecastHours[i] = -1;
        if (updateRTC) {
            rtc_hourlyUV[i] = -1.0f;
            rtc_forecastHours[i] = -1;
        }
    }
    // rtc_hasValidData should be set to false elsewhere if data is truly invalid after this
}

// Helper function for the data fetching sequence
void performDataFetchSequence(bool silent) {
    if (!silent) displayMessage("Connecting to WiFi...", "", TFT_YELLOW, true);
    connectToWiFi(silent);

    if (WiFi.status() == WL_CONNECTED) {
        if (useGpsFromSecrets) {
            deviceLatitude = MY_LATITUDE; deviceLongitude = MY_LONGITUDE;
            locationDisplayStr = "Secrets GPS";
            if (!silent) Serial.println("Using GPS coordinates from secrets.h");
        } else {
            if (!silent) displayMessage("Fetching IP Location...", "", TFT_SKYBLUE, true);
            if (!fetchLocationFromIp(silent)) { // fetchLocationFromIp updates globals on success
                // Fallback to secrets if IP geo fails
                useGpsFromSecrets = true; // Persist this choice
                deviceLatitude = MY_LATITUDE; deviceLongitude = MY_LONGITUDE;
                locationDisplayStr = "IP Fail>Secrets";
                if (!silent) Serial.println("IP Geolocation failed. Falling back to secrets.h GPS.");
            }
        }
        String currentStatusForDisplay = locationDisplayStr;
        if (currentStatusForDisplay.length() > 18) currentStatusForDisplay = currentStatusForDisplay.substring(0,15) + "...";

        if (!silent) displayMessage("Fetching UV data...", currentStatusForDisplay, TFT_CYAN, true);
        if (fetchUVData(silent)) { // fetchUVData updates globals and rtc_hasValidData
            if (!isLowPowerModeActive) lastDataFetchAttemptMs = millis(); // Reset for normal mode WiFi check interval if needed
        } else {
            if (!silent) Serial.println("UV Data fetch failed.");
            // lastUpdateTimeStr and forecast data are handled within fetchUVData on failure
        }
    } else { // WiFi not connected
        locationDisplayStr = "Offline>Secrets"; // Default location display
        useGpsFromSecrets = true; // Default to using secrets if offline
        deviceLatitude = MY_LATITUDE; deviceLongitude = MY_LONGITUDE;
        initializeForecastData(true); // Initialize data, update RTC to show no data
        lastUpdateTimeStr = "Offline";
        strncpy(rtc_lastUpdateTimeStr_char, "Offline", sizeof(rtc_lastUpdateTimeStr_char)-1);
        rtc_lastUpdateTimeStr_char[sizeof(rtc_lastUpdateTimeStr_char)-1] = '\0';
        rtc_hasValidData = false;
        dataJustFetched = true; // To trigger display update
        if (!silent) Serial.println("WiFi not connected. Cannot fetch data. Using offline defaults.");
    }
    force_display_update = true;
    if (WiFi.status() == WL_CONNECTED || useGpsFromSecrets) { // Save state if we used secrets or got new IP
      savePersistentState(); // Save new GPS pref (if changed by IP fail) and any fetched RTC data
    }
}


// --- Setup ---
void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 2000); // Wait for serial, but not indefinitely
    Serial.println("\nUV Index Monitor Starting Up...");

    EEPROM.begin(EEPROM_SIZE);

    pinMode(BUTTON_INFO_PIN, INPUT_PULLUP);
    pinMode(BUTTON_LP_TOGGLE_PIN, INPUT_PULLUP);
    pinMode(TFT_BL_PIN, OUTPUT);

    printWakeupReason();
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    
    loadPersistentState(); // Load LPM state, GPS preference, and last known data from RTC/EEPROM
    #if DEBUG_PERSISTENCE
    Serial.printf("SETUP: After loadPersistentState(), isLowPowerModeActive = %s\n", isLowPowerModeActive ? "true" : "false");
    #endif

    tft.init();
    tft.setRotation(1); // Landscape: USB top left, screen right way up
    tft.setTextDatum(MC_DATUM); // Middle Center datum

    bool performInitialActionsOnPowerOn = (wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED); // True for power-on reset

    if (performInitialActionsOnPowerOn) {
        #if DEBUG_LPM || DEBUG_SCHEDULING
        Serial.println("SETUP: Power-on reset detected. Performing initial data fetch sequence.");
        #endif
        turnScreenOn();
        tft.fillScreen(TFT_BLACK);
        performDataFetchSequence(false); // Fetch data non-silently
    }

    // After initial setup/load, decide operational mode and next steps
    if (isLowPowerModeActive) {
        if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) { // LPM Timer Wake-up (Silent Refresh)
            #if DEBUG_LPM || DEBUG_SCHEDULING
            Serial.println("LPM: Timer Wake-up. Silent refresh cycle.");
            #endif
            temporaryScreenWakeupActive = false;
            turnScreenOff(); // Ensure screen is off for silent refresh
            performDataFetchSequence(true); // Connect to WiFi and fetch UV data silently

            struct tm timeinfo;
            if(!getLocalTime(&timeinfo, 10000)){
                Serial.println("LPM FATAL: Failed to obtain time for next sleep calculation after timer wake!");
                enterDeepSleep(15 * 60 * 1000000ULL, true); // Fallback sleep 15 mins
            } else {
                NextUpdateTimeDetails details = calculateNextUpdateTimeDetails(timeinfo, UPDATES_PER_HOUR_LPM, REFRESH_TARGET_MINUTE, false);
                enterDeepSleep(details.sleepDurationUs, true);
            }
        } else if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) { // LPM Button Wake-up (GPIO 0)
            #if DEBUG_LPM
            Serial.println("LPM: Button Wake-up (GPIO 0). Temporary screen on.");
            #endif
            temporaryScreenWakeupActive = true;
            screenActiveUntilMs = millis() + SCREEN_ON_DURATION_LPM_MS;
            turnScreenOn();
            tft.fillScreen(TFT_BLACK);
            if (rtc_hasValidData) force_display_update = true; // Display data loaded from RTC
            else displayMessage("LPM: No data yet", "Update pending", TFT_YELLOW, true); // Or if RTC had no valid data
        } else { // Includes power-on reset if LPM was active (performInitialActionsOnPowerOn already ran)
            #if DEBUG_LPM || DEBUG_SCHEDULING
            Serial.println("LPM: EEPROM indicated LPM active. Entering LPM cycle.");
            Serial.printf("LPM: Wakeup reason was %d. If power-on, initial fetch already done.\n", wakeup_reason);
            #endif
            temporaryScreenWakeupActive = false;
            
            if (performInitialActionsOnPowerOn) { // If it was a power-on reset and LPM is active
                 displayMessage("LPM Resuming", "Sleeping...", TFT_BLUE, true);
                 delay(2000); // Brief message
            } else if (wakeup_reason != ESP_SLEEP_WAKEUP_EXT0) { // If not button wake (e.g. toggled to LPM)
                turnScreenOn(); // Momentarily turn on screen to show message
                displayMessage("LPM Active", "Initializing sleep...", TFT_BLUE, true);
                delay(1500);
            }
            // For all these cases leading to LPM sleep (except EXT0 wake handled by loop):
            struct tm timeinfo;
            if(!getLocalTime(&timeinfo, 10000)){ // Need time to calculate next sleep
                Serial.println("LPM FATAL: Failed to obtain time for initial LPM sleep calculation!");
                enterDeepSleep(15 * 60 * 1000000ULL, true); // Fallback sleep 15 mins
            } else {
                NextUpdateTimeDetails details = calculateNextUpdateTimeDetails(timeinfo, UPDATES_PER_HOUR_LPM, REFRESH_TARGET_MINUTE, false);
                enterDeepSleep(details.sleepDurationUs, true);
            }
        }
    } else { // Normal Mode (isLowPowerModeActive is false)
        temporaryScreenWakeupActive = false; // Not relevant in normal mode
        turnScreenOn(); // Ensure screen is on

        if (!performInitialActionsOnPowerOn) { // If not a power-on reset (e.g., LPM was just turned off)
            #if DEBUG_LPM || DEBUG_SCHEDULING
            Serial.println("Normal Mode Startup (not power-on reset, e.g., LPM turned off by button).");
            #endif
            // Data might be stale from RTC or LPM was just turned off.
            // The button handler for turning LPM OFF already does a fetch.
            // If we get here by other means (e.g. EEPROM was LPM=false on a non-power-on reset but not EXT0/Timer)
            // ensure data is current or shown.
            if (rtc_hasValidData) force_display_update = true; // Show RTC data first
            // An immediate fetch might be desired if data is old, handled by scheduler next.
        }
        // If performInitialActionsOnPowerOn was true, screen is on and data fetched already.

        // Schedule the first update for Normal Mode
        struct tm timeinfo;
        if(!getLocalTime(&timeinfo, 10000)){
            Serial.println("Normal Mode FATAL: Failed to obtain time for initial scheduling!");
            // Potentially try to fetch data once and then rely on simpler interval? Or retry time.
            // For now, this will mean scheduled updates won't work until time is obtained.
            nextUpdateEpochNormalMode = 0; // Indicate scheduler isn't ready
        } else {
            NextUpdateTimeDetails details = calculateNextUpdateTimeDetails(timeinfo, UPDATES_PER_HOUR_NORMAL_MODE, REFRESH_TARGET_MINUTE, true);
            if (details.updateNow) {
                #if DEBUG_SCHEDULING
                Serial.println("Normal Mode: Initial schedule check indicates UPDATE NOW.");
                #endif
                if (!performInitialActionsOnPowerOn) { // If not already fetched by power-on sequence
                    performDataFetchSequence(false);
                }
            }
            nextUpdateEpochNormalMode = details.nextUpdateEpoch; // Schedule the next one
        }
        if (force_display_update == false && (rtc_hasValidData || performInitialActionsOnPowerOn) ) force_display_update = true;
    }
}


// --- Main Loop ---
void loop() {
    handle_buttons(); // Check for button presses first

    if (isLowPowerModeActive) {
        if (temporaryScreenWakeupActive) {
            if (millis() >= screenActiveUntilMs) {
                #if DEBUG_LPM
                Serial.println("LPM: Screen on-time expired. Going back to deep sleep.");
                #endif
                temporaryScreenWakeupActive = false;
                // Screen will turn off in enterDeepSleep
                struct tm timeinfo;
                if(!getLocalTime(&timeinfo, 10000)){
                     Serial.println("LPM FATAL: Failed to obtain time for sleep after screen timeout!");
                     enterDeepSleep(15 * 60 * 1000000ULL, true); // Fallback
                } else {
                    NextUpdateTimeDetails details = calculateNextUpdateTimeDetails(timeinfo, UPDATES_PER_HOUR_LPM, REFRESH_TARGET_MINUTE, false);
                    enterDeepSleep(details.sleepDurationUs, true);
                }
            }
            // else: screen is on, just loop and wait for timeout or another button press
        } else {
            // This state should ideally not be reached if LPM is active and not temporarily woken,
            // as setup() or other handlers should have put it to sleep.
            // This is a fallback.
            #if DEBUG_LPM
            Serial.println("LPM: Active, screen off. Fallback to deep sleep from loop.");
            #endif
            struct tm timeinfo;
            if(!getLocalTime(&timeinfo, 10000)){
                 Serial.println("LPM FATAL: Failed to obtain time for fallback sleep in loop!");
                 enterDeepSleep(15 * 60 * 1000000ULL, true); // Fallback
            } else {
                NextUpdateTimeDetails details = calculateNextUpdateTimeDetails(timeinfo, UPDATES_PER_HOUR_LPM, REFRESH_TARGET_MINUTE, false);
                enterDeepSleep(details.sleepDurationUs, true);
            }
        }
    } else { // Normal Mode
        unsigned long currentMillis = millis(); // For WiFi reconnect logic primarily now

        // Check for scheduled update
        if (WiFi.status() == WL_CONNECTED && nextUpdateEpochNormalMode > 0) { // Ensure scheduler is ready
            struct tm timeinfo;
            if (!getLocalTime(&timeinfo, 1000)) { // Quick check for current time
                #if DEBUG_SCHEDULING
                Serial.println("Loop Normal: Failed to get time for update check.");
                #endif
            } else {
                time_t nowEpoch = mktime(&timeinfo);
                if (nowEpoch >= nextUpdateEpochNormalMode) {
                    #if DEBUG_SCHEDULING
                    Serial.println("Normal Mode: Scheduled update time reached.");
                    #endif
                    performDataFetchSequence(false); // Perform the data fetch

                    // Reschedule the next update
                    if (!getLocalTime(&timeinfo, 5000)) { // Get fresh time after fetch
                        Serial.println("Normal Mode ERR: Failed to get time for rescheduling!");
                        nextUpdateEpochNormalMode = nowEpoch + ( (60 / UPDATES_PER_HOUR_NORMAL_MODE) * 60 ); // Fallback schedule
                    } else {
                        NextUpdateTimeDetails details = calculateNextUpdateTimeDetails(timeinfo, UPDATES_PER_HOUR_NORMAL_MODE, REFRESH_TARGET_MINUTE, true);
                        // If details.updateNow is true here, it means the calculation thinks we should update again *immediately*.
                        // This can happen if fetch took exactly up to the next slot.
                        // `nextUpdateEpoch` from `details` should already be the *next distinct future slot*.
                        nextUpdateEpochNormalMode = details.nextUpdateEpoch;
                         #if DEBUG_SCHEDULING
                         if(details.updateNow) Serial.println("Normal Mode: Rescheduler also indicated updateNow. Next slot set.");
                         #endif
                    }
                }
            }
        } else if (WiFi.status() == WL_CONNECTED && nextUpdateEpochNormalMode == 0) {
            // Scheduler not initialized, try to initialize it (e.g. if time failed in setup)
            struct tm timeinfo;
            if(getLocalTime(&timeinfo, 5000)){
                NextUpdateTimeDetails details = calculateNextUpdateTimeDetails(timeinfo, UPDATES_PER_HOUR_NORMAL_MODE, REFRESH_TARGET_MINUTE, true);
                if(details.updateNow) performDataFetchSequence(false);
                nextUpdateEpochNormalMode = details.nextUpdateEpoch;
            }
        }


        // WiFi reconnection logic (if disconnected) - keep this independent of scheduled data updates
        if (WiFi.status() != WL_CONNECTED && (currentMillis - lastDataFetchAttemptMs >= 60000)) { // Try reconnect every 60s
             Serial.println("Normal Mode: No WiFi. Attempting reconnect...");
             connectToWiFi(false); // This is a blocking call with its own timeout
             if(WiFi.status() == WL_CONNECTED) {
                Serial.println("Normal Mode: WiFi reconnected. Will fetch at next scheduled time or if initial fetch needed.");
                lastDataFetchAttemptMs = currentMillis; // Reset timer for this check
                // If scheduler wasn't ready, try to set it up now that we have WiFi (and thus NTP)
                if (nextUpdateEpochNormalMode == 0) {
                    struct tm timeinfo;
                    if(getLocalTime(&timeinfo, 5000)){
                        NextUpdateTimeDetails details = calculateNextUpdateTimeDetails(timeinfo, UPDATES_PER_HOUR_NORMAL_MODE, REFRESH_TARGET_MINUTE, true);
                        if(details.updateNow) performDataFetchSequence(false); // Fetch if due
                        nextUpdateEpochNormalMode = details.nextUpdateEpoch;
                    }
                } else { // WiFi reconnected, ensure an update happens if one was missed or due
                    struct tm timeinfo;
                    if(getLocalTime(&timeinfo, 1000)) {
                        if (mktime(&timeinfo) >= nextUpdateEpochNormalMode) {
                             Serial.println("Normal Mode: WiFi reconnected and update is due/overdue. Fetching now.");
                             performDataFetchSequence(false);
                             // Reschedule
                             if(getLocalTime(&timeinfo, 5000)) {
                                NextUpdateTimeDetails details = calculateNextUpdateTimeDetails(timeinfo, UPDATES_PER_HOUR_NORMAL_MODE, REFRESH_TARGET_MINUTE, true);
                                nextUpdateEpochNormalMode = details.nextUpdateEpoch;
                             }
                        }
                    }
                }
             } else {
                lastDataFetchAttemptMs = currentMillis; // Mark attempt, try again later
                // Display will show offline status
                if (!lastUpdateTimeStr.equals("Offline")) { // Update display only if status changed
                    lastUpdateTimeStr = "Offline";
                    initializeForecastData(true); // Ensures display shows no data, RTC reflects this
                    dataJustFetched = true; // Trigger display update
                }
             }
        }
    }

    // Display update logic (common to both modes if screen is on)
    if (force_display_update || dataJustFetched) {
        if (!isLowPowerModeActive || temporaryScreenWakeupActive) { // Only display if not in LPM deep sleep state
            displayInfo();
        }
        force_display_update = false;
        dataJustFetched = false;
    }
    delay(50); // Small delay to yield to other tasks, prevent busy-looping if no other delays
}

// --- Consolidated Button Handling ---
void handle_buttons() {
    unsigned long current_millis = millis();

    // BUTTON_INFO_PIN (GPIO 0) - Info Overlay / Location Toggle
    bool current_info_button_state = digitalRead(BUTTON_INFO_PIN);
    if (current_info_button_state == LOW && button_info_last_state == HIGH && (current_millis - button_info_last_press_time > DEBOUNCE_TIME_MS)) {
        button_info_press_start_time = current_millis;
        button_info_is_held = false;
    } else if (current_info_button_state == LOW && !button_info_is_held) { // Button is being held
        if (current_millis - button_info_press_start_time > LONG_PRESS_TIME_MS) { // Long press detected
            if (showInfoOverlay) { // Long press action only if info overlay is currently shown
                useGpsFromSecrets = !useGpsFromSecrets;
                Serial.printf("Location Mode Toggled (Long Press): %s\n", useGpsFromSecrets ? "Secrets GPS" : "IP Geolocation");
                
                // Perform data fetch sequence with new location preference
                performDataFetchSequence(false); // Non-silent fetch

                savePersistentState(); // Save the new GPS preference

                if (isLowPowerModeActive && temporaryScreenWakeupActive) {
                    screenActiveUntilMs = current_millis + SCREEN_ON_DURATION_LPM_MS; // Extend screen on time
                }
                force_display_update = true; // Ensure display updates with new data/status
            }
            button_info_is_held = true; // Mark as handled to prevent repeated long press actions
            button_info_last_press_time = current_millis; // Update last press time for debouncing next release
        }
    } else if (current_info_button_state == HIGH && button_info_last_state == LOW && (current_millis - button_info_last_press_time > DEBOUNCE_TIME_MS)) { // Button released
        if (!button_info_is_held) { // Short press action
            showInfoOverlay = !showInfoOverlay;
            Serial.printf("Info Button Short Press, showInfoOverlay: %s\n", showInfoOverlay ? "true" : "false");
            if (isLowPowerModeActive && temporaryScreenWakeupActive) {
                screenActiveUntilMs = current_millis + SCREEN_ON_DURATION_LPM_MS; // Extend screen on time
            }
            force_display_update = true; // Update display to show/hide overlay
        }
        button_info_last_press_time = current_millis; // Debounce for next press
        button_info_is_held = false; // Reset held state
    }
    button_info_last_state = current_info_button_state;


    // BUTTON_LP_TOGGLE_PIN (GPIO 35) - Low Power Mode Toggle
    bool current_lp_button_state = digitalRead(BUTTON_LP_TOGGLE_PIN);
    if (current_lp_button_state == LOW && button_lp_last_state == HIGH && (current_millis - button_lp_last_press_time > DEBOUNCE_TIME_MS)) {
        button_lp_press_start_time = current_millis;
        button_lp_is_held = false;
    } else if (current_lp_button_state == LOW && !button_lp_is_held) { // Button is being held
        if (current_millis - button_lp_press_start_time > LONG_PRESS_TIME_MS) { // Long press detected
            isLowPowerModeActive = !isLowPowerModeActive;
            Serial.printf("LPM Toggled (Long Press): %s\n", isLowPowerModeActive ? "ON" : "OFF");
            
            savePersistentState(); // Save the new LPM state to EEPROM

            if (isLowPowerModeActive) { // Transitioning TO Low Power Mode
                temporaryScreenWakeupActive = false; // Ensure not in temp wake state
                turnScreenOn(); // Briefly show message
                displayMessage("Low Power Mode: ON", "Sleeping...", TFT_BLUE, true);
                delay(2000);
                // Screen will be turned off by enterDeepSleep
                struct tm timeinfo;
                if(!getLocalTime(&timeinfo, 10000)){
                    Serial.println("LPM Toggle ON FATAL: No time for sleep calc!");
                    enterDeepSleep(15*60*1000000ULL, true); // Fallback
                } else {
                    NextUpdateTimeDetails details = calculateNextUpdateTimeDetails(timeinfo, UPDATES_PER_HOUR_LPM, REFRESH_TARGET_MINUTE, false);
                    enterDeepSleep(details.sleepDurationUs, true);
                }
            } else { // Transitioning FROM Low Power Mode (to Normal Mode)
                temporaryScreenWakeupActive = false;
                turnScreenOn();
                displayMessage("Low Power Mode: OFF", "Refreshing...", TFT_GREEN, true);
                // delay(1500); // Let performDataFetchSequence handle messages

                Serial.println("Exiting LPM: Attempting data refresh...");
                performDataFetchSequence(false); // Fetch data non-silently

                // Schedule next update for normal mode
                struct tm timeinfo;
                 if(!getLocalTime(&timeinfo, 10000)){
                    Serial.println("LPM Toggle OFF ERR: No time for normal mode schedule!");
                    nextUpdateEpochNormalMode = 0; // Mark scheduler as not ready
                } else {
                    NextUpdateTimeDetails details = calculateNextUpdateTimeDetails(timeinfo, UPDATES_PER_HOUR_NORMAL_MODE, REFRESH_TARGET_MINUTE, true);
                    // If updateNow is true, performDataFetchSequence might have just run.
                    // The nextUpdateEpochNormalMode will be set for the *next* slot.
                    nextUpdateEpochNormalMode = details.nextUpdateEpoch;
                     #if DEBUG_SCHEDULING
                     if(details.updateNow && WiFi.status() == WL_CONNECTED) Serial.println("LPM Toggle OFF: Scheduler indicates immediate update (likely just handled).");
                     #endif
                }
                force_display_update = true; // Ensure display updates
            }
            button_lp_is_held = true; // Mark as handled
            button_lp_last_press_time = current_millis;
        }
    } else if (current_lp_button_state == HIGH && button_lp_last_state == LOW && (current_millis - button_lp_last_press_time > DEBOUNCE_TIME_MS)) { // Button released
        // No short press action for LP toggle button in this design
        button_lp_last_press_time = current_millis; // Debounce
        button_lp_is_held = false; // Reset held state
    }
    button_lp_last_state = current_lp_button_state;
}


// --- Display Functions ---
void displayMessage(String msg_line1, String msg_line2, int color, bool allowDisplay) {
    if (!allowDisplay && !(isLowPowerModeActive && temporaryScreenWakeupActive)) {
        // Only print to serial if display is skipped due to LPM and not temp wake
        #if DEBUG_LPM
        Serial.println("DisplayMessage Skipped (LPM off-screen): " + msg_line1 + " " + msg_line2);
        #endif
        return;
    }
    // If we are here, either allowDisplay is true, or it's LPM temp wake
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(color, TFT_BLACK);
    tft.setTextFont(2); // Choose a suitable font
    tft.setTextDatum(MC_DATUM); // Middle Center datum

    int16_t width = tft.width();
    int16_t height = tft.height();

    if (msg_line2 != "") {
        tft.drawString(msg_line1, width / 2, height / 2 - 10);
        tft.drawString(msg_line2, width / 2, height / 2 + 10);
    } else {
        tft.drawString(msg_line1, width / 2, height / 2);
    }
    #if DEBUG_LPM  // Or a general display debug flag
    Serial.println("Displaying Message: " + msg_line1 + " " + msg_line2);
    #endif
}

void displayInfo() {
    if (isLowPowerModeActive && !temporaryScreenWakeupActive) {
        #if DEBUG_LPM
        Serial.println("displayInfo call skipped: LPM active and screen not temporarily on.");
        #endif
        return;
    }

    tft.fillScreen(TFT_BLACK);
    int padding = 4;
    int top_y_offset = padding; // Initial Y offset for graph drawing

    // Font heights (assuming font 2 is default for info text)
    tft.setTextFont(2);
    int info_font_height = tft.fontHeight(2); // Height of text in font 2
    int base_top_text_line_y = padding + info_font_height / 2; // Y for first line of text

    if (showInfoOverlay) {
        int current_info_y = base_top_text_line_y;
        tft.setTextDatum(TL_DATUM); // Top-Left datum for these lines

        // LPM Status
        tft.setTextColor(isLowPowerModeActive ? TFT_ORANGE : TFT_GREEN, TFT_BLACK);
        tft.drawString(isLowPowerModeActive ? "LPM: ON" : "LPM: OFF", padding, current_info_y);
        current_info_y += (info_font_height + padding);

        // WiFi Status & Last Update Time (on the same line)
        if (WiFi.status() == WL_CONNECTED) {
            tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
            String ssid_str = WiFi.SSID();
            if (ssid_str.length() > 16) ssid_str = ssid_str.substring(0, 13) + "..."; // Truncate long SSIDs
            tft.drawString("WiFi: " + ssid_str, padding, current_info_y);
        } else if (isConnectingToWiFi) { // Show if actively trying to connect
             tft.setTextColor(TFT_YELLOW, TFT_BLACK);
             tft.drawString("WiFi: Connecting...", padding, current_info_y);
        }
         else { // WiFi is definitively offline
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.drawString("WiFi: Offline", padding, current_info_y);
        }

        // Last Update Time (Right Aligned on the same line as WiFi status)
        tft.setTextDatum(TR_DATUM); // Top-Right datum
        tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        String timeToDisplay = lastUpdateTimeStr;
        if (timeToDisplay.length() > 12) timeToDisplay = timeToDisplay.substring(0,9) + "..."; // Truncate if too long
        tft.drawString("Upd: " + timeToDisplay, tft.width() - padding, current_info_y);
        current_info_y += (info_font_height + padding);

        // Location Info
        tft.setTextDatum(TL_DATUM); // Back to Top-Left
        tft.setTextColor(TFT_SKYBLUE, TFT_BLACK);
        String locText = "Loc: " + locationDisplayStr + (useGpsFromSecrets ? " (Sec)" : " (IP)");
        if (locText.length() > 30) locText = locText.substring(0, 27) + "..."; // Truncate if very long
        tft.drawString(locText, padding, current_info_y);

        // Set top_y_offset for graph drawing below the info overlay
        top_y_offset = current_info_y + info_font_height / 2 + padding * 2;

    } else { // Info overlay is OFF - Minimal status display
        tft.setTextDatum(TR_DATUM); // Top-Right for "NoFi"
        int status_x = tft.width() - padding;
        int status_y = base_top_text_line_y;
        
        if (WiFi.status() != WL_CONNECTED && !isConnectingToWiFi) { // Only show if definitively offline
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.drawString("NoFi", status_x, status_y);
             // If "NoFi" is shown, it takes up some top space. Adjust graph offset.
            top_y_offset = base_top_text_line_y + info_font_height / 2 + padding * 2;
        } else if (isConnectingToWiFi) { // Show if actively trying to connect
             tft.setTextColor(TFT_YELLOW, TFT_BLACK);
             tft.drawString("WiFi?", status_x, status_y);
             top_y_offset = base_top_text_line_y + info_font_height / 2 + padding * 2;
        }
        // If WiFi is connected, or overlay is off with no "NoFi" message,
        // top_y_offset remains small (just `padding`), allowing more space for the graph.
        // If no status is shown at the top right, graph can start higher.
        if (WiFi.status() == WL_CONNECTED) {
             top_y_offset = padding; // Graph can start very high
        }
    }

    drawForecastGraph(top_y_offset);
}

void drawForecastGraph(int start_y_offset) {
    // Graph drawing parameters
    int padding = 2; // Padding around graph elements
    int first_uv_val_font = 6; // Font for the first (current/next hour) UV value
    int other_uv_val_font = 4; // Font for subsequent UV values
    int hour_label_font = 2;   // Font for the hour labels at the bottom

    // Calculate text heights for precise positioning
    tft.setTextFont(first_uv_val_font);
    int first_uv_text_h = tft.fontHeight();
    tft.setTextFont(other_uv_val_font);
    int other_uv_text_h = tft.fontHeight();
    tft.setTextFont(hour_label_font);
    int hour_label_text_h = tft.fontHeight();

    // Y positions for graph elements
    // Center the first UV value text vertically in its allocated space
    int first_uv_value_y = start_y_offset + padding + first_uv_text_h / 2;
    // Align subsequent UV values with the first one (adjusting for different font sizes)
    int other_uv_values_line_y = first_uv_value_y + (first_uv_text_h / 2) - (other_uv_text_h / 2);
    if (HOURLY_FORECAST_COUNT <= 1) other_uv_values_line_y = first_uv_value_y; // If only one forecast, use first_uv_value_y

    // Hour labels at the bottom of the screen
    int hour_label_y = tft.height() - padding - hour_label_text_h / 2;
    // Baseline for the bars (top of the hour labels)
    int graph_baseline_y = hour_label_y - hour_label_text_h / 2 - padding;

    // Calculate available height for bars
    int max_bar_pixel_height = graph_baseline_y - (start_y_offset + padding);
    // Ensure a minimum bar height if space is very constrained
    if (max_bar_pixel_height < 10) max_bar_pixel_height = 10;
    if (max_bar_pixel_height < 20 && tft.height() > 100) max_bar_pixel_height = 20; // Slightly larger min if screen is taller

    // UV scale for bars
    const float MAX_UV_FOR_FULL_SCALE = 10.0f; // UV index that corresponds to full bar height
    float pixel_per_uv_unit = 0;
    if (MAX_UV_FOR_FULL_SCALE > 0) {
        pixel_per_uv_unit = (float)max_bar_pixel_height / MAX_UV_FOR_FULL_SCALE;
    }

    // Bar width and spacing
    int graph_area_total_width = tft.width() - 2 * padding; // Usable width for all bars
    int bar_slot_width = graph_area_total_width / HOURLY_FORECAST_COUNT; // Width per bar including spacing
    int bar_actual_width = bar_slot_width * 0.75; // Actual width of the bar (75% of slot)
    if (bar_actual_width < 4) bar_actual_width = 4; // Min bar width
    if (bar_actual_width > 30) bar_actual_width = 30; // Max bar width

    // Starting X position for the graph area (to center it)
    int graph_area_x_start = (tft.width() - (bar_slot_width * HOURLY_FORECAST_COUNT)) / 2 + padding;

    // Draw each forecast item
    for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
        int bar_center_x = graph_area_x_start + (i * bar_slot_width) + (bar_slot_width / 2);

        // Draw hour label
        tft.setTextFont(hour_label_font);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setTextDatum(MC_DATUM); // Middle Center for hour label
        if (forecastHours[i] != -1) {
            tft.drawString(String(forecastHours[i]), bar_center_x, hour_label_y);
        } else {
            tft.drawString("-", bar_center_x, hour_label_y); // Placeholder if no hour data
        }

        float uvVal = hourlyUV[i];
        int roundedUV = (uvVal >= -0.01f) ? round(uvVal) : -1; // Round UV, treat slightly negative as 0 for rounding

        if (roundedUV != -1 && forecastHours[i] != -1) { // Valid data to draw
            // Calculate bar height
            float uv_for_height_calc = (float)roundedUV;
            if (uv_for_height_calc < 0) uv_for_height_calc = 0; // Ensure non-negative for calculation
            if (uv_for_height_calc > MAX_UV_FOR_FULL_SCALE) uv_for_height_calc = MAX_UV_FOR_FULL_SCALE; // Cap at max scale

            int bar_height = round(uv_for_height_calc * pixel_per_uv_unit);
            // Ensure minimal visibility for very low UV values
            if (uvVal > 0 && uvVal < 0.5f && bar_height == 0) bar_height = 1; // Tiny bar for small positive UV
            else if (roundedUV >= 1 && bar_height == 0 && pixel_per_uv_unit > 0) bar_height = 1; // Min 1px if UV >=1 and scale allows
            else if (roundedUV >= 1 && bar_height < 2 && pixel_per_uv_unit > 2) bar_height = 2; // Min 2px if scale is larger

            if (bar_height > max_bar_pixel_height) bar_height = max_bar_pixel_height; // Cap height
            if (bar_height < 0) bar_height = 0; // Ensure non-negative

            int bar_top_y = graph_baseline_y - bar_height; // Y position for top of the bar

            // Determine bar color based on UV index
            uint16_t barColor;
            if (roundedUV < 1) barColor = TFT_DARKGREY;       // Low
            else if (roundedUV <= 2) barColor = TFT_GREEN;    // Low (WHO: 0-2)
            else if (roundedUV <= 5) barColor = TFT_YELLOW;   // Moderate (WHO: 3-5)
            else if (roundedUV <= 7) barColor = 0xFC60;       // Orange for High (WHO: 6-7)
            else if (roundedUV <= 10) barColor = TFT_RED;     // Very High (WHO: 8-10)
            else barColor = TFT_MAGENTA;                      // Extreme (WHO: 11+)

            if (bar_height > 0) {
                tft.fillRect(bar_center_x - bar_actual_width / 2, bar_top_y, bar_actual_width, bar_height, barColor);
            }

            // Draw UV value text
            tft.setTextDatum(MC_DATUM); // Middle Center for UV value text
            String uvText = String(roundedUV);
            int current_uv_text_y;
            uint16_t outlineColor = TFT_BLACK; // Color for text outline
            uint16_t foregroundColor = TFT_WHITE; // Main text color

            if (i == 0) { // First forecast item (larger font)
                tft.setTextFont(first_uv_val_font);
                current_uv_text_y = first_uv_value_y;
            } else { // Subsequent forecast items
                tft.setTextFont(other_uv_val_font);
                current_uv_text_y = other_uv_values_line_y;
            }

            // Simple outline effect by drawing text multiple times with offset
            tft.setTextColor(outlineColor);
            const int outlineOffset = 1;
            tft.drawString(uvText, bar_center_x - outlineOffset, current_uv_text_y - outlineOffset);
            tft.drawString(uvText, bar_center_x + outlineOffset, current_uv_text_y - outlineOffset);
            tft.drawString(uvText, bar_center_x - outlineOffset, current_uv_text_y + outlineOffset);
            tft.drawString(uvText, bar_center_x + outlineOffset, current_uv_text_y + outlineOffset);
            tft.drawString(uvText, bar_center_x - outlineOffset, current_uv_text_y);
            tft.drawString(uvText, bar_center_x + outlineOffset, current_uv_text_y);
            tft.drawString(uvText, bar_center_x, current_uv_text_y - outlineOffset);
            tft.drawString(uvText, bar_center_x, current_uv_text_y + outlineOffset);

            // Draw main text on top
            tft.setTextColor(foregroundColor, TFT_BLACK); // Set background color for text to avoid artifacts
            tft.drawString(uvText, bar_center_x, current_uv_text_y);

        } else { // Placeholder if no valid UV data for this hour
            int placeholder_y;
            int placeholder_font;
            if (i == 0) {
                placeholder_font = first_uv_val_font;
                placeholder_y = first_uv_value_y;
            } else {
                placeholder_font = other_uv_val_font;
                placeholder_y = other_uv_values_line_y;
            }
            tft.setTextFont(placeholder_font);
            tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
            tft.setTextDatum(MC_DATUM);
            tft.drawString("-", bar_center_x, placeholder_y);
        }
    }
    #if DEBUG_GRAPH_DRAWING
    Serial.println("--- End of Graph Draw Cycle ---");
    #endif
}


// --- Network and Data Fetching Functions ---
void connectToWiFi(bool silent) {
    isConnectingToWiFi = true; // Set flag
    if (!silent) {
        Serial.println("Connecting to WiFi using secrets.h values...");
        // displayMessage might be called by caller, or rely on displayInfo update
        force_display_update = true; // Trigger display update to show "Connecting..."
    }
    #if DEBUG_LPM
    else Serial.println("LPM Silent: Connecting to WiFi...");
    #endif

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true,true); // Disconnect previous session, if any
    delay(100); // Short delay for WiFi system

    bool connected = false;
    String connected_ssid = "";

    // Attempt to connect to defined WiFi networks in secrets.h
    // This structure allows for easy expansion if more SSIDs are defined.
    const char* ssids[] = {WIFI_SSID_1, WIFI_SSID_2
                          #if defined(WIFI_SSID_3)
                          , WIFI_SSID_3
                          #endif
                          // Add more SSIDs here if defined in secrets.h
                          };
    const char* passwords[] = {WIFI_PASS_1, WIFI_PASS_2
                              #if defined(WIFI_PASS_3)
                              , WIFI_PASS_3
                              #endif
                              // Add more passwords here
                              };
    int num_networks = sizeof(ssids) / sizeof(ssids[0]);

    for (int i = 0; i < num_networks; ++i) {
        if (strlen(ssids[i]) > 0) { // Check if SSID is defined
            if (!silent) {Serial.print("Attempting SSID: "); Serial.println(ssids[i]);}
            WiFi.begin(ssids[i], passwords[i]);
            unsigned long startTime = millis();
            while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_CONNECTION_TIMEOUT_MS) {
                delay(100);
                if (!silent && (millis() - startTime) % 1000 < 100) Serial.print("."); // Progress dots
            }

            if (WiFi.status() == WL_CONNECTED) {
                connected = true;
                connected_ssid = ssids[i];
                break; // Exit loop once connected
            } else {
                if (!silent) Serial.printf("\nFailed to connect to SSID %s.\n", ssids[i]);
                WiFi.disconnect(true,true); // Disconnect attempt before trying next
                delay(100);
            }
        }
    }

    isConnectingToWiFi = false; // Clear flag
    force_display_update = true; // Trigger display update with new WiFi status

    if (connected) {
        if (!silent) {
            Serial.println("\nWiFi connected!");
            Serial.print("SSID: "); Serial.println(connected_ssid);
            Serial.print("IP address: "); Serial.println(WiFi.localIP());
        }
        #if DEBUG_LPM
        else Serial.printf("LPM Silent: WiFi connected to %s, IP: %s\n", connected_ssid.c_str(), WiFi.localIP().toString().c_str());
        #endif
        
        // Configure time via NTP (UTC)
        // The offset will be applied later from Open-Meteo API if available
        if (!silent) Serial.println("Configuring time via NTP (UTC initial)...");
        configTime(0, 0, "pool.ntp.org", "time.nist.gov"); // UTC, no DST initially
        
        struct tm timeinfo;
        if(!getLocalTime(&timeinfo, 10000)){ // Wait up to 10s for NTP sync
            if (!silent) Serial.println("Failed to obtain initial time from NTP.");
            // Time might still be obtained later by API, or if NTP syncs slowly.
        } else {
            if (!silent) Serial.println("Initial time configured via NTP (UTC).");
        }
    } else { // Could not connect to any network
        if (!silent) {
            Serial.println("\nCould not connect to any configured WiFi network.");
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
        locationDisplayStr = "IP (NoNet)"; // Update display string
        return false;
    }

    HTTPClient http;
    String url = "http://ip-api.com/json/?fields=status,message,lat,lon,city"; // Request specific fields

    if (!silent) {Serial.print("Fetching IP Geolocation: "); Serial.println(url);}
    #if DEBUG_LPM
    else Serial.println("LPM Silent: Fetching IP Geolocation...");
    #endif

    http.begin(url);
    http.setTimeout(10000); // 10 second timeout for HTTP request
    int httpCode = http.GET();

    if (!silent) {Serial.print("IP Geolocation HTTP Code: "); Serial.println(httpCode);}
    #if DEBUG_LPM
    else Serial.printf("LPM Silent: IP Geo HTTP Code: %d\n", httpCode);
    #endif

    bool success = false;
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        if (!silent) Serial.println("IP Geolocation Payload: " + payload);

        JsonDocument doc; // Using dynamic JsonDocument, ensure it's appropriately sized for payload
        DeserializationError error = deserializeJson(doc, payload);

        if (error) {
            if (!silent) {Serial.print(F("deserializeJson() for IP Geo failed: ")); Serial.println(error.c_str());}
            #if DEBUG_LPM
            else Serial.printf("LPM Silent: IP Geo JSON deserialize failed: %s\n", error.c_str());
            #endif
            locationDisplayStr = "IP (JSON Err)";
        } else {
            if (!doc["status"].isNull() && strcmp(doc["status"], "success") == 0) {
                deviceLatitude = doc["lat"].as<float>();
                deviceLongitude = doc["lon"].as<float>();
                const char* city = doc["city"];
                if (city) {
                    locationDisplayStr = String("IP: ") + city;
                } else {
                    locationDisplayStr = "IP: Unknown";
                }
                
                if (!silent) Serial.printf("IP Geo Location: Lat=%.4f, Lon=%.4f, City=%s\n", deviceLatitude, deviceLongitude, city ? city : "N/A");
                #if DEBUG_LPM
                else Serial.printf("LPM Silent: IP Geo Success: Lat=%.2f Lon=%.2f City=%s\n", deviceLatitude, deviceLongitude, city ? city : "N/A");
                #endif
                
                // Update RTC memory with new location if fetched successfully
                rtc_deviceLatitude = deviceLatitude;
                rtc_deviceLongitude = deviceLongitude;
                strncpy(rtc_locationDisplayStr_char, locationDisplayStr.c_str(), sizeof(rtc_locationDisplayStr_char) - 1);
                rtc_locationDisplayStr_char[sizeof(rtc_locationDisplayStr_char) - 1] = '\0';
                // rtc_hasValidData for location isn't explicitly tracked, but this updates the location part of RTC
                success = true;
            } else { // API returned success=false or other status
                const char* msg = doc["message"];
                if (!silent) Serial.printf("IP Geolocation API Error: %s\n", msg ? msg : "Unknown error");
                locationDisplayStr = String("IP (API Err)");
            }
        }
    } else { // HTTP error
        locationDisplayStr = String("IP (HTTP Err ") + String(httpCode) + String(")");
    }
    http.end();
    return success;
}

bool fetchUVData(bool silent) {
    if (WiFi.status() != WL_CONNECTED) {
        if (!silent) Serial.println("WiFi not connected, cannot fetch UV data.");
        #if DEBUG_LPM
        else Serial.println("LPM Silent: No WiFi, cannot fetch UV data.");
        #endif
        initializeForecastData(true); // Clear current and RTC forecast data
        lastUpdateTimeStr = "Offline";
        strncpy(rtc_lastUpdateTimeStr_char, "Offline", sizeof(rtc_lastUpdateTimeStr_char)-1);
        rtc_lastUpdateTimeStr_char[sizeof(rtc_lastUpdateTimeStr_char)-1] = '\0';
        rtc_hasValidData = false; // Mark RTC data as invalid
        dataJustFetched = true; // Trigger display update
        return false;
    }

    HTTPClient http;
    // Construct API URL with current lat/lon
    String apiUrl = openMeteoUrl + "?latitude=" + String(deviceLatitude, 4) +
                    "&longitude=" + String(deviceLongitude, 4) +
                    "&hourly=uv_index&forecast_days=1&timezone=auto";

    if (!silent) {Serial.print("Fetching UV Data from URL: "); Serial.println(apiUrl);}
    #if DEBUG_LPM
    else Serial.println("LPM Silent: Fetching UV data...");
    #endif

    http.begin(apiUrl);
    http.setTimeout(15000); // 15 second timeout
    int httpCode = http.GET();

    if (!silent) {Serial.print("Open-Meteo API HTTP Code: "); Serial.println(httpCode);}
    #if DEBUG_LPM
    else Serial.printf("LPM Silent: Open-Meteo HTTP Code: %d\n", httpCode);
    #endif

    bool parseSuccess = false;
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        // if (!silent) Serial.println("Open-Meteo Payload: " + payload); // Can be very long

        JsonDocument doc; // Dynamic, ensure ESP has enough RAM for typical payload
        DeserializationError error = deserializeJson(doc, payload);

        if (error) {
            if (!silent) {Serial.print(F("deserializeJson() for UV data failed: ")); Serial.println(error.c_str());}
            #if DEBUG_LPM
            else Serial.printf("LPM Silent: UV JSON deserialize failed: %s\n", error.c_str());
            #endif
            initializeForecastData(true); rtc_hasValidData = false;
        } else { // JSON parsing successful
            // Reconfigure ESP32 local time using UTC offset from API for better accuracy
            if (!doc["utc_offset_seconds"].isNull()) {
                long api_utc_offset_sec = doc["utc_offset_seconds"].as<long>();
                // configTime(gmtOffset_sec, daylightOffset_sec, ntpServer)
                configTime(api_utc_offset_sec, 0, "pool.ntp.org", "time.nist.gov"); // 0 for daylightOffset as API gives total offset
                if (!silent) Serial.println("ESP32 local time reconfigured using Open-Meteo offset.");
                #if DEBUG_LPM
                else Serial.println("LPM Silent: ESP32 time reconfigured from API offset.");
                #endif
                delay(100); // Allow time for system to potentially update from new config
            }

            // Check if hourly data is present
            if (!doc["hourly"].isNull() && !doc["hourly"]["time"].isNull() && !doc["hourly"]["uv_index"].isNull()) {
                JsonArray hourly_time_list = doc["hourly"]["time"].as<JsonArray>();
                JsonArray hourly_uv_list = doc["hourly"]["uv_index"].as<JsonArray>();

                struct tm timeinfo; // To get current local hour
                if (!getLocalTime(&timeinfo, 5000)) { // Wait up to 5s for time
                    if (!silent) Serial.println("Failed to obtain ESP32 local time for forecast matching.");
                    #if DEBUG_LPM
                    else Serial.println("LPM Silent: Failed to get local time for forecast matching.");
                    #endif
                    initializeForecastData(true); rtc_hasValidData = false;
                } else {
                    int currentHourLocal = timeinfo.tm_hour;
                    int startIndex = -1; // Index in API arrays corresponding to current local hour or later

                    // Find the starting index in the API's hourly forecast
                    for (int i = 0; i < hourly_time_list.size(); ++i) {
                        String api_time_str = hourly_time_list[i].as<String>(); // e.g., "2023-05-20T14:00"
                        if (api_time_str.length() >= 13) { // Ensure "T" and hour are present
                            int api_hour = api_time_str.substring(11, 13).toInt();
                            if (api_hour >= currentHourLocal) {
                                startIndex = i;
                                break;
                            }
                        }
                    }

                    if (startIndex != -1) { // Found a suitable starting point in forecast
                        for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
                            if (startIndex + i < hourly_uv_list.size() && startIndex + i < hourly_time_list.size()) {
                                JsonVariant uv_val_variant = hourly_uv_list[startIndex + i];
                                // Handle null UV values from API (common at night)
                                hourlyUV[i] = uv_val_variant.isNull() ? 0.0f : uv_val_variant.as<float>();
                                if (hourlyUV[i] < 0) hourlyUV[i] = 0.0f; // Ensure non-negative
                                
                                rtc_hourlyUV[i] = hourlyUV[i]; // Update RTC copy

                                String api_t_str = hourly_time_list[startIndex + i].as<String>();
                                forecastHours[i] = api_t_str.substring(11, 13).toInt(); // Extract hour
                                rtc_forecastHours[i] = forecastHours[i]; // Update RTC copy
                            } else { // Not enough forecast data from API for all slots
                                hourlyUV[i] = -1.0f; forecastHours[i] = -1; // Mark as no data
                                rtc_hourlyUV[i] = -1.0f; rtc_forecastHours[i] = -1;
                            }
                        }
                        parseSuccess = true; // Data successfully parsed and populated
                        rtc_hasValidData = true; // Mark RTC data as valid
                        if (!silent) Serial.println("Successfully populated hourly forecast data.");
                        #if DEBUG_LPM
                        else Serial.println("LPM Silent: Successfully populated hourly forecast data to RAM & RTC.");
                        #endif
                    } else { // No forecast data found for current or future hours
                        if (!silent) Serial.println("No suitable starting forecast index found in API response.");
                        #if DEBUG_LPM
                        else Serial.println("LPM Silent: No suitable forecast index.");
                        #endif
                        initializeForecastData(true); rtc_hasValidData = false; // Clear data
                    }
                }
            } else { // "hourly", "hourly.time", or "hourly.uv_index" missing in JSON
                if (!silent) Serial.println("Hourly UV data structure not found or incomplete in JSON response.");
                 #if DEBUG_LPM
                else Serial.println("LPM Silent: Hourly UV data structure not found/incomplete.");
                #endif
                initializeForecastData(true); rtc_hasValidData = false; // Clear data
            }
        }

        // Update lastUpdateTimeStr regardless of parse success, using current time
        struct tm timeinfo_update;
        if(getLocalTime(&timeinfo_update, 1000)){ // Quick attempt to get current time
            char timeStrBuffer[16];
            // Try to get timezone abbreviation from API response for display
            const char* tz_abbr = doc["timezone_abbreviation"].isNull() ? nullptr : doc["timezone_abbreviation"].as<const char*>();
            
            if (tz_abbr && strlen(tz_abbr) > 0 && strlen(tz_abbr) < 5) { // Basic check for valid TZ string
                snprintf(timeStrBuffer, sizeof(timeStrBuffer), "%02d:%02d %s", timeinfo_update.tm_hour, timeinfo_update.tm_min, tz_abbr);
            } else {
                strftime(timeStrBuffer, sizeof(timeStrBuffer), "%H:%M", &timeinfo_update); // Default to HH:MM if no TZ
            }
            lastUpdateTimeStr = String(timeStrBuffer);
            strncpy(rtc_lastUpdateTimeStr_char, lastUpdateTimeStr.c_str(), sizeof(rtc_lastUpdateTimeStr_char)-1);
            rtc_lastUpdateTimeStr_char[sizeof(rtc_lastUpdateTimeStr_char)-1] = '\0';
        } else { // Failed to get current time for update string
            lastUpdateTimeStr = "Time Err";
            strncpy(rtc_lastUpdateTimeStr_char, "Time Err", sizeof(rtc_lastUpdateTimeStr_char)-1);
            rtc_lastUpdateTimeStr_char[sizeof(rtc_lastUpdateTimeStr_char)-1] = '\0';
        }

    } else { // HTTP request failed
        initializeForecastData(true); rtc_hasValidData = false; // Clear data
        if (WiFi.status() != WL_CONNECTED) { // If HTTP failed because WiFi dropped
            lastUpdateTimeStr = "Offline";
            strncpy(rtc_lastUpdateTimeStr_char, "Offline", sizeof(rtc_lastUpdateTimeStr_char)-1);
        } else { // WiFi connected, but API error
            lastUpdateTimeStr = "API Err " + String(httpCode);
            strncpy(rtc_lastUpdateTimeStr_char, ("API Err " + String(httpCode)).c_str(), sizeof(rtc_lastUpdateTimeStr_char)-1);
        }
         rtc_lastUpdateTimeStr_char[sizeof(rtc_lastUpdateTimeStr_char)-1] = '\0';
    }
    http.end();
    dataJustFetched = true; // Trigger display update
    return parseSuccess; // Return true if UV data was successfully parsed from JSON
}
