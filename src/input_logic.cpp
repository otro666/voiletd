// Переносимая часть ввода: распознавание жестов и раскладка. Не зависит от железа,
// поэтому проверяется тестами на компьютере — в отличие от чтения самих контроллеров.
#include "input.h"

namespace input {

Event feedGesture(GestureState& st, bool pressed, int16_t x, int16_t y, uint32_t nowMs) {
    Event e{};
    e.type = EV_NONE;

    // Палец опустился — запоминаем точку и время, событие пока не выдаём: неизвестно,
    // что это будет — касание, удержание или смахивание.
    if (pressed && !st.down) {
        st.down = true;
        st.x0 = x; st.y0 = y; st.t0 = nowMs;
        st.longFired = false;
        st.dragging = false;
        return e;
    }

    // Палец на экране: следим за удержанием и за перетаскиванием.
    if (pressed && st.down) {
        const int16_t dx = int16_t(x - st.x0);
        const int16_t dy = int16_t(y - st.y0);
        const int16_t adx = dx < 0 ? int16_t(-dx) : dx;
        const int16_t ady = dy < 0 ? int16_t(-dy) : dy;

        // Живое перетаскивание: вертикальное движение подхватывается СРАЗУ, ещё под
        // пальцем, и лента едет вместе с ним — прокрутка «на отпускание» ощущается
        // глухой. Горизонталь не трогаем: она остаётся жестом возврата.
        if (!st.dragging && !st.longFired &&
            ady >= touch::kDragStart && ady > adx) {
            st.dragging = true;
            st.lastY = y;
        }
        if (st.dragging) {
            const int16_t step = int16_t(y - st.lastY);
            if (step != 0) {
                st.lastY = y;
                e.type = EV_DRAG;
                e.x = 0; e.y = step;
            }
            return e;
        }

        const bool moved = (adx > touch::kSwipeMin || ady > touch::kSwipeMin);
        // Удержание засчитываем, только если палец стоит на месте: иначе медленное
        // смахивание превращалось бы в удержание на полпути.
        if (!st.longFired && !moved && nowMs - st.t0 >= touch::kLongPressMs) {
            st.longFired = true;
            e.type = EV_LONG_PRESS;
            e.x = st.x0; e.y = st.y0;
        }
        return e;
    }

    // Палец поднялся — решаем, что это было.
    if (!pressed && st.down) {
        st.down = false;
        if (st.longFired) { e.type = EV_RELEASE; return e; }
        // Перетаскивание уже отработало по ходу движения — на отпускании ничего.
        if (st.dragging) { st.dragging = false; return e; }

        const int16_t dx = int16_t(x - st.x0);
        const int16_t dy = int16_t(y - st.y0);
        const int16_t adx = dx < 0 ? int16_t(-dx) : dx;
        const int16_t ady = dy < 0 ? int16_t(-dy) : dy;

        // Смахивание по преобладающей оси. Горизонтальное справа налево — возврат назад:
        // привычный жест, и его же ждёт рука после телефона. В событие кладём пройденный
        // путь: вертикальные смахивания прокручивают ленты, и прокрутка должна быть
        // соразмерна движению пальца, а не одинаковым шагом.
        if (adx >= touch::kSwipeMin && adx > ady) {
            e.type = (dx < 0) ? EV_BACK : EV_RIGHT;
            e.x = dx; e.y = dy;
            return e;
        }
        if (ady >= touch::kSwipeMin && ady > adx) {
            e.type = (dy < 0) ? EV_SWIPE_UP : EV_SWIPE_DOWN;
            e.x = dx; e.y = dy;
            return e;
        }
        e.type = EV_TAP;
        e.x = x; e.y = y;
        return e;
    }
    return e;
}

namespace keyboard {

namespace { Layout g_layout = LAYOUT_LATIN; }

void setLayout(Layout l) { g_layout = l; }
Layout layout() { return g_layout; }

// ── Shift ──────────────────────────────────────────────────────────────────────────────

namespace {
ShiftMode g_shift = SHIFT_OFF;
uint32_t  g_shiftTapMs = 0;
/** Окно двойного нажатия. Шире окна перебора не нужно: двойное нажатие — жест быстрый. */
constexpr uint32_t kShiftDoubleMs = 600;
}  // namespace

ShiftMode shiftMode() { return g_shift; }

void shiftTap(uint32_t nowMs) {
    switch (g_shift) {
    case SHIFT_OFF:
        g_shift = SHIFT_ONCE;
        break;
    case SHIFT_ONCE:
        // Быстрое второе нажатие — верхний регистр до следующего нажатия; медленное —
        // передумал, выключаем.
        g_shift = (nowMs - g_shiftTapMs < kShiftDoubleMs) ? SHIFT_CAPS : SHIFT_OFF;
        break;
    case SHIFT_CAPS:
        g_shift = SHIFT_OFF;
        break;
    }
    g_shiftTapMs = nowMs;
}

void shiftConsume() {
    if (g_shift == SHIFT_ONCE) g_shift = SHIFT_OFF;
}

// ── перебор ────────────────────────────────────────────────────────────────────────────

namespace {
/** Цепочки перебора: чем добираются буквы, которым не хватило клавиш. Подобраны по
 *  соседству на настоящей клавиатуре (з-х, д-ж-э) и по родству (е-ё, ь-ъ, у-ю, и-б). */
struct Cycle { char key; const char* lower; const char* upper; uint8_t n; };
const Cycle kCycles[] = {
    {'t', "её",  "ЕЁ",  2},
    {'e', "ую",  "УЮ",  2},
    {'b', "иб",  "ИБ",  2},
    {'p', "зх",  "ЗХ",  2},
    {'l', "джэ", "ДЖЭ", 3},
    {'m', "ьъ",  "ЬЪ",  2},
};

char     g_tapKey = 0;
uint32_t g_tapMs = 0;
uint8_t  g_tapIdx = 0;
bool     g_tapUpper = false;
/** Окно перебора: успеть нажать ещё раз, но не путать с обычным повтором буквы. */
constexpr uint32_t kTapWindowMs = 700;
}  // namespace

void multiTapReset() { g_tapKey = 0; }

size_t multiTap(char latin, bool upper, uint32_t nowMs, char out[3], bool& replacePrev) {
    replacePrev = false;
    const char lower = (latin >= 'A' && latin <= 'Z') ? char(latin + 32) : latin;

    const Cycle* c = nullptr;
    for (const Cycle& k : kCycles) if (k.key == lower) { c = &k; break; }
    if (!c) { g_tapKey = 0; return 0; }

    if (lower == g_tapKey && nowMs - g_tapMs < kTapWindowMs) {
        // Та же клавиша в окне — следующая буква цепочки ВМЕСТО только что введённой.
        // Регистр берём тот, что был у первой: Shift к этому времени уже погашен, а
        // человек продолжает ту же букву, просто «докручивает» её.
        g_tapIdx = uint8_t((g_tapIdx + 1) % c->n);
        replacePrev = true;
    } else {
        g_tapIdx = 0;
        g_tapUpper = upper;
    }
    g_tapKey = lower;
    g_tapMs = nowMs;

    const char* src = (g_tapUpper ? c->upper : c->lower) + size_t(g_tapIdx) * 2;
    out[0] = src[0]; out[1] = src[1]; out[2] = 0;
    return 2;
}

size_t cyrillicUtf8(char latin, char out[3]) {
    // Соответствие по МЕСТУ клавиши, как на обычной компьютерной клавиатуре.
    // Строка в порядке латинских клавиш q..p, a..l, z..m — так её видно целиком
    // и легко сверить с настоящей раскладкой.
    static const char* kRow1 = "йцукенгшщз";
    static const char* kRow2 = "фывапролд";
    static const char* kRow3 = "ячсмитьбю";
    static const char* kLat1 = "qwertyuiop";
    static const char* kLat2 = "asdfghjkl";
    static const char* kLat3 = "zxcvbnm";

    struct Row { const char* lat; const char* cyr; };
    const Row rows[3] = { {kLat1, kRow1}, {kLat2, kRow2}, {kLat3, kRow3} };

    // Регистр запоминаем ДО приведения: Shift на клавиатуре даёт заглавную латиницу,
    // и она обязана превращаться в заглавную кириллицу. Раньше регистр стирался,
    // и заглавных русских букв не существовало вовсе.
    const bool upper = (latin >= 'A' && latin <= 'Z');
    const char lower = upper ? char(latin + 32) : latin;
    for (const Row& r : rows) {
        for (size_t i = 0; r.lat[i]; ++i) {
            if (r.lat[i] != lower) continue;
            // Каждая кириллическая буква — ровно два байта UTF-8, поэтому смещение
            // считается умножением, без разбора строки.
            const char* p = r.cyr + i * 2;
            if (!p[0] || !p[1]) return 0;
            if (upper) {
                // а…я = U+0430…U+044F, заглавные ровно на 0x20 ниже. Работать надо с
                // кодом символа, а не байтами: у «р…я» при этом меняется и первый байт.
                uint32_t cp = ((uint8_t(p[0]) & 0x1F) << 6) | (uint8_t(p[1]) & 0x3F);
                cp -= 0x20;
                out[0] = char(0xC0 | (cp >> 6));
                out[1] = char(0x80 | (cp & 0x3F));
            } else {
                out[0] = p[0]; out[1] = p[1];
            }
            out[2] = 0;
            return 2;
        }
    }
    return 0;
}

}  // namespace keyboard

}  // namespace input
