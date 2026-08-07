// Отрисовка интерфейса в стиле Вуали на экране 320×240.
//
// Главное соображение прежнее: полная перерисовка на этом контроллере занимает заметное
// время, поэтому она делается только при смене экрана, а строка состояния и новые
// сообщения дорисовываются поверх.
//
// Как достигается сглаживание.
//
// Панель у нас БЕЗ обратного чтения (readable = false), поэтому полупрозрачный край
// нельзя «положить поверх того, что есть»: прочитать «что есть» нечем. Зато весь
// интерфейс рисуется на известных сплошных подложках — фон, шапка, пузырь. Поэтому
// каждый сглаженный примитив принимает цвет подложки ЯВНО и смешивает край с ним сам.
// Тот же принцип у шрифтов: у каждого вывода текста фон задан явно.
//
// Значки нарисованы тонким штрихом со скруглёнными концами — как в телефонных
// мессенджерах, а не «из палочек», как вышло в первой версии.
#include "ui.h"
#include "rail.h"
#include "nostr.h"
#include "tracker.h"
#include <string.h>
#include <math.h>
#include "emoji.h"

#include "display.h"
#include "fonts_vlw.h"
#include <stdio.h>

#include "board.h"
#include "boot.h"

namespace ui {

namespace {

// Тот же объект, что у экрана загрузки.
VualDisplay& tft = vualScreen();

// ── сглаженные примитивы ───────────────────────────────────────────────────────────────

/** Смешать два цвета экрана: a — доля переднего, 0…255. */
uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t a) {
    const int fr = (fg >> 11) & 31, fgc = (fg >> 5) & 63, fb = fg & 31;
    const int br = (bg >> 11) & 31, bgc = (bg >> 5) & 63, bb = bg & 31;
    const int r = (fr * a + br * (255 - a)) / 255;
    const int g = (fgc * a + bgc * (255 - a)) / 255;
    const int b = (fb * a + bb * (255 - a)) / 255;
    return uint16_t((r << 11) | (g << 5) | b);
}

/** Точка с покрытием: полная — цветом, краевая — смесью с подложкой, пустая — никак.
 *  cover в долях точки, уже умноженных на 255. */
inline void aaPx(int x, int y, uint16_t col, uint16_t bg, int cover) {
    if (cover >= 250)     tft.drawPixel(x, y, col);
    else if (cover > 5)   tft.drawPixel(x, y, blend565(col, bg, uint8_t(cover)));
}

/** Покрытие по расстоянию до края: внутри — 255, на краю — линейный спад в одну точку. */
inline int coverOf(float dist) {
    const float c = 0.5f - dist;              // >0 внутри
    if (c >= 1.0f) return 255;
    if (c <= 0.0f) return 0;
    return int(c * 255.0f);
}

/** Залитый круг со сглаженным краем. */
void aaCircle(float cx, float cy, float r, uint16_t col, uint16_t bg) {
    const int x0 = int(floorf(cx - r - 1)), x1 = int(ceilf(cx + r + 1));
    const int y0 = int(floorf(cy - r - 1)), y1 = int(ceilf(cy + r + 1));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const float d = hypotf(x - cx, y - cy) - r;
            aaPx(x, y, col, bg, coverOf(d));
        }
}

/** Точный обход мелкой формы: расстояние до скруглённого прямоугольника считается для
 *  каждой точки. Медленно, но у капсулы микрофона всего сотня точек. */
void aaRoundRectPx(int x, int y, int w, int h, float r, uint16_t col, uint16_t bg) {
    const float cx = x + (w - 1) * 0.5f, cy = y + (h - 1) * 0.5f;
    const float ex = (w - 1) * 0.5f - r, ey = (h - 1) * 0.5f - r;
    for (int py = y - 1; py <= y + h; ++py)
        for (int px = x - 1; px <= x + w; ++px) {
            float dx = fabsf(px - cx) - ex; if (dx < 0) dx = 0;
            float dy = fabsf(py - cy) - ey; if (dy < 0) dy = 0;
            aaPx(px, py, col, bg, coverOf(hypotf(dx, dy) - r));
        }
}

/** Скруглённый прямоугольник со сглаженными углами. Стороны у него ровные — их
 *  сглаживать нечего, поэтому середина заливается быстрыми прямоугольниками, а
 *  точечно обходятся только четыре угла. Форма, у которой скругление сравнимо с
 *  размером, уходит на точный путь целиком. */
void aaRoundRect(int x, int y, int w, int h, float r, uint16_t col, uint16_t bg) {
    const float rMax = 0.5f * (w < h ? w : h);
    if (r > rMax) r = rMax;
    const int ri = int(ceilf(r));
    if (w <= 2 * ri + 1 || h <= 2 * ri + 1) { aaRoundRectPx(x, y, w, h, r, col, bg); return; }

    tft.fillRect(x + ri, y, w - 2 * ri, h, col);            // середина
    tft.fillRect(x, y + ri, ri, h - 2 * ri, col);           // левая полоса
    tft.fillRect(x + w - ri, y + ri, ri, h - 2 * ri, col);  // правая полоса

    const float ccx[2] = {x + r, x + w - 1 - r};
    const float ccy[2] = {y + r, y + h - 1 - r};
    for (int q = 0; q < 4; ++q) {
        const float qx = ccx[q & 1], qy = ccy[q >> 1];
        const int px0 = (q & 1) ? x + w - ri : x;
        const int py0 = (q >> 1) ? y + h - ri : y;
        for (int py = py0; py < py0 + ri; ++py)
            for (int px = px0; px < px0 + ri; ++px) {
                const float d = hypotf(px - qx, py - qy) - r;
                aaPx(px, py, col, bg, coverOf(d));
            }
    }
}

/** Отрезок заданной толщины со скруглёнными концами. Основа всех значков: тонкий
 *  штрих 2–3 точки без сглаживания выглядит рваным, со сглаживанием — как в телефоне. */
void aaLine(float x0, float y0, float x1, float y1, float halfW,
            uint16_t col, uint16_t bg) {
    const int bx0 = int(floorf(fminf(x0, x1) - halfW - 1));
    const int bx1 = int(ceilf (fmaxf(x0, x1) + halfW + 1));
    const int by0 = int(floorf(fminf(y0, y1) - halfW - 1));
    const int by1 = int(ceilf (fmaxf(y0, y1) + halfW + 1));
    const float dx = x1 - x0, dy = y1 - y0;
    const float len2 = dx * dx + dy * dy;
    for (int y = by0; y <= by1; ++y)
        for (int x = bx0; x <= bx1; ++x) {
            float t = len2 > 0 ? ((x - x0) * dx + (y - y0) * dy) / len2 : 0;
            t = t < 0 ? 0 : (t > 1 ? 1 : t);
            const float d = hypotf(x - (x0 + t * dx), y - (y0 + t * dy)) - halfW;
            aaPx(x, y, col, bg, coverOf(d));
        }
}

/** Дуга кольца: направление dirDeg (0 — вправо, 90 — вверх), раствор ±spreadDeg.
 *  Толщина 2·halfW. Углами задаётся, КУДА дуга смотрит, — так значки Wi-Fi и эфира
 *  описываются одной строкой. */
void aaArc(float cx, float cy, float radius, float halfW,
           float dirDeg, float spreadDeg, uint16_t col, uint16_t bg) {
    const float rOut = radius + halfW;
    const int x0 = int(floorf(cx - rOut - 1)), x1 = int(ceilf(cx + rOut + 1));
    const int y0 = int(floorf(cy - rOut - 1)), y1 = int(ceilf(cy + rOut + 1));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const float rr = hypotf(x - cx, y - cy);
            const float d = fabsf(rr - radius) - halfW;
            if (d >= 0.5f) continue;
            // Экранная ось Y растёт вниз, поэтому знак минус: угол считаем по-школьному.
            float ang = atan2f(-(y - cy), x - cx) * 57.29578f;
            float diff = ang - dirDeg;
            while (diff > 180) diff -= 360;
            while (diff < -180) diff += 360;
            if (fabsf(diff) > spreadDeg) continue;
            aaPx(x, y, col, bg, coverOf(d));
        }
}

// ── шрифты ─────────────────────────────────────────────────────────────────────────────
//
// Три размера, переключаются загрузкой. Загрузка разбирает таблицу шрифта заново,
// поэтому переключаем только при действительной смене — иначе каждая строка платила бы
// за разбор.
enum FontId : uint8_t { F_NONE, F_SMALL, F_TEXT, F_TITLE };
FontId curFont_ = F_NONE;

void setF(FontId f) {
    if (f == curFont_) return;
    curFont_ = f;
    switch (f) {
    case F_SMALL: tft.loadFont(vualFontSmall); break;
    case F_TEXT:  tft.loadFont(vualFontText);  break;
    case F_TITLE: tft.loadFont(vualFontTitle); break;
    default: break;
    }
}

