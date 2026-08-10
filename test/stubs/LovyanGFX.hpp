// Заглушка библиотеки экрана для ПРОВЕРКИ СБОРКИ.
//
// Здесь не воспроизводится настоящая библиотека — только то, что нужно компилятору,
// чтобы поймать НАШИ ошибки: необъявленные имена, опечатки в вызовах, потерянные
// переменные. Большинство методов принимают любые аргументы намеренно: спорить с
// подписями чужой библиотеки бессмысленно, а вот «переменная не объявлена» ловится и так.
#pragma once
// Настоящая библиотека тянет ядро платформы за собой — заглушка обязана делать так же,
// иначе файлы, полагающиеся на это, здесь не соберутся, а на плате соберутся.
#include "Arduino.h"
#include <stdint.h>
#include <stddef.h>

#define ANY_METHOD(name) template <class... A> int name(A&&...) { return 0; }

enum textdatum_t {
    top_left, top_center, top_right,
    middle_left, middle_center, middle_right,
    bottom_left, bottom_center, bottom_right
};
#define TFT_BLACK 0x0000
#define TFT_WHITE 0xFFFF

#define SPI2_HOST 1
#define SPI_DMA_CH_AUTO 3

namespace lgfx {

// Настройки узлов: одна структура с полями «на все случаи». Настоящая библиотека
// разносит их по типам, но для проверки нашего кода важно лишь, что поле существует и
// пишется — опечатку в имени это поймает, а спорить о типах чужой библиотеки незачем.
struct AnyCfg {
    int spi_host, spi_mode, freq_write, freq_read, dma_channel;
    bool spi_3wire, use_lock, invert, rgb_order, dlen_16bit, bus_shared;
    int pin_sclk, pin_mosi, pin_miso, pin_dc, pin_cs, pin_rst, pin_busy, pin_int;
    int panel_width, panel_height, memory_width, memory_height;
    int offset_x, offset_y, offset_rotation, readable, dummy_read_pixel, dummy_read_bits;
    int x_min, x_max, y_min, y_max, i2c_port, i2c_addr, pin_sda, pin_scl, freq;
};

template <class Cfg>
struct Node {
    Cfg config() { return Cfg{}; }
    void config(const Cfg&) {}
};

struct Panel_ST7789 {
    AnyCfg config() { return AnyCfg{}; }
    void config(const AnyCfg&) {}
    template <class T> void setBus(T*) {}
    template <class T> void setTouch(T*) {}
    template <class T> void setLight(T*) {}
};
struct Bus_SPI      { AnyCfg config() { return AnyCfg{}; } void config(const AnyCfg&) {} };
struct Touch_GT911  { AnyCfg config() { return AnyCfg{}; } void config(const AnyCfg&) {} };

}  // namespace lgfx

namespace lgfx {
struct LGFX_Device {
    ANY_METHOD(init)
    ANY_METHOD(begin)
    ANY_METHOD(setRotation)
    ANY_METHOD(setBrightness)
    ANY_METHOD(fillScreen)
    ANY_METHOD(fillRect)
    ANY_METHOD(drawRect)
    ANY_METHOD(fillRoundRect)
    ANY_METHOD(drawRoundRect)
    ANY_METHOD(drawLine)
    ANY_METHOD(drawFastHLine)
    ANY_METHOD(drawFastVLine)
    ANY_METHOD(fillCircle)
    ANY_METHOD(drawCircle)
    ANY_METHOD(fillTriangle)
    ANY_METHOD(drawPixel)
    ANY_METHOD(setTextDatum)
    ANY_METHOD(setTextColor)
    ANY_METHOD(setTextSize)
    ANY_METHOD(drawString)
    ANY_METHOD(setFont)
    ANY_METHOD(unloadFont)
    ANY_METHOD(setClipRect)
    ANY_METHOD(clearClipRect)
    ANY_METHOD(startWrite)
    ANY_METHOD(endWrite)
    ANY_METHOD(pushImage)
    ANY_METHOD(setSwapBytes)
    ANY_METHOD(loadFont)
    ANY_METHOD(calibrateTouch)
    ANY_METHOD(setTouchCalibrate)
    int textWidth(...) { return 0; }
    int fontHeight(...) { return 0; }
    int width() { return 320; }
    int height() { return 240; }
    bool getTouch(uint16_t*, uint16_t*) { return false; }
    bool getTouch(int32_t*, int32_t*) { return false; }
    struct TouchIface { ANY_METHOD(setTouchCalibrate) ANY_METHOD(init) };
    TouchIface* touch() { static TouchIface t; return &t; }
    uint16_t readPixelValue(int, int) { return 0; }
    uint16_t color565(int, int, int) { return 0; }
    void setPanel(void*) {}
    template <class T> void setPanel(T*) {}
};
}  // namespace lgfx
using lgfx::LGFX_Device;

// Встроенные шрифты библиотеки: нужны лишь как имена.
namespace fonts {
struct Font { int dummy; };
extern Font Font0, Font2, Font4, efontRU_10, efontRU_12, efontRU_14, efontRU_16,
            efontRU_24, efontJA_10, efontJA_12, efontJA_14, efontJA_16, efontJA_24, DejaVu12, DejaVu18, DejaVu24, DejaVu40, AsciiFont8x16;
}

struct LGFX_Sprite {
    LGFX_Sprite() {}
    LGFX_Sprite(void*) {}
    ANY_METHOD(setColorDepth)
    ANY_METHOD(fillSprite)
    ANY_METHOD(deleteSprite)
    ANY_METHOD(pushSprite)
    ANY_METHOD(drawPixel)
    ANY_METHOD(fillRect)
    ANY_METHOD(drawString)
    ANY_METHOD(setTextDatum)
    ANY_METHOD(setTextColor)
    ANY_METHOD(setFont)
    ANY_METHOD(pushImage)
    ANY_METHOD(setSwapBytes)
    ANY_METHOD(setPsram)
    bool createSprite(int, int) { return true; }
    bool drawPng(const uint8_t*, size_t, int, int, int, int) { return true; }
    void* getBuffer() { return nullptr; }
    uint16_t readPixelValue(int, int) { return 0; }
    int textWidth(...) { return 0; }
    int width() { return 0; }
    int height() { return 0; }
};
