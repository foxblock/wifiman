#include "wifi_manager.h"

#include <Preferences.h>

typedef unsigned long ArduinoTime_t;

#define WM_PREFERENCES_NAMESPACE "wifiman" // max 15 chars
#define WM_PREFERENCES_KEY_SSID "ssid%d" // max 15 chars
#define WM_PREFERENCES_KEY_PASS "pass%d"
#define WM_PREFERENCES_KEY_STATE "stat%d"

#define WM_SCAN_MAX_AGE_MS 60000

static WM_SharedData* _wifiman_data = nullptr;
static bool _wifiman_autoConnect = false;
static uint32_t _wifiman_scanInterval = WM_SCAN_INTERVAL_DEFAULT_MS;
static TaskHandle_t _wifiman_workerTaskHandle = nullptr;
static WM_StatusChangeCallback _wifiman_statusCallback = nullptr;
static uint8_t _wifiman_maxRetries = WM_RETRIES_DEFAULT;

static ArduinoTime_t _wifiman_scanTime = 0;
static uint8_t _wifiman_retryCount = 0;

static void _wifiman_checkConnection();
static void _wifiman_wifiConnectedEvent(arduino_event_t *event);
static void _wifiman_wifiDisconnectedEvent(arduino_event_t *event);
static void _wifiman_wifiScanDoneEvent(arduino_event_t *event);
static void _wifiman_workerTask(void *parameters);
static void _wifiman_scanResume();
static void _wifiman_scanPause();
static void _wifiman_doScan(ArduinoTime_t when);
static void _wifiman_connect(uint8_t index, bool byUser, ArduinoTime_t when);
static inline bool _time_now_or_passed(ArduinoTime_t timeToTest, ArduinoTime_t now);

struct _WM_WifiConnect
{
    SemaphoreHandle_t lock;
    ArduinoTime_t execTime = 0;
    uint8_t networkIndex = 0;
    bool issuedByUser = true;
    bool handled = true; // make sure to set this last when issueing new command
};

struct _WM_WifiScan
{
    SemaphoreHandle_t lock;
    ArduinoTime_t execTime = 0;
    bool handled = true; // make sure to set this last when issueing new command
};

_WM_WifiConnect nextConnect;
_WM_WifiScan nextScan;

WM_SharedData* wifiman_create(WM_WifiNetwork **networkList, uint8_t capacity)
{
    if (capacity == 0 || capacity == (uint8_t)-1)
        return nullptr;

    WM_SharedData *result = (WM_SharedData*)malloc(sizeof(WM_SharedData));

    if (networkList == nullptr)
    {
        networkList = (WM_WifiNetwork**)malloc(sizeof(networkList[0]) * capacity);
        // set everything to 0, not technically needed, but makes memory a bit easier to read during debugging
        memset(networkList, 0, sizeof(networkList[0]) * capacity); 
        result->length = 0;
    }
    else
    {
        for (int i = 0; i < capacity; ++i)
        {
            if (networkList[i] != nullptr)
                continue;

            result->length = i;
            break;
        }
    }
    result->networks = networkList;
    result->capacity = capacity;

    result->status.targetNetwork = -1;
    result->status.code = WM_IDLE_STATUS;

    return result;
}

void wifiman_free(WM_SharedData *data)
{
    if (data == nullptr)
        return;

    if (data->networks == nullptr)
    {
        free(data);
        return;
    }

    for (int i = 0; i < data->length; ++i)
    {
        free(data->networks[i]->ssid);
        free(data->networks[i]->pass);
        free(data->networks[i]);
    }

    free(data->networks);
    free(data);

    return;
}

