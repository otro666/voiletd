#pragma once
#include "Arduino.h"
struct WiFiClass {
    bool isConnected();
    IPAddress localIP();
    IPAddress subnetMask();
    IPAddress gatewayIP();
    int RSSI();
    int RSSI(int);
    String SSID();
    String SSID(int);
    bool isHidden(int);
    int encryptionType(int);
    void setSleep(bool);
    void setAutoReconnect(bool);
    void scanDelete();
    String macAddress();
    int status();
    void mode(int);
    void begin(const char*, const char* = nullptr);
    bool setHostname(const char*);
    void disconnect(bool = false);
    int scanNetworks();
    bool enableIpV6();
};
extern WiFiClass WiFi;
#define WL_CONNECTED 3
#define WIFI_STA 1
