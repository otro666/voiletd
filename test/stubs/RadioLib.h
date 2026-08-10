// Заглушка радиобиблиотеки для ПРОВЕРКИ СБОРКИ.
//
// Как и у экрана: воспроизводится не поведение, а лишь имена и то, что важно для
// проверки НАШЕГО кода. Возвраты подобраны по смыслу вызова — код ошибки целым числом,
// уровень сигнала дробным, — чтобы наши выражения проверялись по-настоящему.
#pragma once
#include "Arduino.h"
#include "SPI.h"

#define RADIOLIB_ERR_NONE 0
#define RADIOLIB_ERR_CRC_MISMATCH -7
#define RADIOLIB_ERR_RX_TIMEOUT -6
#define RADIOLIB_SX126X_IRQ_RX_DONE 0x02
#define RADIOLIB_SX126X_SYNC_WORD_PRIVATE 0x12
#define RADIOLIB_SX126X_SYNC_WORD_PUBLIC 0x34
#define CARD_NONE 0
#define RADIOLIB_SX126X_IRQ_TX_DONE 0x01

struct Module {
    template <class... A> Module(A&&...) {}
};

struct SX1262 {
    SX1262(Module*) {}
    SX1262(Module) {}
    int begin(float = 434.0, float = 125.0, int = 9, int = 7, uint8_t = 0x12,
              int8_t = 10, uint16_t = 8, float = 1.6, bool = false) { return 0; }
    int setFrequency(float) { return 0; }
    int setBandwidth(float) { return 0; }
    int setSpreadingFactor(int) { return 0; }
    int setCodingRate(int) { return 0; }
    int setSyncWord(uint8_t) { return 0; }
    int setOutputPower(int) { return 0; }
    int setPreambleLength(uint16_t) { return 0; }
    int setCurrentLimit(float) { return 0; }
    int setCRC(bool) { return 0; }
    int setDio2AsRfSwitch(bool) { return 0; }
    int setTCXO(float) { return 0; }
    int setRxBoostedGainMode(bool, bool = true) { return 0; }
    int startReceive() { return 0; }
    int startTransmit(const uint8_t*, size_t) { return 0; }
    int transmit(const uint8_t*, size_t) { return 0; }
    int readData(uint8_t*, size_t) { return 0; }
    size_t getPacketLength() { return 0; }
    float getRSSI() { return -100.0f; }
    float getSNR() { return 0.0f; }
    int standby() { return 0; }
    int sleep() { return 0; }
    int finishTransmit() { return 0; }
    void setPacketReceivedAction(void (*)()) {}
    void setPacketSentAction(void (*)()) {}
    void setDio1Action(void (*)()) {}
    void clearDio1Action() {}
    int scanChannel() { return 0; }
    int getIrqStatus() { return 0; }
    int clearIrqStatus(uint16_t = 0xFFFF) { return 0; }
    int explicitHeader() { return 0; }
    int implicitHeader(size_t) { return 0; }
};