// Высота строки текстового размера — из генератора шрифтов (ascent + descent).
constexpr int kLineText = 21;

// ── компоновка ─────────────────────────────────────────────────────────────────────────
// Шапка 34 точки: в неё надо попадать пальцем — там значок меню и стрелка назад.
constexpr int kHeaderH = 34;
constexpr int kInputH  = 34;
constexpr int kListY   = kHeaderH + 6;
// Значок меню в правом верхнем углу: место, где его ищут рукой в любом приложении.
constexpr int kBurgerX = 320 - 36;
constexpr int kRowH    = 24;      // строки меню

// Строка ввода: раскладка слева, справа — микрофон, смайлики, отправка.
constexpr int kBadgeW = 40;
constexpr int kSendCX  = 320 - 21;             // центр кнопки отправки
constexpr int kEmojiCX = kSendCX - 29;         // центр значка смайликов
constexpr int kMicCX   = kEmojiCX - 30;        // центр микрофона
constexpr int kFieldX  = kBadgeW + 8;
constexpr int kFieldW  = (kMicCX - 16) - kFieldX;

// ── лента сообщений ────────────────────────────────────────────────────────────────────
//
// Держим последние в памяти, остальное лежит на карте. Текст до 160 байт — столько же,
// сколько вмещает черновик: раньше буфер строки был 64 байта и длинные сообщения
// обрезались уже на экране.
constexpr size_t kMaxMsgs = 30;
struct Msg {
    char text[160];
    bool mine;
    bool delivered;
    uint32_t ts;
    char media[64];    // путь к голосовому; пусто — обычный текст
};
Msg msgs_[kMaxMsgs];
size_t msgCount_ = 0;

/** Прокрутка ленты: на сколько точек уехали ВВЕРХ от свежего края. Ноль — прижаты к
 *  свежему, и новые сообщения сами держат ленту внизу. */
int chatScroll_ = 0;

Screen current_ = SCR_CHATS;

constexpr size_t kMaxPeers = 16;
const char* peerNames_[kMaxPeers] = {};
bool        peerOnline_[kMaxPeers] = {};
uint8_t     peerUnread_[kMaxPeers] = {};
size_t      peerCount_ = 0;
size_t      peerSel_ = 0;
/** Первая видимая строка списка: строки стали крупными, и все собеседники разом на экран
 *  не помещаются — окно едет за выделением. */
size_t      peerTop_ = 0;
/** Взведённый на удаление ряд; -1 — никакой. См. armDelete в заголовке. */
int         deleteArm_ = -1;

char input_[160] = {};
bool cyrillic_ = false;
bool emojiOpen_ = false;
// Состояние связи для значков в шапке. Обновляется ядром, а не опрашивается отсюда:
// интерфейс не должен знать, как устроены сеть и радио.
bool wifiUp_ = false, radioUp_ = false, railUp_ = false;
bool recording_ = false;
uint32_t recElapsed_ = 0;
/** Состояние Shift для значка раскладки: 0 — обычное, 1 — одна заглавная, 2 — верхний
 *  регистр. Показывается регистром самого значка: ру → Ру → РУ. */
uint8_t shiftMode_ = 0;
/** Верх панели смайликов. Считается при отрисовке — по нему же разбираются касания. */
int  emojiTop_ = 240;

// ── значки ─────────────────────────────────────────────────────────────────────────────
//
// Все значки нарисованы сглаженным штрихом. Толщина 1.1–1.3 — как выглядит тонкая
// телефонная пиктограмма на плотности этого экрана.

/** Значок меню — три черты со скруглёнными концами. */
void drawBurger(uint16_t c, uint16_t bg) {
    for (int i = 0; i < 3; ++i)
        aaLine(kBurgerX + 8, 11 + i * 6, kBurgerX + 26, 11 + i * 6, 1.3f, c, bg);
}

/** Стрелка назад — один шеврон, а не пучок линий. */
void drawBackChevron(uint16_t c, uint16_t bg) {
    const float cx = 20, cy = kHeaderH / 2.0f;
    aaLine(cx + 4, cy - 7, cx - 4, cy, 1.4f, c, bg);
    aaLine(cx - 4, cy, cx + 4, cy + 7, 1.4f, c, bg);
}

/** Wi-Fi — точка и растущие дуги, смотрят вверх. */
void drawWifiIcon(float cx, float cy, uint16_t c, uint16_t bg) {
    aaCircle(cx, cy + 5, 1.4f, c, bg);
    aaArc(cx, cy + 5, 4.5f, 0.9f, 90, 52, c, bg);
    aaArc(cx, cy + 5, 8.0f, 0.9f, 90, 52, c, bg);
    aaArc(cx, cy + 5, 11.5f, 0.9f, 90, 52, c, bg);
}

/** Эфир — точка с волнами в обе стороны: (( · )). Понятнее мачты и не смахивает на
 *  рацию из походного набора. */
void drawRadioIcon(float cx, float cy, uint16_t c, uint16_t bg) {
    aaCircle(cx, cy, 2.0f, c, bg);
    aaArc(cx, cy, 5.5f, 0.9f, 0, 50, c, bg);
    aaArc(cx, cy, 9.0f, 0.9f, 0, 50, c, bg);
    aaArc(cx, cy, 5.5f, 0.9f, 180, 50, c, bg);
    aaArc(cx, cy, 9.0f, 0.9f, 180, 50, c, bg);
}

/** Рельса — значок «поделиться»: три узла со связями. Рельса и есть передача через
 *  посредника, и этот привычный силуэт читается сразу. */
void drawRailIcon(float cx, float cy, uint16_t c, uint16_t bg) {
    aaLine(cx - 5, cy, cx + 5, cy - 5, 1.1f, c, bg);
    aaLine(cx - 5, cy, cx + 5, cy + 5, 1.1f, c, bg);
    aaCircle(cx - 5, cy, 2.4f, c, bg);
    aaCircle(cx + 5, cy - 5, 2.4f, c, bg);
    aaCircle(cx + 5, cy + 5, 2.4f, c, bg);
}

void drawStatusIcons() {
    const float cy = kHeaderH / 2.0f;
    drawWifiIcon (kBurgerX - 66, cy - 1, wifiUp_  ? kOnline : kTextTertiary, kSurface);
    drawRadioIcon(kBurgerX - 42, cy, radioUp_ ? kOnline : kTextTertiary, kSurface);
    drawRailIcon (kBurgerX - 18, cy, railUp_  ? kOnline : kTextTertiary, kSurface);
}

/** Шапка. withBack — рисовать ли стрелку назад (на корневом списке её нет). */
void drawHeaderBar(const char* title, bool withBack) {
    tft.fillRect(0, 0, 320, kHeaderH, kSurface);
    tft.drawFastHLine(0, kHeaderH - 1, 320, kSurfaceHigh);   // тонкий разделитель
    if (withBack) drawBackChevron(kTextSecond, kSurface);
    tft.setTextColor(kTextPrimary, kSurface);
    tft.setTextDatum(textdatum_t::middle_left);
    setF(F_TITLE);
    // Обрезаем длинное имя, чтобы не наехать на значки состояния.
    tft.setClipRect(0, 0, kBurgerX - 90, kHeaderH);
    tft.drawString(title, withBack ? 38 : 12, kHeaderH / 2 - 1);
    tft.clearClipRect();
    drawBurger(kTextSecond, kSurface);
    drawStatusIcons();
}

// ── разбор текста на элементы ──────────────────────────────────────────────────────────

/** Длина символа UTF-8 по первому байту. Символ берём целиком, иначе кириллица
 *  распадается на половинки. */
