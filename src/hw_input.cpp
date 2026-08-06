// Чтение контроллеров: клавиатура, трекбол, сенсорный экран.
//
// Это единственная часть проекта, которую нельзя проверить без платы — она разговаривает
// с железом. Логика поверх неё (жесты, раскладка) вынесена в input_logic.cpp и покрыта
// тестами; здесь только чтение регистров.
#include "input.h"

#include <Arduino.h>
#include <Wire.h>

#include "board.h"
#include "display.h"

namespace input {

// ── клавиатура ─────────────────────────────────────────────────────────────────────────
//
// Клавиатура T-Deck висит на отдельном микроконтроллере ESP32-C3 и отдаёт по I2C ОДИН
// байт — код уже нажатой клавиши. Модификаторы обрабатывает она сама: Alt с буквой сразу
// даёт напечатанный на клавише символ, а нажатые в одиночку Alt и Shift не возвращают
// ничего. Из-за этого повесить переключение раскладки на сам Alt невозможно — мы просто
// не узнаем о его нажатии.
namespace keyboard {

namespace {

uint32_t g_lastKeyMs = 0;
uint8_t  g_lastKey = 0;

/**
 * Очередь исходящих байтов. Одно нажатие теперь может породить НЕСКОЛЬКО событий:
 * перебор меняет уже введённую букву, а это стирание плюс два байта новой. Событие
 * несёт один байт — остальные ждут следующих опросов. Код 0x08 в очереди — стирание.
 */
uint8_t g_q[6] = {};
uint8_t g_qn = 0, g_qhead = 0;

void qPush(uint8_t b) { if (g_qn < sizeof(g_q)) g_q[(g_qhead + g_qn++) % sizeof(g_q)] = b; }
bool qPop(uint8_t& b) {
    if (!g_qn) return false;
    b = g_q[g_qhead];
    g_qhead = uint8_t((g_qhead + 1) % sizeof(g_q));
    --g_qn;
    return true;
}

/**
 * Диагностика: показывать код каждой нажатой клавиши в порт.
 *
 * Прошивки клавиатуры у разных партий T-Deck отличаются, и какой именно байт приходит от
 * Sym или Alt+пробел — вопрос эмпирический. Включите на время, нажмите нужную клавишу и
 * посмотрите код в мониторе порта, затем впишите его в kLayoutKey или kShiftKey.
 */
constexpr bool kDumpCodes = false;

}  // namespace

/**
 * Клавиша переключения раскладки.
 *
 * По умолчанию — символ $ (на T-Deck это Alt вместе с клавишей динамика). Выбран потому,
 * что в переписке практически не встречается, а нажать его легко одной рукой.
 *
 * Если у вашей прошивки другой код — включите kDumpCodes, нажмите желаемую клавишу и
 * впишите сюда увиденное значение.
 */
constexpr uint8_t kLayoutKey = '$';

/**
 * Код одиночного нажатия Shift.
 *
 * Штатная прошивка клавиатуры про одиночный Shift обычно МОЛЧИТ (она сама поднимает
 * регистр при аккорде), но у части партий он приходит отдельным кодом. Если у вашей
 * приходит — включите kDumpCodes, подсмотрите код и впишите сюда: тогда заработает
 * залипание. Ноль — код неизвестен; залипание тогда включается только аккордами.
 */
constexpr uint8_t kShiftKey = 0;

void begin() {
    // Клавиатура поднимается не мгновенно после подачи питания на периферию: её
    // микроконтроллер грузится сам. Раньше этого читать бесполезно.
    delay(200);
}

Event poll() {
    Event e{};
    e.type = EV_NONE;

    // Хвосты очереди отдаём ПЕРВЫМ делом, до чтения клавиатуры: иначе порядок байт
    // нарушится и вместо буквы получится мусор.
    {
        uint8_t b;
        if (qPop(b)) {
            if (b == 0x08) { e.type = EV_BACKSPACE; return e; }
            e.type = EV_CHAR;
            e.ch = char(b);
            return e;
        }
    }

    Wire.requestFrom(int(kI2cAddr), 1);
    if (!Wire.available()) return e;
    const uint8_t k = uint8_t(Wire.read());
    if (k == 0) { g_lastKey = 0; return e; }

    // Защита от дребезга и от удержания: тот же код подряд принимаем не чаще, чем раз в
    // 120 мс. Иначе одно нажатие даёт десяток букв.
    const uint32_t now = millis();
    if (k == g_lastKey && now - g_lastKeyMs < 120) return e;
    g_lastKey = k;
    g_lastKeyMs = now;

    if (kDumpCodes) Serial.printf("клавиша: 0x%02X (%c)\n", k, k >= 32 ? char(k) : '?');

    switch (k) {
    case 0x08: multiTapReset(); e.type = EV_BACKSPACE; return e;
    case 0x0D: multiTapReset(); e.type = EV_ENTER;     return e;
    case kLayoutKey:
        // Переключение раскладки. Никакого символа при этом не вводится.
        setLayout(layout() == LAYOUT_LATIN ? LAYOUT_CYRILLIC : LAYOUT_LATIN);
        multiTapReset();
        e.type = EV_NONE;
        return e;
    default: break;
    }

    if (kShiftKey && k == kShiftKey) {   // одиночный Shift, если прошивка его отдаёт
        shiftTap(now);
        return e;
    }

    if (k < 32) return e;                       // прочие управляющие пропускаем

    const char raw = char(k);
    const bool isLower = raw >= 'a' && raw <= 'z';
    const bool isUpper = raw >= 'A' && raw <= 'Z';

    if (isLower || isUpper) {
        // Регистр складывается из двух источников: аккорд (клавиатура сама прислала
        // заглавную) и залипший Shift.
        const bool upper = isUpper || (shiftMode() != SHIFT_OFF);
        const char lower = isUpper ? char(raw + 32) : raw;

        if (layout() == LAYOUT_CYRILLIC) {
            char utf8[3];
            bool replacePrev = false;
            // Сперва перебор: у его клавиш своя цепочка букв. Мимо цепочек — обычная
            // раскладка по месту клавиши.
            size_t n = multiTap(lower, upper, now, utf8, replacePrev);
            if (!n) n = cyrillicUtf8(upper ? char(lower - 32) : lower, utf8);
            if (n == 2) {
                if (replacePrev) {
                    // Перебор меняет только что введённую букву: наружу уходит
                    // стирание, новая буква ждёт в очереди.
                    qPush(uint8_t(utf8[0]));
                    qPush(uint8_t(utf8[1]));
                    e.type = EV_BACKSPACE;
                    return e;
                }
                shiftConsume();                 // новая буква расходует одноразовый Shift
                e.type = EV_CHAR;
                e.ch = utf8[0];
                qPush(uint8_t(utf8[1]));
                return e;
            }
        }

        multiTapReset();
        if (!isUpper && upper) shiftConsume();  // регистр поднят залипшим Shift
        e.type = EV_CHAR;
        e.ch = upper ? char(lower - 32) : lower;
        return e;
    }

    multiTapReset();
    e.type = EV_CHAR;
    e.ch = raw;
    return e;
}

}  // namespace keyboard

// ── трекбол ────────────────────────────────────────────────────────────────────────────

namespace trackball {

namespace {
volatile int8_t g_dx = 0, g_dy = 0;
volatile bool   g_click = false;

// Прерывания считают импульсы. Внутри обработчика ничего тяжелее инкремента делать
// нельзя: трекбол при быстром движении даёт их сотнями в секунду.
void IRAM_ATTR onUp()    { --g_dy; }
void IRAM_ATTR onDown()  { ++g_dy; }
void IRAM_ATTR onLeft()  { --g_dx; }
void IRAM_ATTR onRight() { ++g_dx; }
void IRAM_ATTR onClick() { g_click = true; }
}  // namespace

void begin() {
    const int pins[5] = {BOARD_TRACKBALL_UP, BOARD_TRACKBALL_DOWN,
                         BOARD_TRACKBALL_LEFT, BOARD_TRACKBALL_RIGHT,
                         BOARD_TRACKBALL_CLICK};
    for (int p : pins) pinMode(p, INPUT_PULLUP);
    attachInterrupt(BOARD_TRACKBALL_UP,    onUp,    FALLING);
    attachInterrupt(BOARD_TRACKBALL_DOWN,  onDown,  FALLING);
    attachInterrupt(BOARD_TRACKBALL_LEFT,  onLeft,  FALLING);
    attachInterrupt(BOARD_TRACKBALL_RIGHT, onRight, FALLING);
    attachInterrupt(BOARD_TRACKBALL_CLICK, onClick, FALLING);
}

Event poll() {
    Event e{};
    e.type = EV_NONE;

    if (g_click) { g_click = false; e.type = EV_SELECT; return e; }

    // Прореживаем: без этого одно движение пальца прокручивает список до конца.
    if (g_dy <= -int8_t(kStepPulses)) { g_dy = 0; e.type = EV_UP;    return e; }
    if (g_dy >=  int8_t(kStepPulses)) { g_dy = 0; e.type = EV_DOWN;  return e; }
    if (g_dx <= -int8_t(kStepPulses)) { g_dx = 0; e.type = EV_LEFT;  return e; }
    if (g_dx >=  int8_t(kStepPulses)) { g_dx = 0; e.type = EV_RIGHT; return e; }
    return e;
}

}  // namespace trackball

// ── сенсорный экран GT911 ──────────────────────────────────────────────────────────────

namespace touch {

namespace {
GestureState g_gesture;
bool g_ready = false;
}  // namespace

bool begin() {
    // Сенсор поднимается вместе с экраном: он настроен в display.h и управляется тем же
    // объектом. Свой опрос регистров, который я писал раньше, отсюда убран — он не
    // заработал, а готовый драйвер знает и адреса, и порядок пробуждения микросхемы.
    g_ready = vualScreen().touch() != nullptr;
    Serial.println(g_ready ? "сенсор готов" : "сенсор не найден");
    return g_ready;
}

Event poll() {
    Event e{};
    e.type = EV_NONE;
    if (!g_ready) return e;

    // Координаты последнего КАСАНИЯ. Держим их отдельно, потому что при отпускании
    // библиотека оставляет x и y нулями — она сообщает «пальца нет», а не «где он был».
    //
    // Без этого разбор жеста на отпускании видел рывок из точки касания в левый верхний
    // угол и объявлял его смахиванием влево. Каждое нажатие превращалось в «назад», и
    // ни одна кнопка не срабатывала — а смахивание влево работало «само собой».
    static int16_t lastX = 0, lastY = 0;

    int32_t x = 0, y = 0;
    const bool pressed = vualScreen().getTouch(&x, &y);
    if (pressed) { lastX = int16_t(x); lastY = int16_t(y); }

    return feedGesture(g_gesture, pressed, lastX, lastY, millis());
}

}  // namespace touch

}  // namespace input
