//RENAME TO: secrets.h

#ifndef SECRETS_H
#define SECRETS_H

// WiFi Credentials
#define WIFI_SSID_2 "YOUR_WIFI_SSID_1"
#define WIFI_PASS_2 "YOUR_WIFI_PASSWORD_1"

#define WIFI_SSID_2 "YOUR_WIFI_SSID_2"
#define WIFI_PASS_2 "YOUR_WIFI_PASSWORD_2"
// Add more if needed, e.g., WIFI_SSID_3, WIFI_PASS_3

// Location
#define MY_LATITUDE 25.2697f  
#define MY_LONGITUDE 55.3095f   

// Timezone Configuration (for accurate "Last Updated" time)
// Find your offset here: https://en.wikipedia.org/wiki/List_of_UTC_time_offsets
// Example for Central Time (USA) which is UTC-6 or UTC-5 with DST
#define MY_GMT_OFFSET_SEC (-6 * 3600)      // Standard Time Offset (e.g., CST is UTC-6)
#define MY_DAYLIGHT_OFFSET_SEC (1 * 3600)  // Daylight Saving Offset (e.g., +1 hour for CDT)
// // For locations without DST, set MY_DAYLIGHT_OFFSET_SEC to 0

// Timezone Configuration FOR DUBAI (GST = UTC+4, no DST)
// #define MY_GMT_OFFSET_SEC (4 * 3600)      // UTC+4 hours
// #define MY_DAYLIGHT_OFFSET_SEC 0          // No daylight saving


#endif // SECRETS_H