int utf8Len(const char* p) {
    const uint8_t c = uint8_t(*p);
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/** Ширина одного элемента: смайлик — картинкой, буква — шрифтом (F_TEXT должен быть
 *  установлен снаружи). Возвращает длину элемента в байтах. */
int elemWidth(const char* p, int& w, bool& isEmoji) {
    size_t idx;
    const size_t n = emoji::match(p, idx);
    if (n) { w = emoji::kSize + 2; isEmoji = true; return int(n); }
    const int len = utf8Len(p);
    char buf[5] = {};
    memcpy(buf, p, size_t(len));
    w = tft.textWidth(buf);
    isEmoji = false;
    return len;
}

// ── перенос слов ───────────────────────────────────────────────────────────────────────

struct WLine {
    const char* s;
    int len;        // байт в строке
    int w;          // ширина в точках
    bool tall;      // есть смайлик — строка выше обычной
};

/**
 * Разложить текст по строкам не шире maxW.
 *
 * Рвём по последнему пробелу; слово длиннее строки рвём посреди — иначе оно вытолкнуло
 * бы пузырь за экран. Возвращает число строк (не больше maxLines; лишнее отбрасывается —
 * при пределе черновика в 160 байт это недостижимо).
 */
int wrapText(const char* text, int maxW, WLine out[], int maxLines) {
    setF(F_TEXT);
    int lines = 0;
    const char* p = text;
    const char* lineStart = p;
    int lineW = 0;
    bool lineTall = false;
    const char* lastSpace = nullptr;   // где рвать по-хорошему
    int wAtSpace = 0;
    bool tallAtSpace = false;

    auto flush = [&](const char* end, int w, bool tall) {
        if (lines < maxLines) out[lines++] = WLine{lineStart, int(end - lineStart), w, tall};
    };

    while (*p) {
        int ew; bool em;
        const int el = elemWidth(p, ew, em);

        if (*p == ' ') { lastSpace = p; wAtSpace = lineW; tallAtSpace = lineTall; }

        if (lineW + ew > maxW && p != lineStart) {
            if (lastSpace && lastSpace > lineStart) {
                flush(lastSpace, wAtSpace, tallAtSpace);
                lineStart = lastSpace + 1;       // пробел на стыке съедается
                // Пересчитываем ширину хвоста от нового начала до p.
                lineW = 0; lineTall = false;
                for (const char* q = lineStart; q < p; ) {
                    int qw; bool qe; const int ql = elemWidth(q, qw, qe);
                    lineW += qw; lineTall = lineTall || qe; q += ql;
                }
            } else {
                flush(p, lineW, lineTall);
                lineStart = p;
                lineW = 0; lineTall = false;
            }
            lastSpace = nullptr;
        }

        lineW += ew;
        lineTall = lineTall || em;
        p += el;
    }
    if (p > lineStart || lines == 0) flush(p, lineW, lineTall);
    return lines;
}

/** Нарисовать строку из разложенного текста: буквы шрифтом, смайлики картинками.
 *  bg — цвет подложки: сглаживание смешивает буквы именно с ним. */
void drawWrapped(const WLine& l, int x, int cy, uint16_t fg, uint16_t bg) {
    setF(F_TEXT);
    tft.setTextDatum(textdatum_t::middle_left);
    tft.setTextColor(fg, bg);
    int cx = x;
    const char* p = l.s;
    const char* end = l.s + l.len;
    while (p < end) {
        size_t idx;
        const size_t n = emoji::match(p, idx);
        if (n) {
            emoji::draw(idx, cx, cy - emoji::kSize / 2, bg);
            cx += emoji::kSize + 2;
            p += n;
        } else {
            const int len = utf8Len(p);
            char buf[5] = {};
            memcpy(buf, p, size_t(len));
            tft.drawString(buf, cx, cy);
            cx += tft.textWidth(buf);
            p += len;
        }
    }
}

// ── пузыри ─────────────────────────────────────────────────────────────────────────────

constexpr int kBubbleMaxW  = 214;   // предел ширины текста в пузыре
constexpr int kBubblePadX  = 10;
constexpr int kBubblePadY  = 6;
constexpr int kBubbleGap   = 6;     // просвет между пузырями
constexpr int kTickW       = 20;    // место под галочки в своих пузырях
constexpr int kMaxWrap     = 8;

int lineH(const WLine& l) { return l.tall ? emoji::kSize + 4 : kLineText + 1; }


/** Сообщение из одних смайликов (пробелы не в счёт). Такие рисуются БЕЗ пузыря —
 *  смайлик сам себе форма, и подложка вокруг него выглядит нашлёпкой. */
bool emojiOnly(const char* text) {
    bool any = false;
    for (const char* p = text; *p; ) {
        if (*p == ' ') { ++p; continue; }
        size_t idx;
        const size_t n = emoji::match(p, idx);
        if (!n) return false;
        any = true;
        p += n;
    }
    return any;
}

/** Высота и ширина пузыря по разложенному тексту. */
void bubbleSize(const WLine* ls, int n, bool mine, int& w, int& h) {
    int maxW = 0, sumH = 0;
    for (int i = 0; i < n; ++i) {
        if (ls[i].w > maxW) maxW = ls[i].w;
        sumH += lineH(ls[i]);
    }
    w = maxW + kBubblePadX * 2 + (mine ? kTickW : 0);
    h = sumH + kBubblePadY * 2;
}
// Голосовой пузырь: кнопка проигрывания и подпись, размер постоянный.
constexpr int kVoiceW = 170;
constexpr int kVoiceH = 44;

/** Размер пузыря любого сообщения. Голосовое считается без раскладки текста —
 *  у него постоянная форма. Возвращает число строк текста (0 у голосового). */
int msgExtent(const Msg& m, WLine* ls, int& w, int& h) {
    if (m.media[0]) {
        w = kVoiceW + (m.mine ? kTickW : 0);
        h = kVoiceH;
        return 0;
    }
    const int n = wrapText(m.text, kBubbleMaxW, ls, kMaxWrap);
    bubbleSize(ls, n, m.mine, w, h);
    return n;
}


/** Галочки доставки: одна — ушло, две — доставлено. Совпадает с телефоном. */
void drawTicks(float rx, float cy, bool delivered, uint16_t bg) {
    const uint16_t c = delivered ? kTextPrimary : kAccentLight;
    auto tick = [&](float ox) {
        aaLine(rx + ox - 7, cy + 0.5f, rx + ox - 4, cy + 3.5f, 1.0f, c, bg);
        aaLine(rx + ox - 4, cy + 3.5f, rx + ox + 1, cy - 3, 1.0f, c, bg);
    };
    tick(delivered ? -4 : 0);
    if (delivered) tick(1);
}

/** Нарисовать один пузырь. yTop — верхняя кромка. Пузырь плоский, как в телефоне:
 *  без теней и кромок, форму даёт скругление и цвет. Сообщение из одних смайликов
 *  идёт без пузыря вовсе — bare говорит об этом. */
void drawBubble(const Msg& m, const WLine* ls, int n, int yTop, int w, int h, bool bare) {
    const int x = m.mine ? 312 - w : 8;
    const uint16_t fill = bare ? kBg : (m.mine ? kAccent : kSurfaceHigh);
    if (!bare) aaRoundRect(x, yTop, w, h, 9.0f, fill, kBg);

    if (m.media[0]) {
        // Кнопка проигрывания и подпись — как в телефоне: видно, что это звук, и
        // понятно, куда нажимать.
        const float pcx = x + 22, pcy = yTop + h / 2.0f;
        const uint16_t knob = m.mine ? kTextPrimary : kAccent;
        const uint16_t tri  = m.mine ? kAccent : kTextPrimary;
        aaCircle(pcx, pcy, 13.0f, knob, fill);
        aaLine(pcx - 3, pcy - 6, pcx + 6, pcy, 2.2f, tri, knob);
        aaLine(pcx - 3, pcy + 6, pcx + 6, pcy, 2.2f, tri, knob);
        aaLine(pcx - 3, pcy - 6, pcx - 3, pcy + 6, 2.0f, tri, knob);
        setF(F_TEXT);
        tft.setTextDatum(textdatum_t::middle_left);
        tft.setTextColor(kTextPrimary, fill);
        tft.drawString(m.text, x + 42, int(pcy) - 1);
    } else {
        int cy = yTop + kBubblePadY;
        for (int i = 0; i < n; ++i) {
            const int lh = lineH(ls[i]);
            drawWrapped(ls[i], x + kBubblePadX, cy + lh / 2, kTextPrimary, fill);
            cy += lh;
        }
    }
    if (m.mine) drawTicks(x + w - 9, yTop + h - 11, m.delivered, fill);
}

/** Нижняя граница ленты. */
int chatBottomY() { return 240 - kInputH - 4; }

/** Полная высота ленты — чтобы не дать прокрутке укатиться за края. */
int chatTotalHeight() {
    int total = 0;
    WLine ls[kMaxWrap];
    for (size_t i = 0; i < msgCount_; ++i) {
        int w, h;
        msgExtent(msgs_[i], ls, w, h);
        total += h + kBubbleGap;
    }
    return total;
}

/** Перерисовать ленту целиком (без шапки и строки ввода). */
void drawChatArea() {
    const int top = kHeaderH;
    const int bottom = chatBottomY();
    tft.fillRect(0, top, 320, bottom - top + 4, kBg);

    if (msgCount_ == 0) {
        setF(F_SMALL);
        tft.setTextDatum(textdatum_t::middle_center);
        tft.setTextColor(kTextTertiary, kBg);
        tft.drawString("Пока пусто — напишите первым", 160, (top + bottom) / 2);
        return;
    }

    // Не даём прокрутке выйти за края.
    const int viewH = bottom - top;
    const int total = chatTotalHeight();
    const int maxScroll = total > viewH ? total - viewH : 0;
    if (chatScroll_ > maxScroll) chatScroll_ = maxScroll;
    if (chatScroll_ < 0) chatScroll_ = 0;

    // Рисуем от свежего края вверх; частично видимые пузыри срезает область отсечения.
    tft.setClipRect(0, top, 320, viewH);
    int yBottom = bottom + chatScroll_;
    WLine ls[kMaxWrap];
    for (size_t k = msgCount_; k-- > 0; ) {
        const Msg& m = msgs_[k];
        int w, h;
        const int n = msgExtent(m, ls, w, h);
        const int yTop = yBottom - h;
        if (yTop > bottom) { yBottom = yTop - kBubbleGap; continue; }  // ниже окна
        if (yBottom < top) break;                                      // выше окна — всё
        drawBubble(m, ls, n, yTop, w, h, !m.media[0] && emojiOnly(m.text));
        yBottom = yTop - kBubbleGap;
    }
    tft.clearClipRect();
}

// ── список собеседников ────────────────────────────────────────────────────────────────

constexpr int kPeerRowH = 40;
constexpr int kPeerRows = (240 - kListY) / kPeerRowH;   // сколько строк помещается

/** Первая буква имени — в кружок-аватар. */
void firstLetter(const char* name, char out[5]) {
    memset(out, 0, 5);
    if (!name || !*name) { out[0] = '?'; return; }
    const int len = utf8Len(name);
    memcpy(out, name, size_t(len));
}

void drawPeerRow(size_t i, int y) {
    const bool sel = (i == peerSel_);
    const bool arm = (int(i) == deleteArm_);
    if (sel) aaRoundRect(4, y + 1, 312, kPeerRowH - 2, 8.0f, kSurfaceHigh, kBg);
    const uint16_t rowBg = sel ? kSurfaceHigh : kBg;

    // Аватар: цветной кружок с первой буквой. Цвет выводится из имени и постоянен.
    const int acx = 26, acy = y + kPeerRowH / 2;
    const uint16_t av = avatarColor(peerNames_[i]);
    aaCircle(acx, acy, 15.0f, av, rowBg);
    char letter[5];
    firstLetter(peerNames_[i], letter);
    setF(F_TEXT);
    tft.setTextDatum(textdatum_t::middle_center);
    tft.setTextColor(kTextPrimary, av);
    tft.drawString(letter, acx, acy - 1);

    // Точка присутствия — на кромке аватара, с ободком в цвет фона, чтобы читалась.
    aaCircle(acx + 11, acy + 10, 5.0f, rowBg, rowBg);
    aaCircle(acx + 11, acy + 10, 3.2f, peerOnline_[i] ? kOnline : kTextTertiary, rowBg);

    // Имя и строка состояния.
    setF(F_TEXT);
    tft.setTextDatum(textdatum_t::middle_left);
    tft.setTextColor(kTextPrimary, rowBg);
    tft.setClipRect(48, y, 320 - 48 - 44, kPeerRowH);
    tft.drawString(peerNames_[i] ? peerNames_[i] : "?", 48, y + 13);
    tft.clearClipRect();
    setF(F_SMALL);
    tft.setTextColor(arm ? kDanger : (peerOnline_[i] ? kOnline : kTextTertiary), rowBg);
    tft.drawString(arm ? "нажмите ещё раз — удалить"
                       : (peerOnline_[i] ? "в сети" : "не в сети"), 48, y + 29);

    // Непрочитанные: акцентный кружок с числом, левее крестика. Ровно та мелочь,
    // по которой глаз находит, куда заходить.
    if (peerUnread_[i] && !arm) {
        const float ucx = 320 - 56, ucy = acy;
        aaCircle(ucx, ucy, 10.0f, kAccent, rowBg);
        char n[6];
        snprintf(n, sizeof(n), "%u", unsigned(peerUnread_[i] > 99 ? 99 : peerUnread_[i]));
        setF(F_SMALL);
        tft.setTextDatum(textdatum_t::middle_center);
        tft.setTextColor(kTextPrimary, kAccent);
        tft.drawString(n, int(ucx), int(ucy));
    }

    // Крестик удаления справа. Взведённый — белый крест на красном круге.
    const float dcx = 320 - 22, dcy = acy;
    if (arm) {
        aaCircle(dcx, dcy, 11.0f, kDanger, rowBg);
        aaLine(dcx - 4, dcy - 4, dcx + 4, dcy + 4, 1.3f, kTextPrimary, kDanger);
        aaLine(dcx - 4, dcy + 4, dcx + 4, dcy - 4, 1.3f, kTextPrimary, kDanger);
    } else {
        aaLine(dcx - 4, dcy - 4, dcx + 4, dcy + 4, 1.3f, kTextTertiary, rowBg);
        aaLine(dcx - 4, dcy + 4, dcx + 4, dcy - 4, 1.3f, kTextTertiary, rowBg);
    }
}

void drawChatsList() {
    // Окно едет за выделением: строки крупные, и все собеседники разом не помещаются.
    if (peerSel_ < peerTop_) peerTop_ = peerSel_;
    if (peerSel_ >= peerTop_ + kPeerRows) peerTop_ = peerSel_ - kPeerRows + 1;

    tft.fillRect(0, kHeaderH, 320, 240 - kHeaderH, kBg);
    if (peerCount_ == 0) {
        setF(F_TEXT);
        tft.setTextDatum(textdatum_t::middle_center);
        tft.setTextColor(kTextSecond, kBg);
        tft.drawString("Пока никого нет", 160, 110);
        setF(F_SMALL);
        tft.setTextColor(kTextTertiary, kBg);
        tft.drawString("Меню → «Знакомство» — по радио или сети", 160, 134);
        return;
    }
    for (size_t i = peerTop_; i < peerCount_ && i < peerTop_ + kPeerRows; ++i)
        drawPeerRow(i, kListY + int(i - peerTop_) * kPeerRowH);
}

}  // namespace

