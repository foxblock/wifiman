#ifndef _WIFI_MANAGER_H_INCLUDE
#define _WIFI_MANAGER_H_INCLUDE

#include <stdint.h>
#include <stddef.h>
#include <WiFi.h>

class HardwareSerial;

typedef enum WM_NetworkWorkingState : int8_t {
    NETWORK_STATE_UNKNOWN = -1,
    NETWORK_FAILED_BEFORE = 0, // TODO: More detailed error code
    NETWORK_WORKED_BEFORE = 1
} WM_NetworkWorkingState;

typedef struct WM_WifiNetwork {
    char *ssid = nullptr;
    char *pass = nullptr;
    WM_NetworkWorkingState state = NETWORK_STATE_UNKNOWN;
} WM_WifiNetwork;

// NOTE (JSchaefer, 28.04.23): We cannot get dynamic data directly from the ESP API
// since esp_wifi_scan_get_ap_records deletes the internally allocated memory when
// being called and it is automatically called by the Arduino event loop 
// (WifiGeneric:934 -> WiFiScanClass::_scanDone()).
typedef struct WM_WifiNetworkDisplay {
    uint8_t networkIndex;
    uint8_t scanIndex;
} WM_WifiNetworkDisplay;

typedef enum WM_StatusCode : uint8_t {
    WM_IDLE_STATUS = 0,
    CONNECTING,
    CONNECTED,
    DISCONNECTED,
    NETWORK_NOT_FOUND,
    CONNECTION_FAILED,
} WM_StatusCode;

typedef struct WM_Status {
    uint8_t targetNetwork;
    WM_StatusCode code;
    union {
        uint8_t connectAttempts;
        uint8_t disconnectReason;
    };
} WM_Status;

typedef struct WM_SharedData {
    WM_Status status;
    WM_WifiNetwork **networks;
    uint8_t capacity;
    uint8_t length;
} WM_SharedData;

typedef void (*WM_StatusChangeCallback)(WM_Status *newStatus);

// <0 error
// 0 generic success
// >0 specific success
// check (returnCode >= 0) if you just want to know, if the call succeeded
typedef enum WM_ReturnCode : int8_t {
    WMRT_SIZE_MISMATCH = -4,
    WMRT_SCAN_NOT_READY = -3,
    WMRT_NETWORK_NOT_IN_LIST = -2,
    WMRT_NETWORK_LIST_FULL = -1,
    WMRT_SUCCESS = 0,
    WMRT_NETWORK_UPDATED = 1,
} WM_ReturnCode;

#define WM_SCAN_INTERVAL_DEFAULT_MS 30000

#define WM_RETRIES_NONE 0
#define WM_RETRIES_FAST 1
#define WM_RETRIES_DEFAULT 2
#define WM_RETRIES_CAUTIOUS 3

// Create structure used in all wifiman functions
// Memory will be allocated in this function
// Returns a pointer to the newly created data
// You can pass an existing list of networks (and the capacity of the existing structure), which will be referenced
// If networkList is a nullptr a new structure with the given capacity will be created
// NOTE: max capacity is 254, because 255 (-1) is used for several error cases (like "network not found")
WM_SharedData* wifiman_create(WM_WifiNetwork **networkList, uint8_t capacity);
// Free data and all sub-structures
void wifiman_free(WM_SharedData *data);

// Start wifiman service
// Will attach to certain wifi events to update state of known networks
// keeping track of which work and which fail.
// If autoConnect is true it will try to always keep an active connection
// (i.e. in case of a disconnect), using the best network it can find in 
// range (excluding failed ones).
// A background task is used to periodically scan for wifi networks, if
// the ESP is not currently connected to one.
// Wifiman will not immediately connect to a network when start is called,
// allowing the user to select the first network to connect to (e.g. connect
// to a specific one or just call connectToBestWifi).
// Wifiman will keep a connection for as long as possible and not switch
// even if a "better" network might be available.
void wifiman_start(
        WM_SharedData *data, 
        bool autoConnect, 
        WM_StatusChangeCallback callback = nullptr, 
        uint32_t scanInterval = WM_SCAN_INTERVAL_DEFAULT_MS
        );
// Stop wifiman service
// Removes all events and stops background threads
void wifiman_stop();

// Set interval in which a scan for networks is done (if not currently connected)
void wifiman_setScanInterval(uint32_t newInterval);
uint32_t wifiman_getScanInterval();

