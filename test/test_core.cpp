// Проверка переносимой логики на обычном компьютере: формат кадра, разнесение,
// восстановление потерь, отсев повторов. Всё это не зависит от железа, а значит
// может и должно быть проверено до заливки в плату.
#include "voile_frame.h"
#include "voile_diversity.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

static int fails = 0;
static void check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "  ok " : "СБОЙ", what);
    if (!ok) ++fails;
}

using namespace voile;

static void testHeader() {
    Header h{};
    h.type = FT_MSG_KEY;
    memcpy(h.dst, "\xAA\xBB\xCC\xDD", 4);
    memcpy(h.src, "\x11\x22\x33\x44", 4);
    h.seq  = 0xBEEF;
    h.part = packPart(2, 5);
    h.copy = packPart(1, 3);

    uint8_t buf[64] = {};
    const size_t n = writeHeader(buf, h);
    check(n == kHdrLen, "заголовок занимает 13 байт");

    Header r{};
    check(readHeader(buf, n, r), "заголовок разбирается");
    check(r.type == h.type && r.seq == h.seq, "тип и номер сохранились");
    check(memcmp(r.dst, h.dst, 4) == 0 && memcmp(r.src, h.src, 4) == 0, "адреса сохранились");
    check(partIndex(r.part) == 2 && partTotal(r.part) == 5, "номер части распаковался");
    check(partIndex(r.copy) == 1 && partTotal(r.copy) == 3, "номер копии распаковался");

    check(!readHeader(buf, kHdrLen - 1, r), "короткий буфер отвергается");

    // Порядок байт не должен зависеть от процессора.
    check(buf[9] == 0xBE && buf[10] == 0xEF, "номер записан старшим байтом вперёд");
}

static void testCapacity() {
    check(payloadCapacity(false) == 255 - 13 - 8, "обычный кадр: 234 байта под данные");
    check(payloadCapacity(true)  == 255 - 13 - 33 - 8, "кадр с ключом: 201 байт");
    printf("       обычный %zu Б, с ключом %zu Б\n",
           payloadCapacity(false), payloadCapacity(true));
}

static void testDelays() {
    check(copyDelayMs(0, 1) == 0, "первая копия уходит без задержки");
    const uint32_t d1 = copyDelayMs(1, 7);
    const uint32_t d2 = copyDelayMs(2, 7);
    check(d1 >= 2500 && d1 <= 5500, "вторая копия через ~4 с");
    check(d2 > d1, "промежутки растут");
    // Разброс обязан отличаться у разных узлов, иначе они столкнутся в эфире.
    check(copyDelayMs(1, 7) != copyDelayMs(1, 99), "разброс зависит от узла");
}

static void testFreqOffsets() {
    // Смещений больше нет: приёмник слушает одну частоту, и копия на любой другой
    // ушла бы в пустоту. Все копии обязаны идти на основной частоте.
    for (uint8_t i = 0; i < 8; ++i)
        check(copyFreqOffsetKhz(i) == 0, "все копии на основной частоте");
}

static void testFec() {
    const size_t L = 32;
    uint8_t b[4][L];
    for (int i = 0; i < 4; ++i)
        for (size_t j = 0; j < L; ++j) b[i][j] = uint8_t(i * 40 + j);

    const uint8_t* ptrs[4] = {b[0], b[1], b[2], b[3]};
    uint8_t parity[L];
    fecParity(ptrs, 4, L, parity);

    // Теряем блок 2 и восстанавливаем.
    const uint8_t* lost[4] = {b[0], b[1], nullptr, b[3]};
    uint8_t out[L] = {};
    check(fecRecover(lost, 4, L, parity, 2, out), "восстановление отработало");
    check(memcmp(out, b[2], L) == 0, "восстановленный блок совпал с исходным");

    // Два потерянных — восстановить нечем, и это должно честно сообщаться.
    const uint8_t* lost2[4] = {b[0], nullptr, nullptr, b[3]};
    check(!fecRecover(lost2, 4, L, parity, 1, out), "две потери отвергаются");
}

static void testSeen() {
    SeenCache c;
    const uint8_t a[4] = {1, 2, 3, 4};
    const uint8_t z[4] = {9, 9, 9, 9};
    check(!c.seen(a, 100), "первое появление — не повтор");
    check(c.seen(a, 100), "вторая копия распознана как повтор");
    check(!c.seen(a, 101), "другой номер — новое сообщение");
    check(!c.seen(z, 100), "другой отправитель — новое сообщение");

    // Кольцо не должно переполняться и падать.
    for (uint16_t i = 0; i < 1000; ++i) c.seen(a, uint16_t(i + 500));
    check(true, "кольцевой буфер выдержал 1000 записей");
}

int main() {
    printf("── формат кадра ──\n");        testHeader();
    printf("── ёмкость ──\n");             testCapacity();
    printf("── разнесение во времени ──\n"); testDelays();
    printf("── разнесение по частоте ──\n"); testFreqOffsets();
    printf("── восстановление потерь ──\n"); testFec();
    printf("── отсев повторов ──\n");      testSeen();
    printf("\n%s\n", fails ? "ЕСТЬ СБОИ" : "всё сошлось");
    return fails ? 1 : 0;
}