// ── цвет аватара ───────────────────────────────────────────────────────────────────────

uint16_t avatarColor(const char* name) {
    static const uint16_t kPalette[] = {
        rgb(0xE0, 0x4F, 0x5F),  // малиновый
        rgb(0xE8, 0x8A, 0x2E),  // оранжевый
        rgb(0x2E, 0xB8, 0x72),  // зелёный
        rgb(0x2E, 0x9A, 0xE8),  // голубой
        rgb(0x6C, 0x4B, 0xFF),  // фиолетовый — тот же, что акцент
        rgb(0xD8, 0x4F, 0xC0),  // розовый
        rgb(0x18, 0xB8, 0xB0),  // бирюзовый
        rgb(0xB8, 0xA0, 0x2E),  // оливково-золотой
    };
    uint32_t h = 5381;
    for (const char* p = name; p && *p; ++p) h = h * 33 + uint8_t(*p);
    return kPalette[h % (sizeof(kPalette) / sizeof(kPalette[0]))];
}

// ── жизненный цикл ─────────────────────────────────────────────────────────────────────

void begin() {
    // Экран УЖЕ поднят на экране загрузки — повторная инициализация сбрасывает
    // контроллер и гасит картинку. Здесь только чистим поле под интерфейс.
    tft.setRotation(1);            // горизонтально: клавиатура снизу
    tft.fillScreen(kBg);
}

void draw(Screen s) {
    current_ = s;
    tft.fillScreen(kBg);

    switch (s) {
    case SCR_CHATS:
        drawHeaderBar("Вуаль", false);
        drawChatsList();
        break;

    case SCR_CHAT:
        drawHeaderBar(peerCount_ ? peerNames_[peerSel_] : "чат", true);
        drawChatArea();
        setInput(input_, 0, cyrillic_);
        break;

    case SCR_STATUS:
        drawHeaderBar("Состояние", true);
        break;

    default:
        break;
    }
}

void drawStatus(const Status& st) {
    // Обновляем ТОЛЬКО служебный текст в шапке, левее значков связи: перерисовывать
    // весь экран ради уровня сигнала значило бы моргать им при каждом принятом кадре.
    tft.fillRect(134, 0, 66, kHeaderH - 1, kSurface);
    tft.setTextDatum(textdatum_t::middle_right);
    setF(F_SMALL);

    char buf[32];
    if (st.linkUp) {
        // Заряд дописываем, только если он известен: этот вызов приходит и из приёма
        // кадра, где заряд не замеряется.
        if (st.battery) snprintf(buf, sizeof(buf), "%d дБм · %d%%", st.rssi, int(st.battery));
        else            snprintf(buf, sizeof(buf), "%d дБм", st.rssi);
        tft.setTextColor(kTextSecond, kSurface);
    } else {
        snprintf(buf, sizeof(buf), "%d%%", int(st.battery));
        tft.setTextColor(kTextTertiary, kSurface);
    }
    tft.drawString(buf, 198, kHeaderH / 2);
}

// ── лента ──────────────────────────────────────────────────────────────────────────────

void addMessage(const char* text, bool mine, uint32_t ts, bool delivered) {
    if (msgCount_ == kMaxMsgs) {
        memmove(msgs_, msgs_ + 1, sizeof(Msg) * (kMaxMsgs - 1));
        --msgCount_;
    }
    Msg& m = msgs_[msgCount_++];
    snprintf(m.text, sizeof(m.text), "%s", text);
    m.mine = mine;
    m.delivered = delivered;
    m.ts = ts;
    m.media[0] = 0;
    chatScroll_ = 0;               // новое сообщение прижимает ленту к свежему краю
    if (current_ == SCR_CHAT && !emojiOpen_) drawChatArea();
}

