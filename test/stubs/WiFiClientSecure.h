#pragma once
#include "Arduino.h"
struct WiFiClientSecure {
    void setInsecure();
    void setTimeout(uint32_t);
    bool connect(const char*, uint16_t);
    bool connected();
    int available();
    int read();
    int read(uint8_t*, size_t);
    size_t write(const uint8_t*, size_t);
    void stop();
    String readStringUntil(char);
};
