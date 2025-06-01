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
const int WIFI_CONNECTION_TIMEOUT_MS = 15000;
const unsigned long SCREEN_ON_DURATION_LPM_MS = 30 * 1000; // 30 second screen on time in LPM

// --- New Scheduling Configuration ---
const byte REFRESH_TARGET_MINUTE = 2;         // Base minute of the hour for the first refresh slot (0-59)
const byte UPDATES_PER_HOUR_NORMAL_MODE = 4;  // e.g., 4 for every 15 mins, 2 for every 30 mins, 1 for hourly
const byte UPDATES_PER_HOUR_LPM = 1;          // e.g., 1 for hourly (at REFRESH_TARGET_MINUTE)

// --- EEPROM Configuration ---
#define EEPROM_SIZE 1          // Size for EEPROM (1 byte for LPM flag)
#define LPM_FLAG_EEPROM_ADDR 0 // EEPROM address for LPM flag

// --- Debugging Flags ---
#define DEBUG_LPM 0             // Set to 1 to enable LPM specific logs
#define DEBUG_GRAPH_DRAWING 0   // Set to 1 to enable detailed graph drawing logs, 0 to disable
#define DEBUG_PERSISTENCE 0     // Set to 1 to enable detailed EEPROM/RTC save/load logs
#define DEBUG_SCHEDULING 0      // Set to 1 to enable detailed scheduling logs

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

#define RTC_MAGIC_VALUE 0xDEADBEEF

// --- Global variables for Scheduling ---
unsigned long nextUpdateEpochNormalMode = 0; // Stores the epoch time for the next scheduled update in normal mode
unsigned long nextUpdateEpochLpm = 0;        // Stores the epoch time for the next scheduled update in LPM


// --- Function Declarations ---
void turnScreenOn();
void turnScreenOff();
void savePersistentState();
void loadPersistentState();

struct NextUpdateTimeDetails {
    uint64_t sleepDurationUs;
    time_t nextUpdateEpoch;
    bool updateNow;
};
NextUpdateTimeDetails calculateNextUpdateTimeDetails(const struct tm& currentTimeInfo, byte updatesPerHour, byte targetStartMinute, bool isNormalModeCheck);
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
void performDataFetchSequence(bool silent);

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
    #if DEBUG_PERSISTENCE
    Serial.printf("PERSISTENCE SAVE (RTC part): HasValidData: %s, UseGPSSecrets: %s\n", rtc_hasValidData ? "Yes" : "No", rtc_useGpsFromSecretsGlobal ? "Yes" : "No");
    #endif
}

