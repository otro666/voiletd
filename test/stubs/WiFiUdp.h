#pragma once
#include "Arduino.h"
struct WiFiUDP {
    bool begin(uint16_t);
    bool beginMulticast(IPAddress, uint16_t);
    int parsePacket();
    int read(uint8_t*, size_t);
    void beginPacket(IPAddress, uint16_t);
    void beginPacket(const char*, uint16_t);
    void beginMulticastPacket();
    size_t write(const uint8_t*, size_t);
    int endPacket();
    IPAddress remoteIP();
    uint16_t remotePort();
    void stop();
};