// In an ideal world connecting to a wifi will either work or produce the appropriate
// error. In reality an error which looks like wrong-password might be encountered, 
// despite supplying the correct login info. This might be because of low signal strength,
// interferance or package loss in general.
// So it is a good idea to retry connecting a finite amount of times if an error occurs.
// Here you can set the total amount wifiman will attempt to reconnect after an error.
// The default is 2 (WM_RETRIES_DEFAULT), but you can set it to 1 (WM_RETRIES_FAST), if
// you want to switch networks more quickly, or go for additional tries (WM_RETRIES_CAUTIOUS)
// if you are in a difficult environment. Pass 0 (WM_RETRIES_NONE) to disable.
// NOTE: callbacks on error are only called for the final try (after the set retry count)
void wifiman_setRetryCount(uint8_t count);
uint8_t wifiman_getRetryCount();

// Read network data from eeprom and save to data pointer
// Pass values for startIndex and count to restrict to a certain range
// If count is -1 it will read all networks starting at startIndex
// Returns the amount of networs read
uint8_t wifiman_readFromEEPROM(WM_SharedData *data, uint8_t startIndex = 0, uint8_t count = -1);
// Save network data to eeprom
// Pass values for startIndex and count to restrict to a certain range
// If count is -1 it will save all networks starting at startIndex
void wifiman_saveToEEPROM(WM_SharedData *data, uint8_t startIndex = 0, uint8_t count = -1);

// Add new network to list or update an existing entry with the same SSID
// NOTE: Two different networks with the same SSID are currently not supported
// existingUpdated can be used to check if an update happened (pass nullptr if value is not needed)
// Returns index of new or updated entry or -1 on error
uint8_t wifiman_addOrUpdateNetwork(WM_SharedData *data, const char *ssid, const char *pass, bool *existingUpdated = nullptr);
// Delete network from list
// back part of list will be shifted to front, so no gaps are created!
// Returns index of deleted network (or -1 on error)
uint8_t wifiman_deleteNetwork(WM_SharedData *data, uint8_t index);
// Delete network with the given SSID
// Returns index of deleted network (or -1 if network is not in list or other error occurred)
uint8_t wifiman_deleteNetworkByName(WM_SharedData *data, const char *ssid);
// Search for a SSID in the network list
// Returns index if network was found or -1
uint8_t wifiman_findNetworkInList(WM_SharedData *data, const char *ssid);
// Search for a SSID in the network list
// Returns index if network was found or -1
uint8_t wifiman_findNetworkInList(WM_SharedData *data, const uint8_t *ssid, const uint8_t ssidLen);
// Count all networks that are suitable for auto connection
// This includes networks with state UNKNOWN or WORKED_BEFORE
uint8_t wifiman_countUsableNetworks(WM_SharedData *data);

// Connect to the network with the given index
WM_ReturnCode wifiman_connectToNetwork(WM_SharedData *data, uint8_t index);
// Connect to the known network with the lowest RSSI currently in range
// This requires an active network scan result. If that is not present
// it will start a scan and return the respective error code.
WM_ReturnCode wifiman_connectToBestWifi(WM_SharedData *data);

// Print WM_SharedData structure to Serial in human readable form
void wifiman_print(WM_SharedData *data, HardwareSerial *output);

// Fill the passed networks array with results from wifi scan and compare to saved networks.
// Networks will have the same order as in the scan results and their index in wifiman_data
// (if matching SSID is found - else -1)
// 
// Returns
//      WMRT_SUCCESS if successful
//      WMRT_SIZE_MISMATCH if networks is not large enough to fit scan results
//      WMRT_SCAN_NOT_READY if no scan results are available
//      WMRT_NETWORK_NOT_IN_LIST if scan is empty
WM_ReturnCode wifiman_getDisplayFilterByScan(WM_WifiNetworkDisplay networks[], uint8_t count);

// Fill the passed networks array with networks from wifiman_data and compare to scan results.
// Pass a scanFilter array (result of a wifiman_getDisplayFilterByScan call), else the
// function will use the current scan results (if there are any)
// Networks will have the same order as in wifiman_data and their scanIndex set (-1 if not in scan)
//
// Returns
//      WMRT_SUCCESS if successful (also if no filter passed and no scan result active)
//      WMRT_SIZE_MISMATCH if networks is not large enough to fit all wifiman_data networks
WM_ReturnCode wifiman_getDisplayFilterBySaved(WM_WifiNetworkDisplay networks[], uint8_t count,
        WM_WifiNetworkDisplay scanFilter[] = nullptr, uint8_t scanCount = 0);

#endif // _WIFI_MANAGER_H_INCLUDE
