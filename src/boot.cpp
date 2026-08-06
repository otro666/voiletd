#include "boot.h"

#include <Arduino.h>
#include "display.h"
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "ui.h"

// Единственный объект экрана — создаётся при первом обращении.
VualDisplay& vualScreen() { static VualDisplay inst; return inst; }

namespace boot {

namespace {

VualDisplay& tft = vualScreen();

constexpr int kTop = 74;        // под заставкой
constexpr int kLineH = 15;
constexpr int kMaxLines = 10;

int  lineY = kTop;
char lastName[40] = {};
bool haveStep = false;

void drawSplash() {
    // Знак: круг с вертикальной чертой — тот же силуэт, что у значка приложения.
    const int cx = 160, cy = 34;
    tft.drawCircle(cx, cy, 17, ui::kAccent);
    tft.drawCircle(cx, cy, 16, ui::kAccent);
    tft.drawFastVLine(cx + 12, cy, 20, ui::kAccent);

    tft.setTextDatum(textdatum_t::middle_center);
    tft.setTextColor(ui::kTextPrimary, ui::kBg);
    tft.setFont(VUAL_FONT_TITLE); tft.drawString("Vual", cx, cy + 34);

    tft.setTextColor(ui::kTextTertiary, ui::kBg);
    tft.setFont(VUAL_FONT_SMALL); tft.drawString("загрузка", cx, cy + 56);
}

uint16_t colorOf(Status st) {
    switch (st) {
    case OK:   return ui::kOnline;
    case WARN: return ui::kWarning;
    case FAIL: return ui::kDanger;
    default:   return ui::kTextSecond;
    }
}

const char* markOf(Status st) {
    switch (st) {
    case OK:   return "+";
    case WARN: return "!";
    case FAIL: return "x";
    default:   return ".";
    }
}

}  // namespace

void begin() {
    // Каждый шаг пишем в порт ОТДЕЛЬНОЙ строкой: при падении внутри библиотеки экрана
    // последняя напечатанная строка указывает на конкретный вызов. Без этого падение
    // выглядит как «поднимаю экран...» и обрыв, а виновника не найти.
    Serial.println("  подсветка");
    pinMode(BOARD_TFT_BL, OUTPUT);
    digitalWrite(BOARD_TFT_BL, HIGH);

    Serial.println("  tft.init()");
    tft.init();

    Serial.println("  поворот");
    tft.setRotation(1);

    Serial.println("  заливка фона");
    tft.fillScreen(ui::kBg);

    Serial.println("  заставка");
    drawSplash();
    Serial.println("  экран готов");
    lineY = kTop;
    haveStep = false;
}

void step(const char* name) {
    if (haveStep) done(OK);          // предыдущий не закрыли — считаем удачным

    snprintf(lastName, sizeof(lastName), "%s", name ? name : "");
    haveStep = true;

    // Список не прокручиваем: шагов немного, а прокрутка на этом экране стоит дорого.
    // Переполнение означало бы, что шагов стало больше — тогда и менять.
    if (lineY > kTop + kLineH * kMaxLines) return;

    tft.setTextDatum(textdatum_t::middle_left);
    tft.setTextColor(ui::kTextSecond, ui::kBg);
    tft.setFont(VUAL_FONT_SMALL); tft.drawString(".", 14, lineY);
    tft.setFont(VUAL_FONT_SMALL); tft.drawString(lastName, 30, lineY);

    Serial.printf("[загрузка] %s ...\n", lastName);
}

void done(Status st, const char* detail) {
    if (!haveStep) return;
    haveStep = false;

    if (lineY <= kTop + kLineH * kMaxLines) {
        // Перерисовываем только значок и хвост строки: полная очистка строки заметно
        // мигает, а значок и так однобуквенный.
        tft.setTextDatum(textdatum_t::middle_left);
        tft.setTextColor(colorOf(st), ui::kBg);
        tft.setFont(VUAL_FONT_SMALL); tft.drawString(markOf(st), 14, lineY);

        if (detail && *detail) {
            tft.setTextDatum(textdatum_t::middle_right);
            tft.setTextColor(colorOf(st), ui::kBg);
            tft.setFont(VUAL_FONT_SMALL); tft.drawString(detail, 310, lineY);
        }
    }
    lineY += kLineH;

    Serial.printf("[загрузка] %s -> %s%s%s\n", lastName, markOf(st),
                  detail && *detail ? " " : "", detail ? detail : "");
}

void finish() {
    // Экран отдаём приложению. Заставку не стираем сами: следующий экран нарисует поверх.
    Serial.println("[загрузка] готово");
}

void fatal(const char* what) {
    tft.fillRect(0, 200, 320, 40, ui::kSurface);
    tft.setTextDatum(textdatum_t::middle_center);
    tft.setTextColor(ui::kDanger, ui::kSurface);
    tft.setFont(VUAL_FONT_TEXT); tft.drawString(what ? what : "?", 160, 214);
    tft.setTextColor(ui::kTextTertiary, ui::kSurface);
    tft.setFont(VUAL_FONT_SMALL); tft.drawString("RST — перезапуск", 160, 230);
    Serial.printf("[загрузка] ОСТАНОВ: %s\n", what ? what : "?");
}

}  // namespace boot