void wifiman_start(WM_SharedData *data, bool autoConnect, WM_StatusChangeCallback callback, uint32_t scanInterval)
{
    assert(data != nullptr);
    assert(_wifiman_data == nullptr);

    auto temp = WiFi.onEvent(_wifiman_wifiConnectedEvent, ARDUINO_EVENT_WIFI_STA_CONNECTED);
    assert(temp != 0);
    temp = WiFi.onEvent(_wifiman_wifiDisconnectedEvent, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    assert(temp != 0);
    _wifiman_data = data;
    _wifiman_autoConnect = autoConnect;
    _wifiman_scanInterval = scanInterval;
    _wifiman_statusCallback = callback;

    nextConnect.handled = true;
    nextScan.handled = true;
    nextConnect.lock = xSemaphoreCreateMutex();
    nextScan.lock = xSemaphoreCreateMutex();

    if (_wifiman_autoConnect)
    {
        WiFi.onEvent(_wifiman_wifiScanDoneEvent, ARDUINO_EVENT_WIFI_SCAN_DONE);
        
        // We need to disable auto reconnect, else it interferes with our autoConnect
        // The auto reconnect calls WiFi.disconnect and WiFi.begin on each disconnect 
        // event (WiFiGeneric.cpp:975), which will stop/invalidate our background 
        // wifi scan for connectToBestWifi
        // This will still happen once each startup because of WiFiGeneric.cpp:966
        WiFi.setAutoReconnect(false);
    }

    xTaskCreatePinnedToCore(
            _wifiman_workerTask,
            "WifimanWorker",
            3072, // watermark shows max. 1828 - 1940 bytes usage
            nullptr,
            1,
            &_wifiman_workerTaskHandle,
            0);
}

void wifiman_stop()
{
    WiFi.removeEvent(_wifiman_wifiConnectedEvent, ARDUINO_EVENT_WIFI_STA_CONNECTED);
    WiFi.removeEvent(_wifiman_wifiDisconnectedEvent, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

    if (_wifiman_autoConnect)
    {
        WiFi.removeEvent(_wifiman_wifiScanDoneEvent, ARDUINO_EVENT_WIFI_SCAN_DONE);
        vTaskDelete(_wifiman_workerTaskHandle);
        _wifiman_workerTaskHandle = nullptr;
    }

    vSemaphoreDelete(nextConnect.lock);
    vSemaphoreDelete(nextScan.lock);
    _wifiman_data = nullptr;
}

void wifiman_setScanInterval(uint32_t newInterval)
{
    _wifiman_scanInterval = newInterval;
}

uint32_t wifiman_getScanInterval()
{
    return _wifiman_scanInterval;
}

void wifiman_setRetryCount(uint8_t count)
{
    _wifiman_maxRetries = count;
}

uint8_t wifiman_getRetryCount()
{
    return _wifiman_maxRetries;
}

// NOTE (JSchaefer, 05.08.23): Try to minimize use of pref.isKey, since it is suuuper
// wasteful and badly implemented.
// we will just call the API functions with possibly invalid keys, which seems to be
// fine. It generates an error log, but is handled internally and does not crash.
uint8_t wifiman_readFromEEPROM(WM_SharedData *data, uint8_t startIndex, uint8_t count)
{
    if (data == nullptr)
        return 0;

    Preferences pref;
    pref.begin(WM_PREFERENCES_NAMESPACE, true);

    char keySSID[16] = "";
    char keyPass[16] = "";
    char keyState[16] = "";
    // TODO: Read ssid and pass directly to target char*
    // TODO: Use char[] with fixed length instead of char* in WM_WifiNetwork ??
    String valueSSID;
    String valuePass;

    uint8_t entriesRead = 0;

    for (int i = startIndex; i < startIndex + count && i < data->capacity; ++i)
    {
        snprintf(keySSID, 16, WM_PREFERENCES_KEY_SSID, i);
        
        if (! pref.isKey(keySSID))
            break;

        if (i < data->length)
        {
            free(data->networks[i]->ssid);
            free(data->networks[i]->pass);
        }
        else
        {
            data->networks[i] = (WM_WifiNetwork*)malloc(sizeof(WM_WifiNetwork));
            ++(data->length);
        }

        valueSSID = pref.getString(keySSID, "");
        data->networks[i]->ssid = strdup(valueSSID.c_str());

        snprintf(keyPass, 16, WM_PREFERENCES_KEY_PASS, i);
        valuePass = pref.getString(keyPass, "");
        if (valuePass[0] != 0)
            data->networks[i]->pass = strdup(valuePass.c_str());
        else
            data->networks[i]->pass = nullptr;

        snprintf(keyState, 16, WM_PREFERENCES_KEY_STATE, i);
        data->networks[i]->state = (WM_NetworkWorkingState)pref.getChar(keyState, 0);

        ++entriesRead;
    }

    pref.end();

    return entriesRead;
}

void wifiman_saveToEEPROM(WM_SharedData *data, uint8_t startIndex, uint8_t count)
{
    if (data == nullptr || count == 0)
        return;

    if (count == (uint8_t)-1)
        count = data->capacity - startIndex;

    Preferences pref;
    pref.begin(WM_PREFERENCES_NAMESPACE, false);

    char keySSID[16] = "";
    char keyPass[16] = "";
    char keyState[16] = "";

    for (int i = startIndex; i < startIndex + count && i < data->capacity; ++i)
    {
        snprintf(keySSID, 16, WM_PREFERENCES_KEY_SSID, i);
        snprintf(keyPass, 16, WM_PREFERENCES_KEY_PASS, i);
        snprintf(keyState, 16, WM_PREFERENCES_KEY_STATE, i);

        if (i < data->length)
        {
            pref.putString(keySSID, data->networks[i]->ssid);
            if (data->networks[i]->pass != nullptr)
                pref.putString(keyPass, data->networks[i]->pass);
            pref.putChar(keyState, data->networks[i]->state);
        }
        else
        {
            if (! pref.isKey(keySSID))
                break;

            pref.remove(keySSID);
            pref.remove(keyPass);
            pref.remove(keyState);
        }
    }

    pref.end();
}

uint8_t wifiman_addOrUpdateNetwork(WM_SharedData *data, const char *ssid, const char *pass, bool *existingUpdated)
{
    // TODO: Check length of ssid and pass? (look up what max for Wifi functions is and how wifi handles input that is too long)
    if (data == nullptr || ssid == nullptr)
        return -1;

    for (int i = 0; i < data->length; ++i)
    {
        if (strcmp(data->networks[i]->ssid, ssid) != 0)
            continue;

        free(data->networks[i]->pass);
        data->networks[i]->pass = (pass == nullptr ? nullptr : strdup(pass));
        data->networks[i]->state = NETWORK_STATE_UNKNOWN;

        if (existingUpdated != nullptr)
            *existingUpdated = true;

        return i;
    }

    if (data->length == data->capacity)
        return -1;

    data->networks[data->length] = (WM_WifiNetwork*)malloc(sizeof(WM_WifiNetwork));
    data->networks[data->length]->ssid = strdup(ssid);
    data->networks[data->length]->pass = (pass == nullptr ? nullptr : strdup(pass));
    data->networks[data->length]->state = NETWORK_STATE_UNKNOWN;

    if (existingUpdated != nullptr)
        *existingUpdated = false;

    ++(data->length);

    return (data->length - 1);
}

uint8_t wifiman_deleteNetworkByName(WM_SharedData *data, const char *ssid)
{
    return wifiman_deleteNetwork(data, wifiman_findNetworkInList(data, ssid));
}

uint8_t wifiman_deleteNetwork(WM_SharedData *data, uint8_t index)
{
    if (data == nullptr || index >= data->length || data->networks[index] == nullptr)
        return -1;

    free(data->networks[index]->ssid);
    free(data->networks[index]->pass);
    free(data->networks[index]);

    memmove(data->networks + index, data->networks + index + 1, sizeof(data->networks[0]) * (data->length - index - 1));

    data->networks[--(data->length)] = nullptr;

    if (data->status.targetNetwork == index)
        data->status.targetNetwork = -1;
    else if (data->status.targetNetwork > index && data->status.targetNetwork != (uint8_t)-1)
        --(data->status.targetNetwork);

    return index;
}

uint8_t wifiman_findNetworkInList(WM_SharedData *data, const char *ssid)
{
    if (data == nullptr || ssid == nullptr || ssid[0] == 0)
        return -1;

    for (int i = 0; i < data->length; ++i)
    {
        if (strcmp(data->networks[i]->ssid, ssid) != 0)
            continue;

        return i;
    }

    return -1;
}

uint8_t wifiman_findNetworkInList(WM_SharedData *data, const uint8_t *ssid, const uint8_t ssidLen)
{
    if (data == nullptr || ssid == nullptr || ssidLen == 0 || ssid[0] == 0)
        return -1;

    for (int i = 0; i < data->length; ++i)
    {
        if (ssidLen != strlen(data->networks[i]->ssid))
            continue;
        if (memcmp(data->networks[i]->ssid, ssid, ssidLen) != 0)
            continue;

        return i;
    }

    return -1;
}

uint8_t wifiman_countUsableNetworks(WM_SharedData *data)
{
    if (data == nullptr)
        return 0;

    if (data->length == 0)
        return 0;

    uint8_t count = 0;

    for (int i = 0; i < data->length; ++i)
    {
        if (data->networks[i]->state != 0)
            ++count;
    }

    return count;
}

WM_ReturnCode wifiman_connectToNetwork(WM_SharedData *data, uint8_t index)
{
    assert(data != nullptr);
    assert(index < data->length);

    Serial.printf("[WIFIMAN] Manual connection to \"%s\"\n", data->networks[index]->ssid);
    _wifiman_connect(index, true, 0);

    _wifiman_retryCount = 0;

    data->status.code = CONNECTING;
    data->status.targetNetwork = index;
    if (_wifiman_statusCallback != nullptr)
        _wifiman_statusCallback(&data->status);

    return WMRT_SUCCESS;
}

WM_ReturnCode wifiman_connectToBestWifi(WM_SharedData *data)
{
    assert(data != nullptr);

    if (data->length == 0)
        return WMRT_NETWORK_NOT_IN_LIST;

    Serial.print("[WIFIMAN] Connecting to best wifi...\n");

    if (millis() - _wifiman_scanTime > WM_SCAN_MAX_AGE_MS)
    {
        Serial.print("[WIFIMAN] Results are old, issuing new scan...\n");

        _wifiman_doScan(0);
        _wifiman_scanTime = millis();

        return WMRT_SCAN_NOT_READY;
    }

    auto scanResult = WiFi.scanComplete();

    switch (scanResult)
    {
        case -2: // NOT STARTED 
            _wifiman_doScan(0);
            return WMRT_SCAN_NOT_READY;
        case -1:  // RUNNING
            return WMRT_SCAN_NOT_READY;
        case 0:
            return WMRT_NETWORK_NOT_IN_LIST;
    }

    int bestRSSI = INT_MIN;
    int bestIndex = -1;

    for (int i = 0; i < scanResult; ++i)
    {
        uint8_t result = wifiman_findNetworkInList(data, WiFi.SSID(i).c_str());

        if (result >= data->length || data->networks[result]->state == NETWORK_FAILED_BEFORE)
            continue;
        
        if (WiFi.RSSI(i) > bestRSSI)
        {
            bestRSSI = WiFi.RSSI(i);
            bestIndex = result;
        }
    }

    if (bestIndex == -1)
    {
        //// EXPERIMENTAL reset all bad networks -> will retry after next scan interval
        for (int i = 0; i < data->length; ++i)
        {
            if (data->networks[i]->state == NETWORK_FAILED_BEFORE)
                data->networks[i]->state = NETWORK_STATE_UNKNOWN;
        }
        //// EXPERIMENTAL
        return WMRT_NETWORK_NOT_IN_LIST;
    }

    Serial.printf("[WIFIMAN] Connecting to \"%s\"\n", data->networks[bestIndex]->ssid);
    _wifiman_connect(bestIndex, true, 0);

    _wifiman_retryCount = 0;

    data->status.code = CONNECTING;
    data->status.targetNetwork = bestIndex;
    if (_wifiman_statusCallback != nullptr)
        _wifiman_statusCallback(&data->status);

    return WMRT_SUCCESS;
}

void wifiman_print(WM_SharedData *data, HardwareSerial *output)
{
    if (data == nullptr)
    {
        output->print("wifiman_print: data ptr is NULL!\n");
        return;
    }

    output->printf("--- WM_SharedData @ %p ---\n", data);
    output->printf("Network list: %d of %d set @ %p\n", data->length, data->capacity, data->networks);
    output->print("[#] SSID --- Password --- State\n");
    for (int i = 0; i < data->length; ++i)
    {
        output->printf("[%d] %s --- %s --- %d @ %p\n", 
                i, 
                data->networks[i]->ssid, 
                data->networks[i]->pass == nullptr ? "[none]" : data->networks[i]->pass, 
                data->networks[i]->state, 
                data->networks[i]);
    }
    output->printf("[%d] %p\n", data->length, data->networks[data->length]);
}

WM_ReturnCode wifiman_getDisplayFilterByScan(WM_WifiNetworkDisplay networks[], uint8_t count)
{
    assert(networks != nullptr);
    assert(count > 0);

    auto scanResult = WiFi.scanComplete();

    switch (scanResult)
    {
        case -2: // NOT STARTED 
        case -1: // RUNNING
            return WMRT_SCAN_NOT_READY;
        case 0: // NO NETWORK IN RANGE
            return WMRT_NETWORK_NOT_IN_LIST;
    }

    if (count < scanResult)
        return WMRT_SIZE_MISMATCH;

    for (int i = 0; i < scanResult; ++i)
    {
        networks[i].scanIndex = i;
        networks[i].networkIndex = wifiman_findNetworkInList(_wifiman_data, WiFi.SSID(i).c_str());
    }

    return WMRT_SUCCESS;
}

WM_ReturnCode wifiman_getDisplayFilterBySaved(WM_WifiNetworkDisplay networks[], uint8_t count,
        WM_WifiNetworkDisplay scanFilter[], uint8_t scanCount)
{
    assert(networks != nullptr);
    assert(count > 0);

    if (count < _wifiman_data->length)
        return WMRT_SIZE_MISMATCH;

    for (int i = 0; i < _wifiman_data->length; ++i)
    {
        networks[i].networkIndex = i;
        networks[i].scanIndex = -1;
    }

    auto scanResult = WiFi.scanComplete();

    if (scanFilter == nullptr && scanResult <= 0)
        return WMRT_SUCCESS;

    if (scanFilter != nullptr)
    {
        for (int i = 0; i < scanCount; ++i)
        {
            if (scanFilter[i].networkIndex < count)
                networks[scanFilter[i].networkIndex].scanIndex = scanFilter[i].scanIndex;
        }
    }
    else
    {
        for (int i = 0; i < scanResult; ++i)
        {
            uint8_t found = wifiman_findNetworkInList(_wifiman_data, WiFi.SSID(i).c_str());
            if (found < count)
                networks[found].scanIndex = i;
        }
    }

    return WMRT_SUCCESS;
}

static void _wifiman_checkConnection()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.print("[WIFIMAN] Checking connection...already connected\n");
        return;
    }
    if (wifiman_countUsableNetworks(_wifiman_data) == 0)
    {
        Serial.print("[WIFIMAN] Checking connection...no usable networks\n");
        return;
    }

    Serial.print("[WIFIMAN] Checking connection...trying to connect\n");
    int8_t status = wifiman_connectToBestWifi(_wifiman_data);

    Serial.printf("[WIFIMAN] connect to best wifi returned: %d\n", status);

    // Turn on periodic background scans, if no saved network is in range
    // so we connect as soon, as a suitable network moves into range
    if (status == WMRT_NETWORK_NOT_IN_LIST)
        _wifiman_scanResume();
    else
        _wifiman_scanPause();
}

