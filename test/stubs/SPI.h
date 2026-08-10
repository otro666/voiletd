#pragma once
#include "Arduino.h"
struct SPIClass {
    SPIClass(int = 0) {}
    template <class... A> void begin(A&&...) {}
    void end();
};
extern SPIClass SPI;
