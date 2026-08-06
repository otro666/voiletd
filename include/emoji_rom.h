// Встроенные эмодзи со сглаживанием. СГЕНЕРИРОВАНО tools/make_emoji.py.
#pragma once
#include <stdint.h>

namespace emoji {

/** Цвет, домноженный на прозрачность (формат экрана), и прозрачность.
 *  Ровно то же представление, что у картинок, развёрнутых с карты. */
struct RomEmoji { const uint16_t* premul; const uint8_t* alpha; };

extern const RomEmoji kRom[17];

}  // namespace emoji