void loadPersistentState() {
    #if DEBUG_PERSISTENCE
    Serial.println("PERSISTENCE LOAD: Attempting to load state...");
    #endif

    byte lpmFlagFromEEPROM = EEPROM.read(LPM_FLAG_EEPROM_ADDR);
    if (lpmFlagFromEEPROM == 0 || lpmFlagFromEEPROM == 1) {
        isLowPowerModeActive = (bool)lpmFlagFromEEPROM;
        #if DEBUG_PERSISTENCE
        Serial.printf("PERSISTENCE LOAD: Loaded isLowPowerModeActive = %s from EEPROM.\n", isLowPowerModeActive ? "true" : "false");
        #endif
    } else {
        #if DEBUG_PERSISTENCE
        Serial.printf("PERSISTENCE LOAD: EEPROM LPM flag uninitialized (read: 0x%X). Defaulting to LPM OFF.\n", lpmFlagFromEEPROM);
        #endif
        isLowPowerModeActive = false;
        EEPROM.write(LPM_FLAG_EEPROM_ADDR, 0);
        EEPROM.commit();
    }
    
    if (rtc_magic_cookie == RTC_MAGIC_VALUE) {
        useGpsFromSecrets = rtc_useGpsFromSecretsGlobal;
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
            #if DEBUG_PERSISTENCE
            Serial.println("PERSISTENCE LOAD: Valid data loaded from RTC.");
            #endif
        } else {
            initializeForecastData();
            #if DEBUG_PERSISTENCE
            Serial.println("PERSISTENCE LOAD: RTC data marked as not valid, initialized working data to defaults.");
            #endif
        }
    } else {
        #if DEBUG_PERSISTENCE
        Serial.println("PERSISTENCE LOAD: RTC magic cookie mismatch. Initializing RTC data to defaults.");
        #endif
        useGpsFromSecrets = false;
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
        rtc_magic_cookie = RTC_MAGIC_VALUE;
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
    result.sleepDurationUs = 15 * 60 * 1000000ULL; 
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

    int intervalMinutes = (updatesPerHour > 0) ? (60 / updatesPerHour) : 60; 
    if (intervalMinutes == 0 && updatesPerHour > 0) intervalMinutes = 1; 

    time_t foundNextEpoch = 0;

    for (int h_offset = 0; h_offset < 2; ++h_offset) {
        for (int i = 0; i < updatesPerHour; ++i) {
            struct tm candidateTimeStruct = currentTimeInfo;
            candidateTimeStruct.tm_hour = currentTimeInfo.tm_hour + h_offset;
            candidateTimeStruct.tm_min = targetStartMinute + (i * intervalMinutes);
            candidateTimeStruct.tm_sec = 0;

            time_t candidateEpoch = mktime(&candidateTimeStruct);

            if (candidateEpoch > nowEpoch) {
                if (foundNextEpoch == 0 || candidateEpoch < foundNextEpoch) {
                    foundNextEpoch = candidateEpoch;
                }
            } else if (isNormalModeCheck && (candidateEpoch == nowEpoch || (nowEpoch - candidateEpoch < intervalMinutes * 60 && nowEpoch - candidateEpoch < 30 ) )) {
                result.updateNow = true;
            }
        }
        if (foundNextEpoch != 0 && !result.updateNow) {
            break;
        }
        if (foundNextEpoch != 0 && result.updateNow && foundNextEpoch > nowEpoch) {
            break; 
        }
    }

    if (result.updateNow && isNormalModeCheck) {
        if (foundNextEpoch == 0 || foundNextEpoch <= nowEpoch) {
            struct tm nextSlotTimeCalc = currentTimeInfo;
            time_t tempNowPlusInterval = nowEpoch + (intervalMinutes * 60);
            nextSlotTimeCalc = *localtime(&tempNowPlusInterval); 
            
            bool slotFoundForNext = false;
            for(int h_calc = 0; h_calc < 2; ++h_calc) {
                for (int i_calc = 0; i_calc < updatesPerHour; ++i_calc) {
                    struct tm tempCalc = currentTimeInfo; 
                    tempCalc.tm_hour = nextSlotTimeCalc.tm_hour + h_calc; 
                    tempCalc.tm_min = targetStartMinute + (i_calc * intervalMinutes);
                    tempCalc.tm_sec = 0;
                    time_t calcEpoch = mktime(&tempCalc);
                    if (calcEpoch > nowEpoch) {
                         if (foundNextEpoch == 0 || calcEpoch < foundNextEpoch || (foundNextEpoch <= nowEpoch && calcEpoch > foundNextEpoch) ){
                            foundNextEpoch = calcEpoch;
                            slotFoundForNext = true;
                         }
                    }
                }
                if(slotFoundForNext && (foundNextEpoch > nowEpoch)) break;
            }
             if (!slotFoundForNext || foundNextEpoch <= nowEpoch) { 
                struct tm fallbackNext = currentTimeInfo;
                fallbackNext.tm_hour +=1;
                fallbackNext.tm_min = targetStartMinute;
                fallbackNext.tm_sec = 0;
                foundNextEpoch = mktime(&fallbackNext);
                if (foundNextEpoch <= nowEpoch) { 
                    fallbackNext.tm_mday +=1; 
                    mktime(&fallbackNext); 
                    foundNextEpoch = mktime(&fallbackNext);
                }
             }
        }
        result.nextUpdateEpoch = foundNextEpoch;
        result.sleepDurationUs = 0; 
    } else if (foundNextEpoch != 0) {
        result.nextUpdateEpoch = foundNextEpoch;
        result.sleepDurationUs = (uint64_t)difftime(result.nextUpdateEpoch, nowEpoch) * 1000000ULL;
    } else {
        struct tm fallbackTime = currentTimeInfo;
        fallbackTime.tm_hour += 1;
        fallbackTime.tm_min = targetStartMinute;
        fallbackTime.tm_sec = 0;
        result.nextUpdateEpoch = mktime(&fallbackTime);
        if (result.nextUpdateEpoch <= nowEpoch) { 
            fallbackTime.tm_mday +=1; 
            mktime(&fallbackTime); 
            result.nextUpdateEpoch = mktime(&fallbackTime);
        }
        result.sleepDurationUs = (uint64_t)difftime(result.nextUpdateEpoch, nowEpoch) * 1000000ULL;
        #if DEBUG_SCHEDULING
        Serial.println("SCHED WARN: No specific update slot found in 2h search, defaulting to next available target minute.");
        #endif
    }
    
    if (result.sleepDurationUs == 0 && !result.updateNow && result.nextUpdateEpoch == nowEpoch) { 
        #if DEBUG_SCHEDULING
        Serial.println("SCHED ERR: Calculated sleep duration is 0 when not updating now and nextEpoch is now. Advancing to next interval.");
        #endif
        result.nextUpdateEpoch = nowEpoch + (uint64_t)intervalMinutes * 60;
        result.sleepDurationUs = (uint64_t)intervalMinutes * 60 * 1000000ULL;
    }
    if (result.sleepDurationUs == 0 && !result.updateNow) { 
         result.sleepDurationUs = (uint64_t)intervalMinutes * 60 * 1000000ULL;
         if(result.sleepDurationUs == 0) result.sleepDurationUs = 60 * 60 * 1000000ULL; 
         result.nextUpdateEpoch = nowEpoch + (result.sleepDurationUs / 1000000ULL);
    }
    
    uint64_t practicalMaxSleepUs = 3LL * 60 * 60 * 1000000; 
    if (!isNormalModeCheck && result.sleepDurationUs > practicalMaxSleepUs) { 
        #if DEBUG_SCHEDULING
        Serial.printf("SCHED WARN: LPM Sleep duration %llu us too long. Capping to ~%d min.\n", result.sleepDurationUs, intervalMinutes > 0 ? intervalMinutes : 60);
        #endif
        result.sleepDurationUs = (uint64_t)(intervalMinutes > 0 ? intervalMinutes : 60) * 60 * 1000000ULL;
        if(result.sleepDurationUs == 0 || result.sleepDurationUs > practicalMaxSleepUs) result.sleepDurationUs = 60*60*1000000ULL;
        result.nextUpdateEpoch = nowEpoch + (result.sleepDurationUs / 1000000ULL);
    }

    #if DEBUG_SCHEDULING
        char timeBuff[30], nextTimeBuff[30];
        strftime(timeBuff, sizeof(timeBuff), "%F %T", &currentTimeInfo);
        time_t net_debug = result.nextUpdateEpoch; 
        strftime(nextTimeBuff, sizeof(nextTimeBuff), "%F %T", localtime(&net_debug));
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
    Serial.println("Initializing forecast data to defaults (-1).");
    for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
        hourlyUV[i] = -1.0f; 
        forecastHours[i] = -1;
        if (updateRTC) {
            rtc_hourlyUV[i] = -1.0f;
            rtc_forecastHours[i] = -1;
        }
    }
}

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
            if (!fetchLocationFromIp(silent)) {
                useGpsFromSecrets = true; 
                deviceLatitude = MY_LATITUDE; deviceLongitude = MY_LONGITUDE;
                locationDisplayStr = "IP Fail>Secrets";
                if (!silent) Serial.println("IP Geolocation failed. Falling back to secrets.h GPS.");
            }
        }
        String currentStatusForDisplay = locationDisplayStr;
        if (currentStatusForDisplay.length() > 18) currentStatusForDisplay = currentStatusForDisplay.substring(0,15) + "...";

        if (!silent) displayMessage("Fetching UV data...", currentStatusForDisplay, TFT_CYAN, true);
        if (fetchUVData(silent)) { 
            if (!isLowPowerModeActive) lastDataFetchAttemptMs = millis(); 
        } else {
            if (!silent) Serial.println("UV Data fetch failed (API did not return parsable data for any slot).");
        }
    } else { 
        locationDisplayStr = "Offline>Secrets"; 
        useGpsFromSecrets = true; 
        deviceLatitude = MY_LATITUDE; deviceLongitude = MY_LONGITUDE;
        struct tm timeinfo_offline;
        if (getLocalTime(&timeinfo_offline, 2000)) { 
             for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
                forecastHours[i] = (timeinfo_offline.tm_hour + i) % 24;
                hourlyUV[i] = 0.0f;
                // Only update RTC if it was previously valid, to preserve last good data if any,
                // or if we want to show projected zeros in RTC too.
                // For now, let's assume if offline, RTC reflects the projected zeros for consistency.
                rtc_forecastHours[i] = forecastHours[i];
                rtc_hourlyUV[i] = hourlyUV[i];
            }
            rtc_hasValidData = true; // We have a valid structure (projected hours and zeros)
        } else { 
            initializeForecastData(true); 
            rtc_hasValidData = false;
        }
        lastUpdateTimeStr = "Offline";
        strncpy(rtc_lastUpdateTimeStr_char, "Offline", sizeof(rtc_lastUpdateTimeStr_char)-1);
        rtc_lastUpdateTimeStr_char[sizeof(rtc_lastUpdateTimeStr_char)-1] = '\0';
        
        dataJustFetched = true; 
        if (!silent) Serial.println("WiFi not connected. Displaying projected hours with 0 UV or placeholders.");
    }
    force_display_update = true;
    // Save state if we got new IP, new UV data, or if GPS preference changed.
    // fetchUVData sets rtc_hasValidData, performDataFetchSequence calls savePersistentState if WiFi was connected.
    // If offline, we also want to save the projected data and "Offline" status.
    savePersistentState(); 
}