void addVoiceMessage(const char* path, int seconds, bool mine, bool delivered) {
    if (msgCount_ == kMaxMsgs) {
        memmove(msgs_, msgs_ + 1, sizeof(Msg) * (kMaxMsgs - 1));
        --msgCount_;
    }
    Msg& m = msgs_[msgCount_++];
    snprintf(m.text, sizeof(m.text), "Голосовое · %d с", seconds);
    m.mine = mine;
    m.delivered = delivered;
    m.ts = 0;
    snprintf(m.media, sizeof(m.media), "%s", path ? path : "");
    chatScroll_ = 0;
    if (current_ == SCR_CHAT && !emojiOpen_) drawChatArea();
}

const char* voiceAt(int index) {
    if (index < 0 || size_t(index) >= msgCount_) return "";
    return msgs_[size_t(index)].media;
}

namespace {
/** Какое сообщение лежит под точкой y ленты. Проходит раскладку так же, как отрисовка, —
 *  иначе попадание и картинка разойдутся. -1 — точка мимо пузырей. */
int messageAtY(int y) {
    const int bottom = chatBottomY();
    WLine ls[kMaxWrap];
    int yBottom = bottom + chatScroll_;
    for (size_t k = msgCount_; k-- > 0; ) {
        int w, h;
        msgExtent(msgs_[k], ls, w, h);
        const int yTop = yBottom - h;
        if (y >= yTop && y <= yBottom) return int(k);
        if (yBottom < kHeaderH) break;
        yBottom = yTop - kBubbleGap;
    }
    return -1;
}
}  // namespace

void clearMessages() {
    msgCount_ = 0;
    chatScroll_ = 0;
}

void markDelivered() {
    // Ищем последнее СВОЁ без галочки — подтверждение относится к нему.
    for (size_t k = msgCount_; k-- > 0; ) {
        if (!msgs_[k].mine) continue;
        if (msgs_[k].delivered) break;
        msgs_[k].delivered = true;
        if (current_ == SCR_CHAT && !emojiOpen_) drawChatArea();
        return;
    }
}

void chatScroll(int deltaPx) {
    if (current_ != SCR_CHAT || emojiOpen_) return;
    chatScroll_ += deltaPx;        // края выравнивает сама отрисовка
    drawChatArea();
}

// ── панель смайликов ───────────────────────────────────────────────────────────────────

void setEmojiOpen(bool open) {
    emojiOpen_ = open;
    if (open) {
        // Панель занимает низ экрана. Верх запоминаем: по нему же разбираются касания,
        // иначе нажатия попадали бы в ленту под панелью.
        const int rows = int((emoji::kCount + 5) / 6);
        emojiTop_ = 240 - kInputH - (rows * 46 + 6);
        emoji::drawPicker(emojiTop_, size_t(-1));
    } else {
        emojiTop_ = 240;
        draw(SCR_CHAT);          // панель закрылась — восстанавливаем ленту
    }
}

bool emojiOpen() { return emojiOpen_; }

// ── строка ввода ───────────────────────────────────────────────────────────────────────

void setInput(const char* text, size_t, bool cyrillic) {
    snprintf(input_, sizeof(input_), "%s", text ? text : "");
    cyrillic_ = cyrillic;
    if (current_ != SCR_CHAT) return;

    const int y = 240 - kInputH;
    tft.fillRect(0, y, 320, kInputH, kSurface);
    tft.drawFastHLine(0, y, 320, kSurfaceHigh);

    // Значок раскладки: видимый указатель и одновременно кнопка. Его регистром же
    // показывается Shift: «ру» — обычный набор, «Ру» — следующая буква заглавная,
    // «РУ» — верхний регистр включён. Слово и есть образец того, что получится.
    aaRoundRect(4, y + 5, kBadgeW - 4, kInputH - 10, 12.0f, kSurfaceHigh, kSurface);
    tft.setTextDatum(textdatum_t::middle_center);
    tft.setTextColor(cyrillic_ ? kAccentLight : kTextSecond, kSurfaceHigh);
    setF(F_SMALL);
    static const char* kBadge[2][3] = {{"en", "En", "EN"}, {"ру", "Ру", "РУ"}};
    tft.drawString(kBadge[cyrillic_ ? 1 : 0][shiftMode_ > 2 ? 0 : shiftMode_],
                   2 + kBadgeW / 2, y + kInputH / 2);

    // Поле ввода — тёмная капсула. Хвост длинного текста прижат вправо: видно то,
    // что набираешь сейчас.
    aaRoundRect(kFieldX, y + 5, kFieldW, kInputH - 10, 12.0f, kBg, kSurface);
    setF(F_TEXT);
    const int tw = input_[0] ? tft.textWidth(input_) : 0;
    tft.setClipRect(kFieldX + 8, y, kFieldW - 16, kInputH);
    tft.setTextColor(input_[0] ? kTextPrimary : kTextTertiary, kBg);
    if (tw > kFieldW - 16) {
        tft.setTextDatum(textdatum_t::middle_right);
        tft.drawString(input_, kFieldX + kFieldW - 8, y + kInputH / 2);
    } else {
        tft.setTextDatum(textdatum_t::middle_left);
        tft.drawString(input_[0] ? input_ : "Сообщение", kFieldX + 8, y + kInputH / 2);
    }
    tft.clearClipRect();

    // Микрофон: капсула, дужка-подкова и ножка — телефонный силуэт тонким штрихом.
    {
        const float mx = kMicCX, my = y + kInputH / 2.0f;
        const uint16_t mc = recording_ ? kDanger : kTextSecond;
        aaRoundRect(int(mx) - 3, int(my) - 11, 7, 12, 3.4f, mc, kSurface);
        aaArc(mx + 0.5f, my - 3, 6.5f, 1.0f, 270, 88, mc, kSurface);
        aaLine(mx + 0.5f, my + 3.5f, mx + 0.5f, my + 7, 1.0f, mc, kSurface);
        aaLine(mx - 3.5f, my + 7.5f, mx + 4.5f, my + 7.5f, 1.0f, mc, kSurface);
    }

    // Смайлики: рисуем сам смайлик, а не букву — понятно без подписи.
    emoji::draw(0, kEmojiCX - emoji::kSize / 2, y + (kInputH - emoji::kSize) / 2,
                kSurface);

    // Отправка: акцентный круг с бумажным самолётиком — как в телефоне. Раньше кнопки
    // не было видно вовсе — зона нажатия существовала, а рисунка в ней не было.
    {
        const float cx = kSendCX, cy = y + kInputH / 2.0f;
        aaCircle(cx, cy, 13.0f, kAccent, kSurface);
        // Самолётик: два сглаженных «крыла» к носу и хвостовая черта. Заливка
        // треугольником с обводкой давала двоящиеся кромки — три штриха чище.
        aaLine(cx - 5, cy - 5.5f, cx + 7, cy, 1.6f, kTextPrimary, kAccent);
        aaLine(cx - 5, cy + 5.5f, cx + 7, cy, 1.6f, kTextPrimary, kAccent);
        aaLine(cx - 4.0f, cy, cx + 1.5f, cy, 1.2f, kTextPrimary, kAccent);
    }
}

void setShiftMode(uint8_t mode) {
    if (mode == shiftMode_) return;
    shiftMode_ = mode;
    // Перерисовываем одну строку ввода — состояние Shift меняется на каждой букве, и
    // дёргать ради этого весь экран нельзя.
    if (current_ == SCR_CHAT && !recording_) setInput(input_, 0, cyrillic_);
}

Rect layoutBadgeRect() {
    // Область попадания шире самого значка: промах по кнопке раскладки особенно
    // раздражает.
    return Rect{0, int16_t(240 - kInputH), int16_t(kBadgeW + 6), int16_t(kInputH)};
}

// ── список ─────────────────────────────────────────────────────────────────────────────

void setPeers(const char* const* names, const bool* online, size_t count, size_t selected,
              const uint8_t* unread) {
    peerCount_ = count > kMaxPeers ? kMaxPeers : count;
    for (size_t i = 0; i < peerCount_; ++i) {
        peerNames_[i]  = names[i];
        peerOnline_[i] = online[i];
        peerUnread_[i] = unread ? unread[i] : 0;
    }
    peerSel_ = selected < peerCount_ ? selected : 0;
    if (deleteArm_ >= int(peerCount_)) deleteArm_ = -1;
    if (current_ == SCR_CHATS) { drawHeaderBar("Вуаль", false); drawChatsList(); }
}

