#pragma once
#include "Arduino.h"
#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"
struct File {
    operator bool() const;
    int read();
    int read(uint8_t*, size_t);
    size_t write(const uint8_t*, size_t);
    size_t write(const uint8_t);
    int printf(const char*, ...);
    void close();
    uint32_t size();
    bool seek(uint32_t);
    int available();
    String readStringUntil(char);
    const char* name();
    File openNextFile();
    bool isDirectory();
    void flush();
};
struct SDClass {
    template <class... A> bool begin(A&&...) { return true; }
    File open(const char*, const char* = FILE_READ);
    bool exists(const char*);
    bool remove(const char*);
    bool mkdir(const char*);
    bool rename(const char*, const char*);
    uint64_t totalBytes();
    uint64_t cardSize();
    int cardType();
    uint64_t usedBytes();
    void end();
};
extern SDClass SD;
