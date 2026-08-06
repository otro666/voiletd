#include "emoji.h"
#include "emoji_rom.h"

#include "display.h"
#include "ui.h"
#include <SD.h>
#include <string.h>
#include <new>
#include <math.h>

namespace emoji {

namespace {
// Панель: шесть в ряд. Сетка отцентрована по ширине экрана — прежде она была прижата
// к левому краю и справа висело пустое поле.
constexpr int kCols = 6;
constexpr int kCell = 46;
constexpr int kPad  = 6;
constexpr int kGridX = (320 - kCols * kCell) / 2;
}  // namespace

namespace {
/**
 * Развёрнутые картинки с ВОССТАНОВЛЕННОЙ прозрачностью.
 *
 * Прежде картинка разворачивалась на «условно-прозрачный» зелёный и выводилась с одним
 * битом прозрачности на точку: полупрозрачный край смешивался с зелёным, и вокруг
 * смайлика оставалась рваная кайма. Прозрачность в PNG — плавная, и терять её значит
 * терять сглаживание.
 *
 * Восстанавливаем её так: разворачиваем картинку ДВАЖДЫ, на чёрном и на белом. Точка,
 * прозрачная на долю a, на чёрном даёт a·C, на белом — a·C + (1−a)·255; разность и есть
 * прозрачность, без разбора формата PNG вручную. Держим цвет-на-чёрном (это готовое
 * a·C) и прозрачность: смешивание с любой подложкой — одно сложение на канал.
 *
 * Память: kSize²×(2+1) байта на смайлик — на все семнадцать около 50 КБ. Раскрывается
 * один раз при запуске; при каждой отрисовке точки только смешиваются и уходят на экран
 * одним блоком.
 */
struct Baked {
    uint16_t onBlack[kSize * kSize];   // a·C, уже в цвете экрана
    uint8_t  alpha[kSize * kSize];     // 0 — пусто, 255 — плотно
};
Baked* g_baked[64] = {};
size_t g_cardCount = 0;
bool   g_scanned = false;

/** Путь к картинке: имя как в наборе Noto — emoji_u1f642.png, строчными, код добит
 *  нулями до четырёх знаков. Так набор кладётся на карту как есть, без переименования. */
void pathOf(size_t index, char* out, size_t cap) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(kSet[index].utf8);
    uint32_t cp = 0;
    if ((p[0] & 0x80) == 0) cp = p[0];
    else if ((p[0] & 0xE0) == 0xC0) cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    else if ((p[0] & 0xF0) == 0xE0)
        cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    else cp = ((uint32_t(p[0] & 0x07) << 18) | (uint32_t(p[1] & 0x3F) << 12) |
               (uint32_t(p[2] & 0x3F) << 6) | uint32_t(p[3] & 0x3F));
    snprintf(out, cap, "/vual/emoji/emoji_u%04x.png", unsigned(cp));
}

/**
 * Развернуть картинку с карты и восстановить прозрачность.
 *
 * Картинка разворачивается В РОДНОМ размере (наборы кладут 72×72), и только потом
 * ужимается до kSize усреднением по площади. Раньше масштабировала сама развёртка —
 * а она берёт ближайшую точку, без усреднения, и «лесенка» на краях оставалась даже
 * при плавной прозрачности. Усреднение по площади — это то, что делает любой телефон,
 * когда показывает большую картинку маленькой.
 */
bool bake(size_t index) {
    char path[48];
    pathOf(index, path, sizeof(path));

    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    const size_t len = size_t(f.size());
    if (len == 0 || len > 24576) { f.close(); return false; }

    static uint8_t raw[24576];
    const int got = f.read(raw, len);
    f.close();
    if (got != int(len)) return false;

    // Родной размер — из заголовка файла: ширина и высота лежат в нём старшим байтом
    // вперёд. Разворачивать вслепую в 28×28 значило бы снова масштабировать развёрткой.
    if (len < 24 || memcmp(raw + 12, "IHDR", 4) != 0) return false;
    const int w = (raw[16] << 24) | (raw[17] << 16) | (raw[18] << 8) | raw[19];
    const int h = (raw[20] << 24) | (raw[21] << 16) | (raw[22] << 8) | raw[23];
    if (w < kSize || h < kSize || w > 144 || h > 144) return false;

    // Две развёртки одной картинки: на чёрном и на белом — из них восстанавливается
    // прозрачность каждой точки.
    LGFX_Sprite black(&vualScreen()), white(&vualScreen());
    black.setColorDepth(16); white.setColorDepth(16);
    if (!black.createSprite(w, h)) return false;
    if (!white.createSprite(w, h)) { black.deleteSprite(); return false; }
    black.fillSprite(TFT_BLACK);
    white.fillSprite(TFT_WHITE);

    const bool ok = black.drawPng(raw, len, 0, 0, w, h) &&
                    white.drawPng(raw, len, 0, 0, w, h);
    if (!ok) { black.deleteSprite(); white.deleteSprite(); return false; }

    // Порядок байтов в памяти спрайта не угадываем, а ПРОВЕРЯЕМ: рисуем чистый красный
    // и смотрим, как он лёг. Ошибись мы здесь — прозрачность считалась бы по чужому
    // каналу и все смайлики вышли бы рваными.
    static int swapped = -1;
    if (swapped < 0) {
        LGFX_Sprite probe(&vualScreen());
        probe.setColorDepth(16);
        if (probe.createSprite(1, 1)) {
            probe.fillSprite(uint16_t(0xF800));
            swapped = (uint16_t(probe.readPixelValue(0, 0)) == 0xF800) ? 0 : 1;
            probe.deleteSprite();
        } else swapped = 1;
    }
    auto rd = [&](LGFX_Sprite& s, int x, int y) -> uint16_t {
        const uint16_t v = uint16_t(s.readPixelValue(x, y));
        return swapped ? __builtin_bswap16(v) : v;
    };

    Baked* b = new (std::nothrow) Baked;
    if (!b) { black.deleteSprite(); white.deleteSprite(); return false; }

    // Ужатие усреднением по площади: каждая точка итога — среднее всех точек исходника,
    // которые она накрывает, с дробными весами на краях. Считаем в цвете, уже
    // домноженном на прозрачность, — так полупрозрачные края не тянут за собой чёрный.
    const float sx = float(w) / kSize, sy = float(h) / kSize;
    for (int oy = 0; oy < kSize; ++oy) {
        const float fy0 = oy * sy, fy1 = fy0 + sy;
        for (int ox = 0; ox < kSize; ++ox) {
            const float fx0 = ox * sx, fx1 = fx0 + sx;
            float rs = 0, gs = 0, bs = 0, as = 0, area = 0;
            for (int iy = int(fy0); iy < int(ceilf(fy1)) && iy < h; ++iy) {
                const float wy = fminf(fy1, iy + 1.0f) - fmaxf(fy0, float(iy));
                if (wy <= 0) continue;
                for (int ix = int(fx0); ix < int(ceilf(fx1)) && ix < w; ++ix) {
                    const float wx = fminf(fx1, ix + 1.0f) - fmaxf(fx0, float(ix));
                    if (wx <= 0) continue;
                    const float wgt = wx * wy;
                    const uint16_t cb = rd(black, ix, iy);
                    const uint16_t cw = rd(white, ix, iy);
                    const int gb = (cb >> 5) & 63, gw = (cw >> 5) & 63;
                    int diff = gw - gb; if (diff < 0) diff = 0;
                    int a = 255 - diff * 255 / 63;
                    if (a < 0) a = 0; if (a > 255) a = 255;
                    rs += wgt * float(((cb >> 11) & 31) * 255 / 31);   // a·C по каналам
                    gs += wgt * float(gb * 255 / 63);
                    bs += wgt * float((cb & 31) * 255 / 31);
                    as += wgt * float(a);
                    area += wgt;
                }
            }
            const float inv = area > 0 ? 1.0f / area : 0;
            const int r8 = int(rs * inv), g8 = int(gs * inv), b8 = int(bs * inv);
            const int a8 = int(as * inv);
            b->onBlack[oy * kSize + ox] =
                uint16_t(((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3));
            b->alpha[oy * kSize + ox] = uint8_t(a8 > 255 ? 255 : a8);
        }
    }

    black.deleteSprite();
    white.deleteSprite();
    g_baked[index] = b;
    return true;
}

/** a·C + (1−a)·bg по каналам: цвет уже домножен на прозрачность при развёртке. */
inline uint16_t overBg(uint16_t premul, uint16_t bg, uint8_t a) {
    const int ia = 255 - a;
    const int r = ((premul >> 11) & 31) + ((bg >> 11) & 31) * ia / 255;
    const int g = ((premul >> 5) & 63) + ((bg >> 5) & 63) * ia / 255;
    const int b = (premul & 31) + (bg & 31) * ia / 255;
    return uint16_t(((r > 31 ? 31 : r) << 11) | ((g > 63 ? 63 : g) << 5) |
                    (b > 31 ? 31 : b));
}

}  // namespace

void scanCard() {
    g_cardCount = 0;
    const uint32_t t0 = millis();
    for (size_t i = 0; i < kCount && i < 64; ++i) {
        if (bake(i)) ++g_cardCount;
    }
    g_scanned = true;
    Serial.printf("эмодзи с карты: %u из %u, развёрнуто за %lu мс\n",
                  unsigned(g_cardCount), unsigned(kCount),
                  (unsigned long)(millis() - t0));
}

size_t cardCount() { return g_cardCount; }

void draw(size_t index, int x, int y, uint16_t bg) {
    if (index >= kCount) return;

    // Собираем картинку в спрайте и отдаём одним блоком: точечный вывод по шине был бы
    // на два порядка медленнее, а спрайт вдобавок избавляет от вопросов о порядке байтов.
    static LGFX_Sprite* cb = nullptr;
    if (!cb) {
        cb = new LGFX_Sprite(&vualScreen());
        cb->setColorDepth(16);
        cb->createSprite(kSize, kSize);
    }
    static uint16_t out[kSize * kSize];

    // Картинка с карты: плавная прозрачность восстановлена при развёртке, смешивание
    // с подложкой — одно сложение на канал.
    if (index < 64 && g_baked[index]) {
        const Baked* b = g_baked[index];
        for (int i = 0; i < kSize * kSize; ++i)
            out[i] = b->alpha[i] ? overBg(b->onBlack[i], bg, b->alpha[i]) : bg;
        for (int row = 0; row < kSize; ++row)
            for (int col = 0; col < kSize; ++col)
                cb->drawPixel(col, row, out[row * kSize + col]);
        cb->pushSprite(x, y);
        return;
    }

    // Встроенный рисунок. Он хранится так же, как картинка с карты: цвет, заранее
    // домноженный на прозрачность, плюс сама прозрачность — рисунки отрисованы
    // вчетверо крупнее и сжаты с плавным краем ещё на сборке (tools/make_emoji.py),
    // поэтому сглаживание здесь получается бесплатно, тем же сложением на канал.
    const RomEmoji& rom = kRom[index];
    for (int i = 0; i < kSize * kSize; ++i)
        out[i] = rom.alpha[i] ? overBg(rom.premul[i], bg, rom.alpha[i]) : bg;
    for (int row = 0; row < kSize; ++row)
        for (int col = 0; col < kSize; ++col)
            cb->drawPixel(col, row, out[row * kSize + col]);
    cb->pushSprite(x, y);
}

size_t match(const char* text, size_t& indexOut) {
    if (!text || !*text) return 0;
    for (size_t i = 0; i < kCount; ++i) {
        const char* e = kSet[i].utf8;
        size_t n = 0;
        while (e[n]) ++n;
        bool same = true;
        for (size_t k = 0; k < n; ++k) {
            if (text[k] != e[k]) { same = false; break; }
        }
        if (same) { indexOut = i; return n; }
    }
    return 0;
}

int drawPicker(int y, size_t selected) {
    auto& tft = vualScreen();
    const int rows = int((kCount + kCols - 1) / kCols);
    const int h = rows * kCell + kPad;

    tft.fillRect(0, y, 320, h, ui::kSurface);
    tft.drawFastHLine(0, y, 320, ui::kSurfaceHigh);   // кромка панели
    for (size_t i = 0; i < kCount; ++i) {
        const int cx = kGridX + int(i % kCols) * kCell;
        const int cy = y + int(i / kCols) * kCell + kPad;
        const bool sel = (i == selected);
        if (sel) {
            tft.fillRoundRect(cx - 2, cy - 2, kCell - 4, kCell - 4, 8, ui::kSurfaceHigh);
        }
        // Подложка ячейки — то, на чём стоит смайлик: край смешивается с ней.
        draw(i, cx + (kCell - 8 - kSize) / 2, cy + (kCell - 8 - kSize) / 2,
             sel ? ui::kSurfaceHigh : ui::kSurface);
    }
    return h;
}

int pickerHit(int panelY, int16_t x, int16_t y) {
    if (y < panelY) return -1;
    const int col = (x - kGridX) / kCell;
    const int row = (y - panelY - kPad) / kCell;
    if (col < 0 || col >= kCols || row < 0) return -1;
    const size_t idx = size_t(row * kCols + col);
    return idx < kCount ? int(idx) : -1;
}

}  // namespace emoji