void peersScroll(int deltaRows) {
    if (current_ != SCR_CHATS || peerCount_ <= size_t(kPeerRows)) return;
    const int maxTop = int(peerCount_) - kPeerRows;
    int top = int(peerTop_) + deltaRows;
    if (top < 0) top = 0;
    if (top > maxTop) top = maxTop;
    if (size_t(top) == peerTop_) return;
    peerTop_ = size_t(top);
    // Выделение оставляем как есть: палец листает ОКНО, а не выбор. Но отрисовка
    // подтягивает окно к выделению — поэтому на время ручной прокрутки этого не делаем.
    tft.fillRect(0, kHeaderH, 320, 240 - kHeaderH, kBg);
    for (size_t i = peerTop_; i < peerCount_ && i < peerTop_ + kPeerRows; ++i)
        drawPeerRow(i, kListY + int(i - peerTop_) * kPeerRowH);
}

void armDelete(int row) {
    deleteArm_ = row;
    if (current_ == SCR_CHATS) drawChatsList();
}

int armedDelete() { return deleteArm_; }

// ── прочие экраны ──────────────────────────────────────────────────────────────────────

namespace {

/** Значки пунктов меню, около 18×18 вокруг центра (cx, cy). Тонкий сглаженный штрих —
 *  единый стиль со значками шапки. */
void drawMenuIcon(MenuItem it, float cx, float cy, uint16_t c, uint16_t bg) {
    switch (it) {
    case MENU_IDENTITY:   // человек: голова и плечи
        aaArc(cx, cy - 4.5f, 3.6f, 1.0f, 90, 180, c, bg);
        aaArc(cx, cy + 9, 7.0f, 1.0f, 90, 62, c, bg);
        break;
    case MENU_CALIBRATE:  // прицел
        aaArc(cx, cy, 6.0f, 1.0f, 0, 180, c, bg);
        aaLine(cx - 9, cy, cx - 4, cy, 1.0f, c, bg);
        aaLine(cx + 4, cy, cx + 9, cy, 1.0f, c, bg);
        aaLine(cx, cy - 9, cx, cy - 4, 1.0f, c, bg);
        aaLine(cx, cy + 4, cx, cy + 9, 1.0f, c, bg);
        break;
    case MENU_ADD_NET:    // глобус
        aaArc(cx, cy, 8.0f, 1.0f, 0, 180, c, bg);
        aaLine(cx - 8, cy, cx + 8, cy, 0.9f, c, bg);
        // Меридианы — две пологие дуги с далеко отнесёнными центрами: вблизи они
        // выглядят как вертикальный эллипс, и «глобусность» читается.
        aaArc(cx - 8.2f, cy, 11.0f, 0.9f, 0, 42, c, bg);
        aaArc(cx + 8.2f, cy, 11.0f, 0.9f, 180, 42, c, bg);
        break;
    case MENU_ADD_RADIO:  drawRadioIcon(cx, cy, c, bg); break;
    case MENU_LOG:        // документ со строками
        aaRoundRect(int(cx) - 6, int(cy) - 8, 13, 17, 2.5f, c, bg);
        aaRoundRect(int(cx) - 4, int(cy) - 6, 9, 13, 1.5f, bg, c);
        for (int i = 0; i < 3; ++i)
            aaLine(cx - 2.5f, cy - 3.5f + i * 4, cx + 2.5f, cy - 3.5f + i * 4, 0.8f, c, bg);
        break;
    case MENU_WIFI:       drawWifiIcon(cx, cy - 2, c, bg); break;
    case MENU_STATUS:     // пульс
        aaLine(cx - 9, cy, cx - 5, cy, 1.0f, c, bg);
        aaLine(cx - 5, cy, cx - 2, cy - 6, 1.0f, c, bg);
        aaLine(cx - 2, cy - 6, cx + 2, cy + 6, 1.0f, c, bg);
        aaLine(cx + 2, cy + 6, cx + 5, cy, 1.0f, c, bg);
        aaLine(cx + 5, cy, cx + 9, cy, 1.0f, c, bg);
        break;
    case MENU_BACK:       // стрелка влево
        aaLine(cx - 7, cy, cx + 7, cy, 1.1f, c, bg);
        aaLine(cx - 7, cy, cx - 1, cy - 5.5f, 1.1f, c, bg);
        aaLine(cx - 7, cy, cx - 1, cy + 5.5f, 1.1f, c, bg);
        break;
    default: break;
    }
}

/** Поле ввода на служебных экранах: тёмная капсула с текстом или подсказкой. */
void drawField(int y, const char* value, const char* placeholder) {
    aaRoundRect(12, y, 296, 34, 10.0f, kSurfaceHigh, kBg);
    setF(F_TEXT);
    tft.setTextDatum(textdatum_t::middle_left);
    tft.setTextColor(value && *value ? kTextPrimary : kTextTertiary, kSurfaceHigh);
    tft.setClipRect(20, y, 282, 34);
    tft.drawString(value && *value ? value : placeholder, 22, y + 17);
    tft.clearClipRect();
}

/** Кнопка: акцентная или служебная. */
void drawButton(int x, int y, int w, int h, const char* label, bool accent) {
    aaRoundRect(x, y, w, h, 9.0f, accent ? kAccent : kSurfaceHigh, kBg);
    setF(F_SMALL);
    tft.setTextDatum(textdatum_t::middle_center);
    tft.setTextColor(accent ? kTextPrimary : kTextSecond, accent ? kAccent : kSurfaceHigh);
    tft.drawString(label, x + w / 2, y + h / 2);
}

}  // namespace

void drawMenu(MenuItem selected) {
    tft.fillScreen(kBg);
    drawHeaderBar("Меню", true);

    static const char* kLabels[MENU_COUNT] = {
        "Кто я",
        "Настроить сенсор",
        "Знакомство через интернет",
        "Знакомство по радио",
        "Журнал",
        "Подключиться к сети",
        "Состояние устройства",
        "Назад к перепискам",
    };
    for (int i = 0; i < MENU_COUNT; ++i) {
        const int y = kListY + i * kRowH;
        const bool sel = i == int(selected);
        if (sel) aaRoundRect(6, y, 308, kRowH - 2, 7.0f, kSurfaceHigh, kBg);
        const uint16_t bg = sel ? kSurfaceHigh : kBg;
        drawMenuIcon(MenuItem(i), 22, y + kRowH / 2.0f - 1,
                     sel ? kAccentLight : kTextTertiary, bg);
        setF(F_TEXT);
        tft.setTextDatum(textdatum_t::middle_left);
        tft.setTextColor(sel ? kTextPrimary : kTextSecond, bg);
        tft.drawString(kLabels[i], 44, y + kRowH / 2 - 1);
    }
}

void drawIdentity(const char* name, const char* addr, bool haveKey) {
    tft.fillScreen(kBg);
    drawHeaderBar("Кто я", true);

    tft.setTextDatum(textdatum_t::middle_left);
    setF(F_SMALL);
    tft.setTextColor(kTextSecond, kBg);
    tft.drawString("Имя, под которым вас видят собеседники.", 14, kListY + 6);
    tft.drawString("Ключ создаётся один раз и не меняется:", 14, kListY + 24);
    tft.drawString("по нему вас и узнают.", 14, kListY + 42);

    drawField(kListY + 56, name, "ваше имя");

    setF(F_SMALL);
    tft.setTextDatum(textdatum_t::middle_left);
    tft.setTextColor(haveKey ? kOnline : kTextTertiary, kBg);
    tft.drawString(haveKey ? "Ключ создан" : "Ключа ещё нет", 14, kListY + 104);
    if (haveKey && addr && *addr) {
        tft.setTextColor(kTextSecond, kBg);
        tft.drawString(addr, 14, kListY + 122);
    }

    drawButton(168, kListY + 136, 140, 26, "сохранить", true);
}

void drawAdd(const char* phrase, const char* status) {
    tft.fillScreen(kBg);
    drawHeaderBar("Новый собеседник", true);

    tft.setTextDatum(textdatum_t::middle_left);
    setF(F_SMALL);
    tft.setTextColor(kTextSecond, kBg);
    // Объясняем смысл, а не только просим ввести: без пояснения непонятно, откуда взять
    // фразу и что она делает.
    tft.drawString("Придумайте общую фразу и назовите её", 14, kListY + 6);
    tft.drawString("собеседнику любым способом. Введите её", 14, kListY + 24);
    tft.drawString("оба — связь установится сама.", 14, kListY + 42);

    drawField(kListY + 56, phrase, "кодовое слово");

    if (status && *status) {
        tft.setTextDatum(textdatum_t::middle_center);
        setF(F_SMALL);
        tft.setTextColor(kAccentLight, kBg);
        tft.drawString(status, 160, kListY + 108);
    }

    tft.setTextDatum(textdatum_t::middle_center);
    setF(F_SMALL);
    tft.setTextColor(kTextTertiary, kBg);
    tft.drawString("Enter — искать, влево — назад", 160, 228);
}