static void _wifiman_wifiConnectedEvent(arduino_event_t *event)
{
    Serial.printf("[WIFIMAN] Connected to \"%.*s\" after %d attempts\n", 
        event->event_info.wifi_sta_connected.ssid_len,
        (char*)(event->event_info.wifi_sta_connected.ssid), 
        _wifiman_retryCount + 1);

    uint8_t index = wifiman_findNetworkInList(_wifiman_data, event->event_info.wifi_sta_connected.ssid, event->event_info.wifi_sta_connected.ssid_len);
    
    _wifiman_data->status.code = CONNECTED;
    _wifiman_data->status.targetNetwork = index;
    _wifiman_data->status.connectAttempts = _wifiman_retryCount + 1;
    if (_wifiman_statusCallback != nullptr)
        _wifiman_statusCallback(&_wifiman_data->status);
    
    if (index >= _wifiman_data->length)
        return;

    _wifiman_retryCount = 0;

    _wifiman_data->networks[index]->state = NETWORK_WORKED_BEFORE;

    if (_wifiman_autoConnect)
        _wifiman_scanPause();
}

static void _wifiman_wifiDisconnectedEvent(arduino_event_t *event)
{
    Serial.printf("[WIFIMAN] Disconnected from \"%.*s\", reason: %d\n", 
        event->event_info.wifi_sta_disconnected.ssid_len,
        (char*)(event->event_info.wifi_sta_disconnected.ssid), 
        event->event_info.wifi_sta_disconnected.reason);

    uint8_t index = wifiman_findNetworkInList(_wifiman_data, event->event_info.wifi_sta_disconnected.ssid, event->event_info.wifi_sta_disconnected.ssid_len);

    // https://espressif-docs.readthedocs-hosted.com/projects/espressif-esp-faq/en/latest/software-framework/wifi.html#connect-while-esp32-connecting-wi-fi-how-can-i-determine-the-reason-of-failure-by-error-codes
    // https://github.com/espressif/esp-idf/issues/3349#issuecomment-485764274
    // https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/wifi.html#wi-fi-reason-code
    switch (event->event_info.wifi_sta_disconnected.reason)
    {
        case WIFI_REASON_ASSOC_LEAVE: // after calling WiFi.disconnect()
        case WIFI_REASON_AUTH_LEAVE:  // network was shut down
            _wifiman_data->status.code = DISCONNECTED;
            break;
        case WIFI_REASON_NO_AP_FOUND: // SSID not found
            _wifiman_data->status.code = NETWORK_NOT_FOUND;
            break;
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: // wrong password
        case WIFI_REASON_HANDSHAKE_TIMEOUT: // wrong password (less common)
        case WIFI_REASON_AUTH_FAIL: // generic fail (happens sometimes, hard to pin down)
        case WIFI_REASON_AUTH_EXPIRE: // i.e. when reconnecting to phone hotspot with phone on standby
        default:
            if (index < _wifiman_data->length && _wifiman_retryCount >= _wifiman_maxRetries)
                _wifiman_data->networks[index]->state = NETWORK_FAILED_BEFORE;
            _wifiman_data->status.code = CONNECTION_FAILED;
            break;
    }
    
    _wifiman_data->status.targetNetwork = index;
    _wifiman_data->status.disconnectReason = event->event_info.wifi_sta_disconnected.reason;

    if (index < _wifiman_data->length && 
            _wifiman_retryCount < _wifiman_maxRetries && 
            event->event_info.wifi_sta_disconnected.reason != WIFI_REASON_ASSOC_LEAVE)
    {
        Serial.printf("[WIFIMAN] Attempting to reconnect to %s (attempt #%d)\n", (char*)(event->event_info.wifi_sta_disconnected.ssid), _wifiman_retryCount + 1);

        // connect after 1 - 2 - 4 - 8 - ... seconds
        _wifiman_connect(index, false, _wifiman_retryCount >= 3 ? 8000 : 1000 * (1 << _wifiman_retryCount));

        ++_wifiman_retryCount;
    }
    else 
    {
        if (_wifiman_statusCallback != nullptr)
            _wifiman_statusCallback(&_wifiman_data->status);

        if (_wifiman_autoConnect && event->event_info.wifi_sta_disconnected.reason != WIFI_REASON_ASSOC_LEAVE)
            _wifiman_checkConnection();
    }
}

