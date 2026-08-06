// Проверка ввода: распознавание жестов и раскладка. Ошибка здесь особенно неприятна —
// устройство начинает «жить своей жизнью», а причина неочевидна.
#include "input.h"

#include <cstdio>
#include <cstring>

static int fails = 0;
static void check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "  ok " : "СБОЙ", what);
    if (!ok) ++fails;
}

using namespace input;

static void testTap() {
    GestureState st;
    Event e = feedGesture(st, true, 100, 100, 1000);
    check(e.type == EV_NONE, "нажатие само по себе события не даёт");

    e = feedGesture(st, false, 102, 101, 1080);
    check(e.type == EV_TAP, "короткое касание на месте — это тап");
    check(e.x == 102 && e.y == 101, "координаты тапа переданы");
}

static void testSwipes() {
    // Возврат назад: смахивание справа налево. Привычный жест после телефона.
    GestureState st;
    feedGesture(st, true, 250, 120, 0);
    Event e = feedGesture(st, false, 100, 125, 200);
    check(e.type == EV_BACK, "смахивание влево — возврат назад");

    GestureState st2;
    feedGesture(st2, true, 60, 120, 0);
    e = feedGesture(st2, false, 220, 118, 200);
    check(e.type == EV_RIGHT, "смахивание вправо — вперёд");

    GestureState st3;
    feedGesture(st3, true, 160, 200, 0);
    e = feedGesture(st3, false, 158, 60, 200);
    check(e.type == EV_SWIPE_UP, "смахивание вверх — прокрутка");

    GestureState st4;
    feedGesture(st4, true, 160, 40, 0);
    e = feedGesture(st4, false, 162, 200, 200);
    check(e.type == EV_SWIPE_DOWN, "смахивание вниз — прокрутка");

    // Короткое движение не должно превращаться в жест: палец всегда чуть смещается.
    GestureState st5;
    feedGesture(st5, true, 160, 120, 0);
    e = feedGesture(st5, false, 160 + touch::kSwipeMin - 5, 120, 150);
    check(e.type == EV_TAP, "дрожание пальца остаётся тапом, а не жестом");
}

static void testLongPress() {
    // Удержание — это рация: говорим, пока держим.
    GestureState st;
    feedGesture(st, true, 160, 120, 0);
    Event e = feedGesture(st, true, 160, 120, touch::kLongPressMs - 50);
    check(e.type == EV_NONE, "до порога удержание не срабатывает");

    e = feedGesture(st, true, 161, 121, touch::kLongPressMs + 10);
    check(e.type == EV_LONG_PRESS, "после порога — удержание");

    e = feedGesture(st, true, 161, 121, touch::kLongPressMs + 500);
    check(e.type == EV_NONE, "удержание срабатывает ОДИН раз, а не потоком");

    e = feedGesture(st, false, 161, 121, touch::kLongPressMs + 900);
    check(e.type == EV_RELEASE, "отпускание завершает удержание");

    // Медленное смахивание не должно становиться удержанием на полпути — иначе рация
    // включалась бы при каждой неспешной прокрутке.
    GestureState st2;
    feedGesture(st2, true, 250, 120, 0);
    Event mid = feedGesture(st2, true, 150, 120, touch::kLongPressMs + 10);
    check(mid.type == EV_NONE, "движение отменяет удержание");
    Event up = feedGesture(st2, false, 100, 120, touch::kLongPressMs + 200);
    check(up.type == EV_BACK, "и жест распознаётся как смахивание");
}