void drawAddRadio(const char* phrase, const char* status, int heard) {
    tft.fillScreen(kBg);
    drawHeaderBar("Знакомство по радио", true);

    tft.setTextDatum(textdatum_t::middle_left);
    setF(F_SMALL);
    tft.setTextColor(kTextSecond, kBg);
    // Объясняем главное отличие от сетевого: собеседник должен быть В ПРЕДЕЛАХ
    // СЛЫШИМОСТИ, и это не то же самое, что «где-то в интернете».
    tft.drawString("Оба устройства должны слышать друг друга.", 14, kListY + 6);
    tft.drawString("Введите общую фразу на обоих — и ждите:", 14, kListY + 24);
    tft.drawString("зов уходит примерно раз в полминуты.", 14, kListY + 42);

    drawField(kListY + 56, phrase, "кодовое слово");

    // Сколько узлов слышно: по этому числу видно, работает ли радио вообще, — иначе
    // непонятно, молчит собеседник или молчит наш приёмник.
    char h[40];
    snprintf(h, sizeof(h), "Слышно узлов: %d", heard);
    setF(F_SMALL);
    tft.setTextColor(heard > 0 ? kOnline : kTextTertiary, kBg);
    tft.drawString(h, 14, kListY + 104);

    if (status && *status) {
        tft.setTextColor(kAccentLight, kBg);
        tft.drawString(status, 14, kListY + 122);
    }

    tft.setTextDatum(textdatum_t::middle_center);
    tft.setTextColor(kTextTertiary, kBg);
    tft.drawString("Enter — начать звать, влево — назад", 160, 228);
}

void drawLog(const char* const* lines, size_t count, size_t from) {
    tft.fillScreen(kBg);
    drawHeaderBar("Журнал", true);

    tft.setTextDatum(textdatum_t::middle_left);
    setF(F_SMALL);

    // Десять строк — столько помещается читаемо. Показываем ПОСЛЕДНИЕ: разбираются
    // всегда свежие события, а до старых прокручивают.
    constexpr size_t kRows = 10;
    for (size_t i = 0; i < kRows && from + i < count; ++i) {
        const char* l = lines[from + i];
        if (!l) continue;
        // Предупреждения выделяем: в потоке строк они иначе теряются.
        const bool warn = strstr(l, "не ") || strstr(l, "НЕТ") || strstr(l, "ошиб");
        tft.setTextColor(warn ? kWarning : kTextSecond, kBg);
        tft.setClipRect(0, kListY, 320, 240 - kListY);
        tft.drawString(l, 8, kListY + 8 + int(i) * 17);
        tft.clearClipRect();
    }

    // Кнопка очистки: журнал растёт, и разбирать его через сотни строк неудобно —
    // после разбора проще стереть и смотреть заново с чистого места.
    drawButton(232, 212, 80, 24, "", false);
    setF(F_SMALL);
    tft.setTextDatum(textdatum_t::middle_center);
    tft.setTextColor(kDanger, kSurfaceHigh);
    tft.drawString("очистить", 272, 224);

    tft.setTextColor(kTextTertiary, kBg);
    tft.drawString("трекбол — листать", 110, 224);
}

void drawWifi(const char* const* names, const int* levels, size_t count,
              size_t selected, const char* status) {
    tft.fillScreen(kBg);
    drawHeaderBar("Сеть", true);

    if (count == 0) {
        tft.setTextDatum(textdatum_t::middle_center);
        setF(F_TEXT);
        tft.setTextColor(kTextSecond, kBg);
        tft.drawString(status && *status ? status : "Поиск сетей…", 160, 110);
    } else {
        // Показываем не больше пяти: на экране 320×240 больше не помещается читаемо,
        // а прокрутка списка сетей — редкая нужда.
        const size_t shown = count > 5 ? 5 : count;
        for (size_t i = 0; i < shown; ++i) {
            const int y = kListY + int(i) * 30;
            const bool sel = i == selected;
            if (sel) aaRoundRect(6, y, 308, 28, 7.0f, kSurfaceHigh, kBg);
            const uint16_t bg = sel ? kSurfaceHigh : kBg;

            // Столбики уровня: сильная сеть — три сочных, слабая — один тусклый.
            const int lvl = levels ? levels[i] : -100;
            const int bars = lvl > -55 ? 3 : lvl > -75 ? 2 : 1;
            for (int b = 0; b < 3; ++b) {
                const uint16_t c = b < bars ? (sel ? kAccentLight : kOnline)
                                            : kTextTertiary;
                aaRoundRect(14 + b * 6, y + 19 - b * 4, 4, 4 + b * 4, 1.5f, c, bg);
            }

            setF(F_TEXT);
            tft.setTextDatum(textdatum_t::middle_left);
            tft.setTextColor(sel ? kTextPrimary : kTextSecond, bg);
            tft.setClipRect(0, y, 250, 30);
            tft.drawString(names[i], 40, y + 13);
            tft.clearClipRect();
            char lvlText[12];
            snprintf(lvlText, sizeof(lvlText), "%d дБм", lvl);
            setF(F_SMALL);
            tft.setTextDatum(textdatum_t::middle_right);
            tft.setTextColor(kTextTertiary, bg);
            tft.drawString(lvlText, 306, y + 14);
        }
        if (status && *status) {
            tft.setTextDatum(textdatum_t::middle_center);
            setF(F_SMALL);
            tft.setTextColor(kAccentLight, kBg);
            tft.drawString(status, 160, 208);
        }
    }

    tft.setTextDatum(textdatum_t::middle_center);
    setF(F_SMALL);
    tft.setTextColor(kTextTertiary, kBg);
    tft.drawString("нажмите сеть или Enter", 160, 228);
}

void drawWifiPass(const char* ssid, const char* pass, bool reveal, const char* status) {
    tft.fillScreen(kBg);
    drawHeaderBar("Пароль сети", true);

    tft.setTextDatum(textdatum_t::middle_left);
    setF(F_TEXT);
    tft.setTextColor(kTextSecond, kBg);
    tft.drawString(ssid ? ssid : "", 14, kListY + 14);

    // Поле пароля: точками, но с подсказкой длины — иначе непонятно, набралось ли
    // вообще что-то.
    char shown[64] = {};
    const size_t n = pass ? strlen(pass) : 0;
    if (reveal) snprintf(shown, sizeof(shown), "%s", pass ? pass : "");
    else for (size_t i = 0; i < n && i < sizeof(shown) - 1; ++i) shown[i] = '*';
    drawField(kListY + 32, n > 0 ? shown : nullptr, "пароль");

    drawButton(12, kListY + 74, 140, 26, reveal ? "скрыть пароль" : "показать пароль",
               false);
    drawButton(168, kListY + 74, 140, 26, "подключиться", true);

    if (status && *status) {
        setF(F_SMALL);
        tft.setTextDatum(textdatum_t::middle_center);
        tft.setTextColor(kAccentLight, kBg);
        tft.drawString(status, 160, kListY + 118);
    }
}

void drawStatus2(const Status& st, const char* ip, bool radioOk, bool sdOk) {
    tft.fillScreen(kBg);
    drawHeaderBar("Состояние", true);

    char buf[64];
    int y = kListY + 8;
    auto line = [&](const char* label, const char* value, uint16_t color) {
        tft.setTextDatum(textdatum_t::middle_left);
        setF(F_SMALL);
        tft.setTextColor(kTextSecond, kBg);
        tft.drawString(label, 16, y);
        tft.setTextDatum(textdatum_t::middle_right);
        tft.setTextColor(color, kBg);
        tft.drawString(value, 306, y);
        y += 21;
    };

    line("Радио", radioOk ? "работает" : "не отвечает", radioOk ? kOnline : kDanger);
    line("Сеть", ip && *ip ? ip : "не подключена", ip && *ip ? kOnline : kTextTertiary);
    line("Карта памяти", sdOk ? "есть" : "нет", sdOk ? kOnline : kWarning);

    // Рельсы знакомства — ВСЕ ТРИ вида по отдельности: их закрывают разными способами
    // и в разных местах, и каждая может быть жива или мертва отдельно от остальных.
    {
        size_t up = 0;
        for (size_t i = 0; i < rail::brokerCount(); ++i) if (rail::brokerOnline(i)) ++up;
        char r[40];
        snprintf(r, sizeof(r), "%u из %u", unsigned(up), unsigned(rail::brokerCount()));
        line("Брокеры", r, up ? kOnline : kTextTertiary);

        const size_t nn = nostr::openCount();
        snprintf(r, sizeof(r), "%u из %u", unsigned(nn), unsigned(nostr::kMaxOpen));
        line("Nostr", r, nn ? kOnline : kTextTertiary);

        const size_t t = tracker::openCount();
        snprintf(r, sizeof(r), "%u из %u", unsigned(t), unsigned(tracker::kMaxOpen));
        line("Трекеры", r, t ? kOnline : kTextTertiary);
    }

    snprintf(buf, sizeof(buf), "%d", st.neighbours);
    line("Слышно узлов", buf, st.neighbours > 0 ? kTextPrimary : kTextTertiary);

    if (st.linkUp) {
        snprintf(buf, sizeof(buf), "%d дБм / %.0f дБ", st.rssi, double(st.snr));
        line("Последний приём", buf, kTextPrimary);
    }

    snprintf(buf, sizeof(buf), "%d%%", int(st.battery));
    line("Заряд", buf, st.battery < 20 ? kWarning : kTextPrimary);
}