// --- Setup ---
void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 2000); 
    Serial.println("\nUV Index Monitor Starting Up...");

    EEPROM.begin(EEPROM_SIZE);

    pinMode(BUTTON_INFO_PIN, INPUT_PULLUP);
    pinMode(BUTTON_LP_TOGGLE_PIN, INPUT_PULLUP);
    pinMode(TFT_BL_PIN, OUTPUT);

    printWakeupReason();
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    
    loadPersistentState(); 
    #if DEBUG_PERSISTENCE
    Serial.printf("SETUP: After loadPersistentState(), isLowPowerModeActive = %s\n", isLowPowerModeActive ? "true" : "false");
    #endif

    tft.init();
    tft.setRotation(1); 
    tft.setTextDatum(MC_DATUM); 

    bool performInitialActionsOnPowerOn = (wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED); 

    if (performInitialActionsOnPowerOn) {
        #if DEBUG_LPM || DEBUG_SCHEDULING
        Serial.println("SETUP: Power-on reset detected. Performing initial data fetch sequence.");
        #endif
        turnScreenOn();
        tft.fillScreen(TFT_BLACK);
        performDataFetchSequence(false); 
    }

    // Initialize schedulers and determine next actions
    struct tm timeinfo_setup;
    if(!getLocalTime(&timeinfo_setup, 10000)){
        Serial.println("SETUP FATAL: Failed to obtain time for initial scheduling! Operations will be unreliable.");
        // Without time, scheduled updates won't work. Display whatever RTC data we have.
        nextUpdateEpochNormalMode = 0; 
        nextUpdateEpochLpm = 0;
        if (isLowPowerModeActive && wakeup_reason != ESP_SLEEP_WAKEUP_EXT0) { // If not button wake, go to fallback sleep
            displayMessage("Time Error", "Sleeping 15m", TFT_RED, true); delay(2000);
            enterDeepSleep(15 * 60 * 1000000ULL, true);
        } else if (isLowPowerModeActive && wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
            // Button wake, screen is on, but no time for proper LPM scheduling. Will timeout.
            temporaryScreenWakeupActive = true; // Already set by printWakeupReason logic if ext0
            screenActiveUntilMs = millis() + SCREEN_ON_DURATION_LPM_MS;
             if (rtc_hasValidData) force_display_update = true; 
            else displayMessage("LPM: No data", "Time Error", TFT_YELLOW, true);
        }
    } else { // Time obtained successfully
        if (isLowPowerModeActive) {
            NextUpdateTimeDetails lpm_details = calculateNextUpdateTimeDetails(timeinfo_setup, UPDATES_PER_HOUR_LPM, REFRESH_TARGET_MINUTE, false);
            nextUpdateEpochLpm = lpm_details.nextUpdateEpoch;

            if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) { 
                #if DEBUG_LPM || DEBUG_SCHEDULING
                Serial.println("LPM: Timer Wake-up. Silent refresh cycle.");
                #endif
                temporaryScreenWakeupActive = false;
                turnScreenOff(); 
                performDataFetchSequence(true); 
                // After fetch, get fresh time and recalculate for next sleep
                if(getLocalTime(&timeinfo_setup, 5000)){
                    lpm_details = calculateNextUpdateTimeDetails(timeinfo_setup, UPDATES_PER_HOUR_LPM, REFRESH_TARGET_MINUTE, false);
                    nextUpdateEpochLpm = lpm_details.nextUpdateEpoch;
                } else { // Time failed after fetch, use old details for sleep duration
                    Serial.println("LPM Timer Wake ERR: Failed to get time post-fetch. Using pre-fetch sleep calc.");
                }
                enterDeepSleep(lpm_details.sleepDurationUs, true);
            } else if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) { 
                #if DEBUG_LPM
                Serial.println("LPM: Button Wake-up (GPIO 0). Temporary screen on.");
                #endif
                temporaryScreenWakeupActive = true;
                screenActiveUntilMs = millis() + SCREEN_ON_DURATION_LPM_MS;
                turnScreenOn();
                tft.fillScreen(TFT_BLACK);
                if (rtc_hasValidData) force_display_update = true; 
                else displayMessage("LPM: No data yet", "Update pending", TFT_YELLOW, true); 
                // nextUpdateEpochLpm is already set from above
            } else { // Power-on into LPM, or toggled to LPM (initial sleep)
                #if DEBUG_LPM || DEBUG_SCHEDULING
                Serial.println("LPM: EEPROM indicated LPM active. Entering LPM cycle (initial sleep).");
                #endif
                temporaryScreenWakeupActive = false;
                if (performInitialActionsOnPowerOn) { 
                    displayMessage("LPM Resuming", "Sleeping...", TFT_BLUE, true); delay(2000); 
                } else { 
                    turnScreenOn(); displayMessage("LPM Active", "Initializing sleep...", TFT_BLUE, true); delay(1500);
                }
                // nextUpdateEpochLpm is set, use lpm_details.sleepDurationUs for this first sleep
                enterDeepSleep(lpm_details.sleepDurationUs, true);
            }
        } else { // Normal Mode
            temporaryScreenWakeupActive = false; 
            turnScreenOn(); 
            NextUpdateTimeDetails normal_details = calculateNextUpdateTimeDetails(timeinfo_setup, UPDATES_PER_HOUR_NORMAL_MODE, REFRESH_TARGET_MINUTE, true);
            if (normal_details.updateNow) {
                #if DEBUG_SCHEDULING
                Serial.println("Normal Mode (Setup): Initial schedule check indicates UPDATE NOW.");
                #endif
                if (!performInitialActionsOnPowerOn) { 
                    performDataFetchSequence(false);
                }
            }
            nextUpdateEpochNormalMode = normal_details.nextUpdateEpoch; 
            if (force_display_update == false && (rtc_hasValidData || performInitialActionsOnPowerOn) ) force_display_update = true;
        }
    }
}