static void testLayout() {
    using namespace input::keyboard;
    char out[3];

    check(cyrillicUtf8('q', out) == 2, "клавиша даёт два байта UTF-8");
    check(strcmp(out, "й") == 0, "Q даёт Й — как на обычной клавиатуре");

    cyrillicUtf8('w', out); check(strcmp(out, "ц") == 0, "W даёт Ц");
    cyrillicUtf8('a', out); check(strcmp(out, "ф") == 0, "A даёт Ф");
    cyrillicUtf8('m', out); check(strcmp(out, "ь") == 0, "M даёт Ь");
    cyrillicUtf8('Z', out); check(strcmp(out, "Я") == 0,
                                  "заглавная латиница даёт ЗАГЛАВНУЮ кириллицу");
    cyrillicUtf8('Q', out); check(strcmp(out, "Й") == 0, "Shift+Q даёт Й");
    cyrillicUtf8('H', out); check(strcmp(out, "Р") == 0,
                                  "Shift+H даёт Р — у «р…я» меняется и первый байт");

    // ── перебор ────────────────────────────────────────────────────────────────────────
    // Буквы, которым не хватило клавиш, добираются повторным нажатием той же клавиши.
    {
        bool repl = false;
        multiTapReset();
        check(multiTap('t', false, 1000, out, repl) == 2 && strcmp(out, "е") == 0 && !repl,
              "первое нажатие цепочки — обычная буква");
        check(multiTap('t', false, 1300, out, repl) == 2 && strcmp(out, "ё") == 0 && repl,
              "второе в окне — Ё вместо Е");
        check(multiTap('t', false, 1600, out, repl) == 2 && strcmp(out, "е") == 0 && repl,
              "третье — цепочка идёт по кругу");
        check(multiTap('t', false, 9000, out, repl) == 2 && strcmp(out, "е") == 0 && !repl,
              "окно вышло — снова первая буква, без замены");

        multiTap('l', false, 100, out, repl);
        multiTap('l', false, 300, out, repl);
        check(strcmp(out, "ж") == 0 && repl, "Д→Ж");
        multiTap('l', false, 500, out, repl);
        check(strcmp(out, "э") == 0 && repl, "Ж→Э");

        multiTap('T', true, 2000, out, repl);
        check(strcmp(out, "Е") == 0, "перебор в верхнем регистре");
        multiTap('t', false, 2200, out, repl);
        check(strcmp(out, "Ё") == 0 && repl,
              "регистр цепочки берётся у первой буквы: Shift уже погашен");

        check(multiTap('q', false, 100, out, repl) == 0, "клавиша вне цепочек — обычная раскладка");
        // х, ъ, ё, ж, э, б, ю — все добираются перебором
        multiTapReset(); multiTap('p', false, 0, out, repl); multiTap('p', false, 100, out, repl);
        check(strcmp(out, "х") == 0, "З→Х");
        multiTapReset(); multiTap('m', false, 0, out, repl); multiTap('m', false, 100, out, repl);
        check(strcmp(out, "ъ") == 0, "Ь→Ъ");
        multiTapReset(); multiTap('e', false, 0, out, repl); multiTap('e', false, 100, out, repl);
        check(strcmp(out, "ю") == 0, "У→Ю");
        multiTapReset(); multiTap('b', false, 0, out, repl); multiTap('b', false, 100, out, repl);
        check(strcmp(out, "б") == 0, "И→Б");
    }

    // ── залипающий Shift ───────────────────────────────────────────────────────────────
    {
        check(shiftMode() == SHIFT_OFF, "изначально выключен");
        shiftTap(1000);
        check(shiftMode() == SHIFT_ONCE, "одно нажатие — одна заглавная");
        shiftConsume();
        check(shiftMode() == SHIFT_OFF, "буква гасит одноразовый");
        shiftTap(2000); shiftTap(2300);
        check(shiftMode() == SHIFT_CAPS, "быстрое двойное — верхний регистр");
        shiftConsume();
        check(shiftMode() == SHIFT_CAPS, "буква верхний регистр НЕ гасит");
        shiftTap(3000);
        check(shiftMode() == SHIFT_OFF, "нажатие выключает верхний регистр");
        shiftTap(4000); shiftTap(5000);
        check(shiftMode() == SHIFT_OFF, "медленное второе — передумал, выключено");
    }

    check(cyrillicUtf8('1', out) == 0, "у цифры кириллицы нет");
    check(cyrillicUtf8(' ', out) == 0, "у пробела кириллицы нет");

    // Все буквы раскладки должны отдавать что-то осмысленное: пропуск означал бы
    // невводимую букву, и обнаружилось бы это в самый неподходящий момент.
    const char* all = "qwertyuiopasdfghjklzxcvbnm";
    bool complete = true;
    for (const char* p = all; *p; ++p) if (cyrillicUtf8(*p, out) != 2) complete = false;
    check(complete, "все 26 клавиш дают кириллицу");

    // Буквы не должны повторяться: повтор означает потерянную букву алфавита.
    bool unique = true;
    char seen[26][3] = {};
    size_t n = 0;
    for (const char* p = all; *p; ++p) {
        cyrillicUtf8(*p, out);
        for (size_t i = 0; i < n; ++i) if (strcmp(seen[i], out) == 0) unique = false;
        memcpy(seen[n++], out, 3);
    }
    check(unique, "буквы не повторяются");

    setLayout(LAYOUT_CYRILLIC);
    check(layout() == LAYOUT_CYRILLIC, "раскладка переключается");
    setLayout(LAYOUT_LATIN);
}

int main() {
    printf("── касание ──\n");   testTap();
    printf("── жесты ──\n");     testSwipes();
    printf("── удержание ──\n"); testLongPress();
    printf("── раскладка ──\n"); testLayout();
    printf("\n%s\n", fails ? "ЕСТЬ СБОИ" : "всё сошлось");
    return fails ? 1 : 0;
}