static void _wifiman_wifiScanDoneEvent(arduino_event_t *event)
{
    Serial.printf("[WIFIMAN] Scan done! Networks found: %d, scan id %d, status %d\n", event->event_info.wifi_scan_done.number, event->event_info.wifi_scan_done.scan_id, event->event_info.wifi_scan_done.status);

    _wifiman_scanTime = millis();

    _wifiman_checkConnection();
}

static void _wifiman_scanResume()
{
    Serial.print("[WIFIMAN] Resuming wifi scan thread\n");
    xTaskNotify(_wifiman_workerTaskHandle, 1, eSetValueWithOverwrite);
}

static void _wifiman_scanPause()
{
    Serial.print("[WIFIMAN] Pausing wifi scan thread\n");
    xTaskNotify(_wifiman_workerTaskHandle, 0, eSetValueWithOverwrite);
}

static void _wifiman_doScan(ArduinoTime_t delay)
{
    Serial.printf("[WIFIMAN] Issuing scan command: %lu...\n", delay);

    xSemaphoreTake(nextScan.lock, portMAX_DELAY);

    nextScan.execTime = millis() + delay;
    nextScan.handled = false;

    xSemaphoreGive(nextScan.lock);
}

static void _wifiman_connect(uint8_t index, bool byUser, ArduinoTime_t delay)
{
    Serial.printf("[WIFIMAN] Issuing connect command: %d, %d, %lu...\n", index, byUser, delay);

    xSemaphoreTake(nextConnect.lock, portMAX_DELAY);

    nextConnect.execTime = millis() + delay;
    nextConnect.networkIndex = index;
    nextConnect.issuedByUser = byUser;
    nextConnect.handled = false;

    xSemaphoreGive(nextConnect.lock);
}