// --- Main Loop ---
void loop() {
    handle_buttons(); 

    if (isLowPowerModeActive) {
        if (temporaryScreenWakeupActive) {
            struct tm timeinfo_lpm_loop;
            if (getLocalTime(&timeinfo_lpm_loop, 1000)) {
                time_t nowEpoch_lpm_loop = mktime(&timeinfo_lpm_loop);

                // Check for scheduled update while screen is on
                if (nextUpdateEpochLpm != 0 && nowEpoch_lpm_loop >= nextUpdateEpochLpm) {
                    #if DEBUG_SCHEDULING || DEBUG_LPM
                    Serial.println("LPM (Screen On): Scheduled update time reached.");
                    #endif
                    performDataFetchSequence(true); // Silent fetch

                    // Reschedule next LPM update
                    if (getLocalTime(&timeinfo_lpm_loop, 5000)) { // Get fresh time
                        NextUpdateTimeDetails details_reschedule_lpm = calculateNextUpdateTimeDetails(timeinfo_lpm_loop, UPDATES_PER_HOUR_LPM, REFRESH_TARGET_MINUTE, false);
                        nextUpdateEpochLpm = details_reschedule_lpm.nextUpdateEpoch;
                    } else {
                        Serial.println("LPM (Screen On) ERR: Failed to get time for rescheduling LPM update!");
                        nextUpdateEpochLpm = nowEpoch_lpm_loop + (((UPDATES_PER_HOUR_LPM > 0) ? (60 / UPDATES_PER_HOUR_LPM) : 60) * 60); // Fallback
                    }
                    screenActiveUntilMs = millis() + SCREEN_ON_DURATION_LPM_MS; // Reset screen on time
                    // dataJustFetched is true from performDataFetchSequence, will trigger display update
                }
            } // else: time failed, can't check schedule, will rely on screen timeout

            // Check for screen timeout
            if (millis() >= screenActiveUntilMs) {
                #if DEBUG_LPM
                Serial.println("LPM: Screen on-time expired. Going back to deep sleep.");
                #endif
                temporaryScreenWakeupActive = false;
                
                struct tm timeinfo_goto_sleep;
                uint64_t sleep_duration_us;
                if(!getLocalTime(&timeinfo_goto_sleep, 5000)){
                     Serial.println("LPM Screen Timeout: Failed to obtain time for sleep calc! Sleeping 15 min.");
                     sleep_duration_us = 15 * 60 * 1000000ULL; 
                     nextUpdateEpochLpm = 0; // Mark as unknown
                } else {
                    time_t nowEpoch_for_sleep = mktime(&timeinfo_goto_sleep);
                    if (nextUpdateEpochLpm != 0 && nextUpdateEpochLpm > nowEpoch_for_sleep) {
                        sleep_duration_us = (nextUpdateEpochLpm - nowEpoch_for_sleep) * 1000000ULL;
                    } else { // nextUpdateEpochLpm was missed, 0, or in the past. Recalculate.
                        #if DEBUG_SCHEDULING
                        Serial.println("LPM Screen Timeout: Recalculating next sleep slot.");
                        #endif
                        NextUpdateTimeDetails sleep_details = calculateNextUpdateTimeDetails(timeinfo_goto_sleep, UPDATES_PER_HOUR_LPM, REFRESH_TARGET_MINUTE, false);
                        nextUpdateEpochLpm = sleep_details.nextUpdateEpoch;
                        sleep_duration_us = sleep_details.sleepDurationUs;
                    }
                }
                enterDeepSleep(sleep_duration_us, true);
            }
        } else { // Not temporaryScreenWakeupActive - should be in deep sleep, this is a fallback
            #if DEBUG_LPM
            Serial.println("LPM: Active, screen off. Fallback to deep sleep from loop (should not happen often).");
            #endif
            struct tm timeinfo_fallback_sleep;
             if(!getLocalTime(&timeinfo_fallback_sleep, 5000)){
                 Serial.println("LPM Fallback Sleep: No time! Sleeping 15 min.");
                 enterDeepSleep(15 * 60 * 1000000ULL, true); 
            } else {
                NextUpdateTimeDetails details = calculateNextUpdateTimeDetails(timeinfo_fallback_sleep, UPDATES_PER_HOUR_LPM, REFRESH_TARGET_MINUTE, false);
                nextUpdateEpochLpm = details.nextUpdateEpoch; // Set for next wake
                enterDeepSleep(details.sleepDurationUs, true);
            }
        }
    } else { // Normal Mode
        unsigned long currentMillis = millis(); 

        if (WiFi.status() == WL_CONNECTED && nextUpdateEpochNormalMode > 0) { 
            struct tm timeinfo_normal_loop;
            if (!getLocalTime(&timeinfo_normal_loop, 1000)) { 
                #if DEBUG_SCHEDULING
                Serial.println("Loop Normal: Failed to get time for update check.");
                #endif
            } else {
                time_t nowEpoch_normal_loop = mktime(&timeinfo_normal_loop);
                if (nowEpoch_normal_loop >= nextUpdateEpochNormalMode) {
                    #if DEBUG_SCHEDULING
                    Serial.println("Normal Mode: Scheduled update time reached.");
                    #endif
                    performDataFetchSequence(false); 

                    if (!getLocalTime(&timeinfo_normal_loop, 5000)) { 
                        Serial.println("Normal Mode ERR: Failed to get time for rescheduling!");
                        nextUpdateEpochNormalMode = nowEpoch_normal_loop + ( ( (UPDATES_PER_HOUR_NORMAL_MODE > 0) ? (60 / UPDATES_PER_HOUR_NORMAL_MODE) : 60 ) * 60 ); 
                    } else {
                        NextUpdateTimeDetails details_reschedule_normal = calculateNextUpdateTimeDetails(timeinfo_normal_loop, UPDATES_PER_HOUR_NORMAL_MODE, REFRESH_TARGET_MINUTE, true);
                        nextUpdateEpochNormalMode = details_reschedule_normal.nextUpdateEpoch;
                         #if DEBUG_SCHEDULING
                         if(details_reschedule_normal.updateNow) Serial.println("Normal Mode: Rescheduler also indicated updateNow. Next slot set.");
                         #endif
                    }
                }
            }
        } else if (WiFi.status() == WL_CONNECTED && nextUpdateEpochNormalMode == 0) { // Scheduler not ready, try to init
            struct tm timeinfo_normal_init_sched;
            if(getLocalTime(&timeinfo_normal_init_sched, 5000)){
                NextUpdateTimeDetails details_init_normal = calculateNextUpdateTimeDetails(timeinfo_normal_init_sched, UPDATES_PER_HOUR_NORMAL_MODE, REFRESH_TARGET_MINUTE, true);
                if(details_init_normal.updateNow) performDataFetchSequence(false);
                nextUpdateEpochNormalMode = details_init_normal.nextUpdateEpoch;
            }
        }

        if (WiFi.status() != WL_CONNECTED && (currentMillis - lastDataFetchAttemptMs >= 60000)) { 
             Serial.println("Normal Mode: No WiFi. Attempting reconnect...");
             connectToWiFi(false); 
             if(WiFi.status() == WL_CONNECTED) {
                Serial.println("Normal Mode: WiFi reconnected. Will fetch at next scheduled time or if initial fetch needed.");
                lastDataFetchAttemptMs = currentMillis; 
                if (nextUpdateEpochNormalMode == 0) { // If scheduler wasn't ready
                    struct tm timeinfo_reconnect_sched;
                    if(getLocalTime(&timeinfo_reconnect_sched, 5000)){
                        NextUpdateTimeDetails details_reconnect_normal = calculateNextUpdateTimeDetails(timeinfo_reconnect_sched, UPDATES_PER_HOUR_NORMAL_MODE, REFRESH_TARGET_MINUTE, true);
                        if(details_reconnect_normal.updateNow) performDataFetchSequence(false); 
                        nextUpdateEpochNormalMode = details_reconnect_normal.nextUpdateEpoch;
                    }
                } else { // Scheduler was ready, check if update is due now that WiFi is back
                    struct tm timeinfo_reconnect_check;
                    if(getLocalTime(&timeinfo_reconnect_check, 1000)) {
                        if (mktime(&timeinfo_reconnect_check) >= nextUpdateEpochNormalMode) {
                             Serial.println("Normal Mode: WiFi reconnected and update is due/overdue. Fetching now.");
                             performDataFetchSequence(false);
                             if(getLocalTime(&timeinfo_reconnect_check, 5000)) { // Get fresh time for reschedule
                                NextUpdateTimeDetails details_post_reconnect_fetch = calculateNextUpdateTimeDetails(timeinfo_reconnect_check, UPDATES_PER_HOUR_NORMAL_MODE, REFRESH_TARGET_MINUTE, true);
                                nextUpdateEpochNormalMode = details_post_reconnect_fetch.nextUpdateEpoch;
                             }
                        }
                    }
                }
             } else { // WiFi reconnect failed
                lastDataFetchAttemptMs = currentMillis; 
                if (!lastUpdateTimeStr.equals("Offline")) { 
                    lastUpdateTimeStr = "Offline";
                    struct tm timeinfo_offline_loop_normal;
                    if (getLocalTime(&timeinfo_offline_loop_normal, 1000)) {
                        for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
                            forecastHours[i] = (timeinfo_offline_loop_normal.tm_hour + i) % 24;
                            hourlyUV[i] = 0.0f;
                        }
                    } else {
                        initializeForecastData(false); 
                    }
                    dataJustFetched = true; 
                }
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

    bool current_info_button_state = digitalRead(BUTTON_INFO_PIN);
    if (current_info_button_state == LOW && button_info_last_state == HIGH && (current_millis - button_info_last_press_time > DEBOUNCE_TIME_MS)) {
        button_info_press_start_time = current_millis;
        button_info_is_held = false;
    } else if (current_info_button_state == LOW && !button_info_is_held) { 
        if (current_millis - button_info_press_start_time > LONG_PRESS_TIME_MS) { 
            if (showInfoOverlay) { 
                useGpsFromSecrets = !useGpsFromSecrets;
                Serial.printf("Location Mode Toggled (Long Press): %s\n", useGpsFromSecrets ? "Secrets GPS" : "IP Geolocation");
                performDataFetchSequence(false); 
                // savePersistentState is called within performDataFetchSequence
                if (isLowPowerModeActive && temporaryScreenWakeupActive) {
                    screenActiveUntilMs = current_millis + SCREEN_ON_DURATION_LPM_MS; 
                }
                // force_display_update is true from performDataFetchSequence
            }
            button_info_is_held = true; 
            button_info_last_press_time = current_millis; 
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
        button_info_last_press_time = current_millis; 
        button_info_is_held = false; 
    }
    button_info_last_state = current_info_button_state;

    bool current_lp_button_state = digitalRead(BUTTON_LP_TOGGLE_PIN);
    if (current_lp_button_state == LOW && button_lp_last_state == HIGH && (current_millis - button_lp_last_press_time > DEBOUNCE_TIME_MS)) {
        button_lp_press_start_time = current_millis;
        button_lp_is_held = false;
    } else if (current_lp_button_state == LOW && !button_lp_is_held) { 
        if (current_millis - button_lp_press_start_time > LONG_PRESS_TIME_MS) { 
            isLowPowerModeActive = !isLowPowerModeActive;
            Serial.printf("LPM Toggled (Long Press): %s\n", isLowPowerModeActive ? "ON" : "OFF");
            savePersistentState(); // Save new LPM state immediately

            if (isLowPowerModeActive) { // Transitioning TO LPM
                temporaryScreenWakeupActive = false; 
                turnScreenOn(); 
                displayMessage("Low Power Mode: ON", "Sleeping...", TFT_BLUE, true);
                delay(2000);
                struct tm timeinfo_lpm_on;
                if(!getLocalTime(&timeinfo_lpm_on, 10000)){
                    Serial.println("LPM Toggle ON FATAL: No time for sleep calc! Sleeping 15 min.");
                    nextUpdateEpochLpm = 0; // Mark as unknown
                    enterDeepSleep(15*60*1000000ULL, true); 
                } else {
                    NextUpdateTimeDetails details_lpm_on = calculateNextUpdateTimeDetails(timeinfo_lpm_on, UPDATES_PER_HOUR_LPM, REFRESH_TARGET_MINUTE, false);
                    nextUpdateEpochLpm = details_lpm_on.nextUpdateEpoch;
                    enterDeepSleep(details_lpm_on.sleepDurationUs, true);
                }
            } else { // Transitioning FROM LPM (to Normal Mode)
                temporaryScreenWakeupActive = false;
                nextUpdateEpochLpm = 0; // Clear LPM scheduler
                turnScreenOn();
                displayMessage("Low Power Mode: OFF", "Refreshing...", TFT_GREEN, true);
                Serial.println("Exiting LPM: Attempting data refresh...");
                performDataFetchSequence(false); 
                
                struct tm timeinfo_lpm_off;
                 if(!getLocalTime(&timeinfo_lpm_off, 10000)){
                    Serial.println("LPM Toggle OFF ERR: No time for normal mode schedule!");
                    nextUpdateEpochNormalMode = 0; 
                } else {
                    NextUpdateTimeDetails details_lpm_off_normal = calculateNextUpdateTimeDetails(timeinfo_lpm_off, UPDATES_PER_HOUR_NORMAL_MODE, REFRESH_TARGET_MINUTE, true);
                    // If updateNow is true, performDataFetchSequence already handled it.
                    nextUpdateEpochNormalMode = details_lpm_off_normal.nextUpdateEpoch;
                     #if DEBUG_SCHEDULING
                     if(details_lpm_off_normal.updateNow && WiFi.status() == WL_CONNECTED) Serial.println("LPM Toggle OFF: Scheduler indicates immediate update (likely just handled).");
                     #endif
                }
                // force_display_update is true from performDataFetchSequence
            }
            button_lp_is_held = true; 
            button_lp_last_press_time = current_millis;
        }
    } else if (current_lp_button_state == HIGH && button_lp_last_state == LOW && (current_millis - button_lp_last_press_time > DEBOUNCE_TIME_MS)) { 
        button_lp_last_press_time = current_millis; 
        button_lp_is_held = false; 
    }
    button_lp_last_state = current_lp_button_state;
}


// --- Display Functions ---
void displayMessage(String msg_line1, String msg_line2, int color, bool allowDisplay) {
    if (!allowDisplay && !(isLowPowerModeActive && temporaryScreenWakeupActive)) {
        #if DEBUG_LPM
        Serial.println("DisplayMessage Skipped (LPM off-screen): " + msg_line1 + " " + msg_line2);
        #endif
        return;
    }
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(color, TFT_BLACK);
    tft.setTextFont(2); 
    tft.setTextDatum(MC_DATUM); 

    int16_t width = tft.width();
    int16_t height = tft.height();

    if (msg_line2 != "") {
        tft.drawString(msg_line1, width / 2, height / 2 - 10);
        tft.drawString(msg_line2, width / 2, height / 2 + 10);
    } else {
        tft.drawString(msg_line1, width / 2, height / 2);
    }
    #if DEBUG_LPM 
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
    int top_y_offset = padding; 

    tft.setTextFont(2);
    int info_font_height = tft.fontHeight(2); 
    int base_top_text_line_y = padding + info_font_height / 2; 

    if (showInfoOverlay) {
        int current_info_y = base_top_text_line_y;
        tft.setTextDatum(TL_DATUM); 

        tft.setTextColor(isLowPowerModeActive ? TFT_ORANGE : TFT_GREEN, TFT_BLACK);
        tft.drawString(isLowPowerModeActive ? "LPM: ON" : "LPM: OFF", padding, current_info_y);
        current_info_y += (info_font_height + padding);

        if (WiFi.status() == WL_CONNECTED) {
            tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
            String ssid_str = WiFi.SSID();
            if (ssid_str.length() > 16) ssid_str = ssid_str.substring(0, 13) + "..."; 
            tft.drawString("WiFi: " + ssid_str, padding, current_info_y);
        } else if (isConnectingToWiFi) { 
             tft.setTextColor(TFT_YELLOW, TFT_BLACK);
             tft.drawString("WiFi: Connecting...", padding, current_info_y);
        }
         else { 
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.drawString("WiFi: Offline", padding, current_info_y);
        }

        tft.setTextDatum(TR_DATUM); 
        tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        String timeToDisplay = lastUpdateTimeStr;
        if (timeToDisplay.length() > 12) timeToDisplay = timeToDisplay.substring(0,9) + "..."; 
        tft.drawString("Upd: " + timeToDisplay, tft.width() - padding, current_info_y);
        current_info_y += (info_font_height + padding);

        tft.setTextDatum(TL_DATUM); 
        tft.setTextColor(TFT_SKYBLUE, TFT_BLACK);
        String locText = "Loc: " + locationDisplayStr + (useGpsFromSecrets ? " (Sec)" : " (IP)");
        if (locText.length() > 30) locText = locText.substring(0, 27) + "..."; 
        tft.drawString(locText, padding, current_info_y);
        top_y_offset = current_info_y + info_font_height / 2 + padding * 2;

    } else { 
        tft.setTextDatum(TR_DATUM); 
        int status_x = tft.width() - padding;
        int status_y = base_top_text_line_y;
        
        if (WiFi.status() != WL_CONNECTED && !isConnectingToWiFi) { 
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.drawString("NoFi", status_x, status_y);
            top_y_offset = base_top_text_line_y + info_font_height / 2 + padding * 2;
        } else if (isConnectingToWiFi) { 
             tft.setTextColor(TFT_YELLOW, TFT_BLACK);
             tft.drawString("WiFi?", status_x, status_y);
             top_y_offset = base_top_text_line_y + info_font_height / 2 + padding * 2;
        }
        if (WiFi.status() == WL_CONNECTED) {
             top_y_offset = padding; 
        }
    }
    drawForecastGraph(top_y_offset);
}

void drawForecastGraph(int start_y_offset) {
    int padding = 2; 
    int first_uv_val_font = 6; 
    int other_uv_val_font = 4; 
    int hour_label_font = 2;   
    const int UV_TEXT_OUTLINE_THICKNESS = 2; // User can adjust this (e.g., 1 or 2) for thicker outline

    tft.setTextFont(first_uv_val_font);
    int first_uv_text_h = tft.fontHeight();
    tft.setTextFont(other_uv_val_font);
    int other_uv_text_h = tft.fontHeight();
    tft.setTextFont(hour_label_font);
    int hour_label_text_h = tft.fontHeight();

    int first_uv_value_y = start_y_offset + padding + first_uv_text_h / 2;
    int other_uv_values_line_y = first_uv_value_y + (first_uv_text_h / 2) - (other_uv_text_h / 2);
    if (HOURLY_FORECAST_COUNT <= 1) other_uv_values_line_y = first_uv_value_y; 

    int hour_label_y = tft.height() - padding - hour_label_text_h / 2;
    int graph_baseline_y = hour_label_y - hour_label_text_h / 2 - padding;

    int max_bar_pixel_height = graph_baseline_y - (start_y_offset + padding);
    if (max_bar_pixel_height < 10) max_bar_pixel_height = 10;
    if (max_bar_pixel_height < 20 && tft.height() > 100) max_bar_pixel_height = 20; 

    const float MAX_UV_FOR_FULL_SCALE = 8.0f; // Changed from 10.0f
    float pixel_per_uv_unit = 0;
    if (MAX_UV_FOR_FULL_SCALE > 0) {
        pixel_per_uv_unit = (float)max_bar_pixel_height / MAX_UV_FOR_FULL_SCALE;
    }

    int graph_area_total_width = tft.width() - 2 * padding; 
    int bar_slot_width = graph_area_total_width / HOURLY_FORECAST_COUNT; 
    int bar_actual_width = bar_slot_width * 0.75; 
    if (bar_actual_width < 4) bar_actual_width = 4; 
    if (bar_actual_width > 30) bar_actual_width = 30; 

    int graph_area_x_start = (tft.width() - (bar_slot_width * HOURLY_FORECAST_COUNT)) / 2 + padding;

    for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
        int bar_center_x = graph_area_x_start + (i * bar_slot_width) + (bar_slot_width / 2);

        tft.setTextFont(hour_label_font);
        tft.setTextColor(TFT_WHITE); // Text color for hour label (transparent background)
        tft.setTextDatum(MC_DATUM); 
        if (forecastHours[i] >= 0 && forecastHours[i] <= 23) { 
            tft.drawString(String(forecastHours[i]), bar_center_x, hour_label_y);
        } else {
            tft.drawString("H?", bar_center_x, hour_label_y); 
        }

        float uvVal = hourlyUV[i];
        int roundedUV;

        if (uvVal == -1.0f) { 
            roundedUV = 0;    
        } else if (uvVal < 0.0f) { 
            roundedUV = 0;    
        } else { 
            roundedUV = round(uvVal);
        }

        if (forecastHours[i] >= 0 && forecastHours[i] <= 23) { 
            float uv_for_height_calc = (float)roundedUV; 
            // Cap bar height at MAX_UV_FOR_FULL_SCALE, but text will show true value
            if (uv_for_height_calc > MAX_UV_FOR_FULL_SCALE) uv_for_height_calc = MAX_UV_FOR_FULL_SCALE; 

            int bar_height = round(uv_for_height_calc * pixel_per_uv_unit);
            if (uvVal > 0 && uvVal < 0.5f && bar_height == 0 && roundedUV == 0) bar_height = 1; 
            else if (roundedUV >= 1 && bar_height == 0 && pixel_per_uv_unit > 0) bar_height = 1; 
            else if (roundedUV >= 1 && bar_height < 2 && pixel_per_uv_unit > 2) bar_height = 2; 

            if (bar_height > max_bar_pixel_height) bar_height = max_bar_pixel_height; 
            if (bar_height < 0) bar_height = 0; 

            int bar_top_y = graph_baseline_y - bar_height; 

            uint16_t barColor;
            if (roundedUV == 0 && uvVal <= 0) barColor = TFT_DARKGREY; 
            else if (roundedUV < 1) barColor = TFT_GREEN; 
            else if (roundedUV <= 2) barColor = TFT_GREEN;    
            else if (roundedUV <= 5) barColor = TFT_YELLOW;   
            else if (roundedUV <= 7) barColor = 0xFC60;       
            else if (roundedUV <= 10) barColor = TFT_RED;     
            else barColor = TFT_MAGENTA;                      

            if (bar_height > 0) {
                tft.fillRect(bar_center_x - bar_actual_width / 2, bar_top_y, bar_actual_width, bar_height, barColor);
            } else if (roundedUV == 0) { 
                 tft.drawFastHLine(bar_center_x - bar_actual_width / 4, graph_baseline_y -1, bar_actual_width / 2, barColor);
            }

            tft.setTextDatum(MC_DATUM); 
            String uvText = String(roundedUV); // Shows actual rounded UV, e.g. "11"
            int current_uv_text_y;
            uint16_t outlineColor = TFT_BLACK; 
            uint16_t foregroundColor = TFT_WHITE; 

            if (i == 0) { 
                tft.setTextFont(first_uv_val_font);
                current_uv_text_y = first_uv_value_y;
            } else { 
                tft.setTextFont(other_uv_val_font);
                current_uv_text_y = other_uv_values_line_y;
            }

            // Draw outline
            tft.setTextColor(outlineColor); // Set outline color (e.g., black)
            for (int ox = -UV_TEXT_OUTLINE_THICKNESS; ox <= UV_TEXT_OUTLINE_THICKNESS; ++ox) {
                for (int oy = -UV_TEXT_OUTLINE_THICKNESS; oy <= UV_TEXT_OUTLINE_THICKNESS; ++oy) {
                    if (ox == 0 && oy == 0) continue; // Don't draw the center with the outline color
                    tft.drawString(uvText, bar_center_x + ox, current_uv_text_y + oy);
                }
            }

            // Draw main text with transparent background
            tft.setTextColor(foregroundColor); // Set foreground color (e.g., white)
            tft.drawString(uvText, bar_center_x, current_uv_text_y);


        } else { 
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
            tft.setTextColor(TFT_DARKGREY); // Transparent background for placeholder
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
    isConnectingToWiFi = true; 
    if (!silent) {
        Serial.println("Connecting to WiFi using secrets.h values...");
        force_display_update = true; 
    }
    #if DEBUG_LPM
    else Serial.println("LPM Silent: Connecting to WiFi...");
    #endif

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true,true); 
    delay(100); 

    bool connected = false;
    String connected_ssid = "";

    const char* ssids[] = {
        WIFI_SSID_1, 
        WIFI_SSID_2
        #if defined(WIFI_SSID_3)
        , WIFI_SSID_3
        #endif
        #if defined(WIFI_SSID_4)
        , WIFI_SSID_4
        #endif
    };
    const char* passwords[] = {
        WIFI_PASS_1, 
        WIFI_PASS_2
        #if defined(WIFI_SSID_3) 
        , WIFI_PASS_3
        #endif
        #if defined(WIFI_SSID_4) 
        , WIFI_PASS_4
        #endif
    };
    int num_networks = sizeof(ssids) / sizeof(ssids[0]);

    for (int i = 0; i < num_networks; ++i) {
        if (strlen(ssids[i]) > 0) { 
            if (!silent) {Serial.print("Attempting SSID: "); Serial.println(ssids[i]);}
            WiFi.begin(ssids[i], passwords[i]);
            unsigned long startTime = millis();
            while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_CONNECTION_TIMEOUT_MS) {
                delay(100);
                if (!silent && (millis() - startTime) % 1000 < 100) Serial.print("."); 
            }

            if (WiFi.status() == WL_CONNECTED) {
                connected = true;
                connected_ssid = ssids[i];
                break; 
            } else {
                if (!silent) Serial.printf("\nFailed to connect to SSID %s.\n", ssids[i]);
                WiFi.disconnect(true,true); 
                delay(100);
            }
        }
    }

    isConnectingToWiFi = false; 
    force_display_update = true; 

    if (connected) {
        if (!silent) {
            Serial.println("\nWiFi connected!");
            Serial.print("SSID: "); Serial.println(connected_ssid);
            Serial.print("IP address: "); Serial.println(WiFi.localIP());
        }
        #if DEBUG_LPM
        else Serial.printf("LPM Silent: WiFi connected to %s, IP: %s\n", connected_ssid.c_str(), WiFi.localIP().toString().c_str());
        #endif
        
        if (!silent) Serial.println("Configuring time via NTP (UTC initial)...");
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

    HTTPClient http;
    String url = "http://ip-api.com/json/?fields=status,message,lat,lon,city"; 

    if (!silent) {Serial.print("Fetching IP Geolocation: "); Serial.println(url);}
    #if DEBUG_LPM
    else Serial.println("LPM Silent: Fetching IP Geolocation...");
    #endif

    http.begin(url);
    http.setTimeout(10000); 
    int httpCode = http.GET();

    if (!silent) {Serial.print("IP Geolocation HTTP Code: "); Serial.println(httpCode);}
    #if DEBUG_LPM
    else Serial.printf("LPM Silent: IP Geo HTTP Code: %d\n", httpCode);
    #endif

    bool success = false;
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        if (!silent) Serial.println("IP Geolocation Payload: " + payload);

        JsonDocument doc; 
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
                
                rtc_deviceLatitude = deviceLatitude;
                rtc_deviceLongitude = deviceLongitude;
                strncpy(rtc_locationDisplayStr_char, locationDisplayStr.c_str(), sizeof(rtc_locationDisplayStr_char) - 1);
                rtc_locationDisplayStr_char[sizeof(rtc_locationDisplayStr_char) - 1] = '\0';
                success = true;
            } else { 
                const char* msg = doc["message"];
                if (!silent) Serial.printf("IP Geolocation API Error: %s\n", msg ? msg : "Unknown error");
                locationDisplayStr = String("IP (API Err)");
            }
        }
    } else { 
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
        struct tm timeinfo_offline_fetch;
        if (getLocalTime(&timeinfo_offline_fetch, 1000)) {
            for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
                forecastHours[i] = (timeinfo_offline_fetch.tm_hour + i) % 24;
                hourlyUV[i] = 0.0f;
                rtc_forecastHours[i] = forecastHours[i]; 
                rtc_hourlyUV[i] = hourlyUV[i];
            }
            rtc_hasValidData = true; 
        } else {
            initializeForecastData(true); 
            rtc_hasValidData = false;
        }
        lastUpdateTimeStr = "Offline";
        strncpy(rtc_lastUpdateTimeStr_char, "Offline", sizeof(rtc_lastUpdateTimeStr_char)-1);
        rtc_lastUpdateTimeStr_char[sizeof(rtc_lastUpdateTimeStr_char)-1] = '\0';
        dataJustFetched = true; 
        return false; 
    }

    HTTPClient http;
    String apiUrl = openMeteoUrl + "?latitude=" + String(deviceLatitude, 4) +
                    "&longitude=" + String(deviceLongitude, 4) +
                    "&hourly=uv_index&forecast_days=1&timezone=auto";

    if (!silent) {Serial.print("Fetching UV Data from URL: "); Serial.println(apiUrl);}
    #if DEBUG_LPM
    else Serial.println("LPM Silent: Fetching UV data...");
    #endif

    http.begin(apiUrl);
    http.setTimeout(15000); 
    int httpCode = http.GET();

    if (!silent) {Serial.print("Open-Meteo API HTTP Code: "); Serial.println(httpCode);}
    #if DEBUG_LPM
    else Serial.printf("LPM Silent: Open-Meteo HTTP Code: %d\n", httpCode);
    #endif

    bool actualDataParsedFromApi = false; 
    rtc_hasValidData = false; 

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        JsonDocument doc; 
        DeserializationError error = deserializeJson(doc, payload);

        if (error) {
            if (!silent) {Serial.print(F("deserializeJson() for UV data failed: ")); Serial.println(error.c_str());}
            #if DEBUG_LPM
            else Serial.printf("LPM Silent: UV JSON deserialize failed: %s\n", error.c_str());
            #endif
            struct tm timeinfo_json_fail;
            if (getLocalTime(&timeinfo_json_fail, 1000)) {
                for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
                    forecastHours[i] = (timeinfo_json_fail.tm_hour + i) % 24;
                    hourlyUV[i] = 0.0f;
                    rtc_forecastHours[i] = forecastHours[i];
                    rtc_hourlyUV[i] = hourlyUV[i];
                }
                rtc_hasValidData = true;
            } else {
                initializeForecastData(true); 
            }
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

            struct tm timeinfo; 
            if (!getLocalTime(&timeinfo, 5000)) { 
                if (!silent) Serial.println("Failed to obtain ESP32 local time for forecast matching after JSON parse.");
                #if DEBUG_LPM
                else Serial.println("LPM Silent: Failed to get local time for forecast matching post-JSON.");
                #endif
                initializeForecastData(true); 
            } else {
                int currentHourLocal = timeinfo.tm_hour;
                int startIndex = -1; 

                if (!doc["hourly"].isNull() && !doc["hourly"]["time"].isNull() && !doc["hourly"]["uv_index"].isNull()) {
                    JsonArray hourly_time_list = doc["hourly"]["time"].as<JsonArray>();
                    JsonArray hourly_uv_list = doc["hourly"]["uv_index"].as<JsonArray>();

                    for (int k = 0; k < hourly_time_list.size(); ++k) {
                        String api_time_str = hourly_time_list[k].as<String>(); 
                        if (api_time_str.length() >= 13) { 
                            int api_hour = api_time_str.substring(11, 13).toInt();
                            if (api_hour >= currentHourLocal) {
                                startIndex = k;
                                break;
                            }
                        }
                    }

                    if (startIndex != -1) { 
                        for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
                            if ((startIndex + i < hourly_uv_list.size()) && (startIndex + i < hourly_time_list.size())) {
                                JsonVariant uv_val_variant = hourly_uv_list[startIndex + i];
                                hourlyUV[i] = uv_val_variant.isNull() ? 0.0f : uv_val_variant.as<float>();
                                if (hourlyUV[i] < 0) hourlyUV[i] = 0.0f; 

                                String api_t_str = hourly_time_list[startIndex + i].as<String>();
                                forecastHours[i] = api_t_str.substring(11, 13).toInt();
                                actualDataParsedFromApi = true; 
                            } else {
                                forecastHours[i] = (currentHourLocal + i) % 24; 
                                hourlyUV[i] = 0.0f;
                            }
                            rtc_hourlyUV[i] = hourlyUV[i];
                            rtc_forecastHours[i] = forecastHours[i];
                        }
                        rtc_hasValidData = true; 
                        if (!silent && actualDataParsedFromApi) Serial.println("Successfully populated forecast data (some/all from API).");
                        else if (!silent) Serial.println("Populated forecast with projections as API data was insufficient/missing for some future slots.");

                    } else { 
                        if (!silent) Serial.println("No suitable starting forecast index in API. Projecting all hours with 0 UV.");
                        for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
                            forecastHours[i] = (currentHourLocal + i) % 24; 
                            hourlyUV[i] = 0.0f;                             
                            rtc_forecastHours[i] = forecastHours[i];
                            rtc_hourlyUV[i] = hourlyUV[i];
                        }
                        rtc_hasValidData = true; 
                    }
                } else { 
                     if (!silent) Serial.println("Hourly data structure missing/incomplete in JSON. Projecting all hours with 0 UV.");
                     for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
                        forecastHours[i] = (currentHourLocal + i) % 24;
                        hourlyUV[i] = 0.0f;
                        rtc_forecastHours[i] = forecastHours[i];
                        rtc_hourlyUV[i] = hourlyUV[i];
                    }
                    rtc_hasValidData = true;
                }
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
        } else { 
            lastUpdateTimeStr = "Time Err";
        }
    } else { 
        struct tm timeinfo_http_fail;
        if (getLocalTime(&timeinfo_http_fail, 1000)) {
            for (int i = 0; i < HOURLY_FORECAST_COUNT; ++i) {
                forecastHours[i] = (timeinfo_http_fail.tm_hour + i) % 24;
                hourlyUV[i] = 0.0f;
                rtc_forecastHours[i] = forecastHours[i];
                rtc_hourlyUV[i] = hourlyUV[i];
            }
            rtc_hasValidData = true;
        } else {
            initializeForecastData(true); 
        }
        if (WiFi.status() != WL_CONNECTED) { 
            lastUpdateTimeStr = "Offline";
        } else { 
            lastUpdateTimeStr = "API Err " + String(httpCode);
        }
    }
    strncpy(rtc_lastUpdateTimeStr_char, lastUpdateTimeStr.c_str(), sizeof(rtc_lastUpdateTimeStr_char)-1);
    rtc_lastUpdateTimeStr_char[sizeof(rtc_lastUpdateTimeStr_char)-1] = '\0';

    http.end();
    dataJustFetched = true; 
    return actualDataParsedFromApi; 
}