// ── касания ────────────────────────────────────────────────────────────────────────────

HitResult hitTest(Screen s, int16_t x, int16_t y) {
    // Шапка: значок меню справа, стрелка назад слева. Одинаково на всех экранах —
    // человеку не приходится помнить, где он находится.
    if (y < kHeaderH) {
        if (x >= kBurgerX) return HitResult{HIT_BURGER, -1};
        if (x < 40 && s != SCR_CHATS) return HitResult{HIT_BACK, -1};
        return HitResult{HIT_NONE, -1};
    }

    switch (s) {
    case SCR_CHAT:
        // Строка ввода внизу: раскладка, микрофон, смайлики, отправка.
        if (y >= 240 - kInputH) {
            if (x < kBadgeW + 6) return HitResult{HIT_LAYOUT, -1};
            if (x >= kSendCX - 16) return HitResult{HIT_SEND, -1};
            if (x >= (kMicCX + kEmojiCX) / 2) return HitResult{HIT_EMOJI, -1};
            if (x >= kMicCX - 15) return HitResult{HIT_MIC, -1};
            return HitResult{HIT_NONE, -1};
        }
        // Панель смайликов, если открыта, забирает низ экрана себе. Проверяем ПОСЛЕ
        // строки ввода: она всё равно поверх панели.
        if (emojiOpen_ && y >= emojiTop_) {
            const int idx = emoji::pickerHit(emojiTop_, x, y);
            return idx >= 0 ? HitResult{HIT_EMOJI_CELL, idx} : HitResult{HIT_NONE, -1};
        }
        // Остальное — сама лента: касание голосового пузыря проигрывает именно его.
        if (y >= kHeaderH) return HitResult{HIT_ROW, messageAtY(y)};
        return HitResult{HIT_NONE, -1};

    case SCR_CHATS: {
        const int row = (y - kListY) / kPeerRowH;
        if (row < 0) return HitResult{HIT_NONE, -1};
        const int idx = int(peerTop_) + row;
        // Правый край строки — крестик удаления; область шире рисунка, чтобы
        // попадать пальцем.
        if (x >= 320 - 44) return HitResult{HIT_DELETE, idx};
        return HitResult{HIT_ROW, idx};
    }

    case SCR_MENU: {
        const int idx = (y - kListY) / kRowH;
        return idx >= 0 ? HitResult{HIT_ROW, idx} : HitResult{HIT_NONE, -1};
    }

    case SCR_WIFI: {
        const int idx = (y - kListY) / 30;
        return idx >= 0 ? HitResult{HIT_ROW, idx} : HitResult{HIT_NONE, -1};
    }

    case SCR_IDENTITY:
        if (y >= kListY + 136 && y <= kListY + 162 && x > 160) {
            return HitResult{HIT_ROW, 0};
        }
        return HitResult{HIT_NONE, -1};

    case SCR_LOG:
        // Кнопка очистки внизу справа.
        if (y >= 212 && x >= 232) return HitResult{HIT_ROW, 0};
        return HitResult{HIT_NONE, -1};

    case SCR_WIFI_PASS:
        // Две кнопки в один ряд: слева «показать», справа «подключиться».
        if (y >= kListY + 74 && y <= kListY + 100) {
            return HitResult{HIT_ROW, x < 160 ? 0 : 1};
        }
        return HitResult{HIT_NONE, -1};

    default:
        return HitResult{HIT_NONE, -1};
    }
}

// ── настройка сенсора ──────────────────────────────────────────────────────────────────
//
// Сенсорная панель и матрица — разные устройства, и их координаты совпадают лишь
// приблизительно. Без сопоставления палец попадает мимо: чем дальше от центра, тем
// сильнее промах. Библиотека умеет это сама: показывает точки по углам, человек тыкает
// в каждую, и по четырём попаданиям выводится поправка.

void calibrateTouch(uint16_t out[8]) {
    tft.fillScreen(kBg);

    tft.setTextDatum(textdatum_t::middle_center);
    setF(F_TITLE);
    tft.setTextColor(kTextPrimary, kBg);
    tft.drawString("Настройка сенсора", 160, 92);
    setF(F_SMALL);
    tft.setTextColor(kTextSecond, kBg);
    tft.drawString("Нажмите на появляющиеся уголки", 160, 122);
    tft.drawString("Точнее нажмёте — точнее попадания", 160, 140);
    delay(1500);

    // Уголки рисует сама библиотека; наше дело — цвета и сохранение результата.
    tft.calibrateTouch(out, kAccentLight, kBg, 20);

    tft.fillScreen(kBg);
    tft.setTextDatum(textdatum_t::middle_center);
    setF(F_TITLE);
    tft.setTextColor(kOnline, kBg);
    tft.drawString("Готово", 160, 120);
    delay(700);
}

// ── обновление шапки и записи ──────────────────────────────────────────────────────────

void setLinkState(bool wifi, bool radio, bool rail) {
    // Перерисовываем только при ИЗМЕНЕНИИ: значки обновляются раз в секунду, и рисовать
    // их каждый раз означало бы заметное мерцание шапки.
    if (wifi == wifiUp_ && radio == radioUp_ && rail == railUp_) return;
    wifiUp_ = wifi; radioUp_ = radio; railUp_ = rail;
    tft.fillRect(kBurgerX - 80, 0, 80, kHeaderH - 1, kSurface);
    drawStatusIcons();
}

void setTransfer(int percent, bool incoming, bool voice) {
    static int shown = -1;
    if (percent < 0) {
        // Полоска накрывала низ ленты — возвращаем его на место.
        if (shown >= 0 && current_ == SCR_CHAT && !emojiOpen_) drawChatArea();
        shown = -1;
        return;
    }
    if (current_ != SCR_CHAT || emojiOpen_ || recording_) { shown = percent; return; }
    shown = percent;

    const int hgt = 18;
    const int y = 240 - kInputH - hgt;
    aaRoundRect(6, y, 308, hgt - 2, 8.0f, kSurface, kBg);

    char label[48];
    snprintf(label, sizeof(label), "%s %s · %d%%",
             incoming ? "принимается" : "отправляется",
             voice ? "голосовое" : "файл", percent);
    setF(F_SMALL);
    tft.setTextDatum(textdatum_t::middle_left);
    tft.setTextColor(kTextSecond, kSurface);
    tft.drawString(label, 16, y + hgt / 2 - 2);

    // Тонкая нить хода по нижней кромке: проценты словами уже есть, нить видна
    // краем глаза с расстояния.
    const int w = 296 * (percent > 100 ? 100 : percent) / 100;
    if (w > 4) aaRoundRect(12, y + hgt - 5, w, 3, 1.5f, kAccent, kSurface);
}

void setRecording(bool on, uint32_t elapsedMs) {
    recording_ = on;
    recElapsed_ = elapsedMs;

    const int y = 240 - kInputH;
    tft.fillRect(0, y, 320, kInputH, kSurface);
    tft.drawFastHLine(0, y, 320, kSurfaceHigh);
    if (!on) { setInput(input_, 0, cyrillic_); return; }

    // КНОПКА ОСТАНОВКИ на своём месте — там же, где был микрофон. Она обязана
    // оставаться видимой, иначе человек решит, что остановить нельзя, — и будет прав.
    const float mx = kMicCX, my = y + kInputH / 2.0f;
    aaCircle(mx, my, 11.0f, kDanger, kSurface);
    aaRoundRect(int(mx) - 4, int(my) - 4, 8, 8, 2.0f, kTextPrimary, kDanger);

    // Полоска слева от кнопки, чтобы не налезать на неё.
    constexpr uint32_t kMaxMs = 60000;
    const int barW = kMicCX - 26;
    const int w = int(int64_t(barW) * (elapsedMs < kMaxMs ? elapsedMs : kMaxMs) / kMaxMs);
    aaRoundRect(10, int(my) - 3, barW, 6, 3.0f, kSurfaceHigh, kSurface);
    if (w > 6)
        aaRoundRect(10, int(my) - 3, w, 6, 3.0f,
                    elapsedMs > kMaxMs - 10000 ? kWarning : kDanger, kSurfaceHigh);

    // Оставшееся время — справа от кнопки.
    char t[16];
    const uint32_t left = elapsedMs < kMaxMs ? (kMaxMs - elapsedMs) / 1000 : 0;
    snprintf(t, sizeof(t), "%lu", (unsigned long)left);
    tft.setTextDatum(textdatum_t::middle_right);
    setF(F_SMALL);
    tft.setTextColor(kTextSecond, kSurface);
    tft.drawString(t, 314, my);
}

}  // namespace ui
