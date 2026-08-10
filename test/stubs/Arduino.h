// Заглушки платформы для ПРОВЕРКИ СБОРКИ на компьютере.
//
// Настоящих заголовков платы здесь нет, а ошибки вида «переменная не объявлена»,
// «не тот тип аргумента», «sizeof от указателя» ловятся и без них — компилятору
// достаточно знать имена и подписи. Это дешевле, чем узнавать о них из чужого лога
// сборки через полчаса после отправки.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

typedef uint8_t byte;
#define IRAM_ATTR
#define RISING 3
#define FALLING 2
#define CHANGE 1
void attachInterrupt(int, void (*)(), int);
void detachInterrupt(int);
int digitalPinToInterrupt(int);
#define OUTPUT 2
#define INPUT 1
#define INPUT_PULLUP 5
#define HIGH 1
#define LOW 0
void pinMode(int, int);
void digitalWrite(int, int);
int digitalRead(int);
int analogRead(int);
uint32_t analogReadMilliVolts(int);
unsigned long millis();
unsigned long micros();
void delay(unsigned long);
long random(long);
void randomSeed(unsigned long);

struct String {
    String() {}
    String(const char*) {}
    String(char) {}
    unsigned length() const;
    const char* c_str() const;
    int indexOf(const char*) const;
    int indexOf(char, int = 0) const;
    int lastIndexOf(char) const;
    bool endsWith(const char*) const;
    bool startsWith(const char*) const;
    String substring(int, int = -1) const;
    int toInt() const;
    operator const char*() const;
    String operator+(const char*) const;
    String operator+(const String&) const;
    bool operator==(const char*) const;
    char charAt(int) const;
};
String operator+(const char*, const String&);

struct IPAddress {
    IPAddress() {}
    IPAddress(uint32_t) {}
    IPAddress(int, int, int, int) {}
    uint8_t& operator[](int);
    uint8_t operator[](int) const;
    bool fromString(const char*);
    operator uint32_t() const;
    String toString() const;
};

struct SerialClass {
    void begin(unsigned long);
    void println();
    void println(const char*);
    void print(const char*);
    int printf(const char*, ...);
    operator bool() const;
};
extern SerialClass Serial;

struct WireClass {
    template <class... A> bool begin(A&&...) { return true; }
    void beginTransmission(int);
    size_t write(uint8_t);
    int endTransmission();
    int requestFrom(int, int);
    int available();
    int read();
    void setClock(uint32_t);
};
extern WireClass Wire;

struct EspClass {
    uint32_t getFreeHeap();
    uint32_t getPsramSize();
    void restart();
};
extern EspClass ESP;

int ets_printf(const char*, ...);
void configTime(long, int, const char*, const char* = nullptr, const char* = nullptr);
#include <time.h>
#include "esp_err.h"
#include "esp_random.h"
#define PROGMEM
#define pdMS_TO_TICKS(x) (x)
typedef void* TaskHandle_t;
void vTaskDelay(uint32_t);
int xTaskCreatePinnedToCore(void (*)(void*), const char*, uint32_t, void*, int,
                            TaskHandle_t*, int);
