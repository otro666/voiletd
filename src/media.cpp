#include "media.h"

#include <stdio.h>
#include <string.h>
#include <initializer_list>

namespace media {

ImageFormat detect(const uint8_t* data, size_t len) {
    if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) return IMG_JPEG;
    // Собственный формат обложек: заголовок "V5" и размеры.
    if (len >= 6 && data[0] == 'V' && data[1] == '5') return IMG_RGB565;
    return IMG_UNSUPPORTED;
}

uint8_t pickScale(int srcW, int srcH, int dstW, int dstH) {
    // Идём от большего уменьшения к меньшему и берём первое, при котором картинка
    // ещё покрывает экран. Восьмикратное — предел, заложенный в самом формате JPEG.
    for (uint8_t s : {8, 4, 2}) {
        if (srcW / s >= dstW && srcH / s >= dstH) return s;
    }
    return 1;
}

Fit fitCenter(int srcW, int srcH, int dstW, int dstH) {
    if (srcW <= 0 || srcH <= 0) return Fit{0, 0, dstW, dstH};
    // Вписываем целиком, а не обрезаем: на маленьком экране обрезка съедает половину
    // содержимого, и человек не понимает, что ему прислали.
    const int byW = dstW * srcH / srcW;
    Fit f{};
    if (byW <= dstH) { f.w = dstW; f.h = byW; }
    else             { f.h = dstH; f.w = dstH * srcW / srcH; }
    f.x = (dstW - f.w) / 2;
    f.y = (dstH - f.h) / 2;
    return f;
}

void thumbPath(const char* fileId, char* out, size_t outLen) {
    snprintf(out, outLen, "/vual/thumbs/%s.jpg", fileId);
}

}  // namespace media