static void _wifiman_workerTask(void *parameters)
{
    Serial.print("[WIFIMAN-THREAD] worker task: started.\n");

    uint32_t notifyValue;
    _WM_WifiConnect connect;
    _WM_WifiScan scan;

    while (true)
    {
        // When other threads issue new commands -> copy to internal buffer
        // so we reduce the amount of locks and unlocks done
        if (! nextConnect.handled)
        {
            Serial.print("[WIFIMAN-THREAD] Getting new connect cmd...\n");

            xSemaphoreTake(nextConnect.lock, portMAX_DELAY);
            // Do not let automatic reconnects (not issued by user) overwrite
            // manual connect orders by user
            if (nextConnect.issuedByUser || connect.handled || ! connect.issuedByUser)
            {
                connect = nextConnect;
                nextConnect.handled = true;
            }
            xSemaphoreGive(nextConnect.lock);
        }

        if (! nextScan.handled)
        {
            Serial.print("[WIFIMAN-THREAD] Getting new scan cmd...\n");

            xSemaphoreTake(nextScan.lock, portMAX_DELAY);
            scan = nextScan;
            nextScan.handled = true;
            xSemaphoreGive(nextScan.lock);
        }

        xTaskNotifyWait(0, 0, &notifyValue, 0);

        if (! connect.handled && _time_now_or_passed(connect.execTime, millis()))
        {
            Serial.printf("[WIFIMAN-THREAD] connecting to network: %s...\n", _wifiman_data->networks[connect.networkIndex]->ssid);

            WiFi.disconnect();
            WiFi.begin(_wifiman_data->networks[connect.networkIndex]->ssid, 
                    _wifiman_data->networks[connect.networkIndex]->pass);
            connect.handled = true;
        }

        if ((! scan.handled || notifyValue != 0) && _time_now_or_passed(scan.execTime, millis()))
        {
            Serial.printf("[WIFIMAN-THREAD] doing %sWiFi scan...\n", notifyValue != 0 ? "PERIODIC " : "");

            if (WiFi.scanComplete() != WIFI_SCAN_RUNNING)
            {
                WiFi.scanDelete();
                WiFi.scanNetworks(true);
            }

            if (notifyValue != 0)
                scan.execTime = scan.execTime + _wifiman_scanInterval;

            scan.handled = true;
        }

#ifdef _DEBUG
        static unsigned long printTime = -300000;
        if (millis() - printTime > 300000)
        {
            Serial.printf("[WIFIMAN-THREAD] thread watermark: %d\n", uxTaskGetStackHighWaterMark(NULL));
            printTime = millis();
        }
#endif

        delay(1);
    }

    Serial.print("[WIFIMAN-THREAD] connectivity task: stopping.\n");

    vTaskDelete(nullptr);
}

// https://arduino.stackexchange.com/questions/12587/how-can-i-handle-the-millis-rollover
static inline bool _time_now_or_passed(ArduinoTime_t timeToTest, ArduinoTime_t now)
{
    return ((timeToTest - now - 1ul) & 0x80000000);
}