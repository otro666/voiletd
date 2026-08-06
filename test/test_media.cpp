// Проверка переносимой логики новых модулей: сжатие звука, выбор транспорта,
// журнал на карте. Всё это не зависит от железа и должно работать до заливки.
#include "audio.h"
#include "transport.h"
#include "store.h"
#include "media.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

static int fails = 0;
static void check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "  ok " : "СБОЙ", what);
    if (!ok) ++fails;
}

// ── звук ───────────────────────────────────────────────────────────────────────────────

static void testAudio() {
    using namespace audio;

    // Синтезируем речеподобный сигнал: сумма тонов в полосе голоса.
    const size_t N = 1600;                       // 100 мс при 16 кГц
    std::vector<int16_t> src(N);
    for (size_t i = 0; i < N; ++i) {
        const double t = double(i) / kSampleRate;
        src[i] = int16_t(8000 * std::sin(2*M_PI*220*t) + 3000 * std::sin(2*M_PI*700*t));
    }

    std::vector<uint8_t> packed(N / 2 + 8);
    AdpcmState enc{};
    const size_t bytes = adpcmEncode(enc, src.data(), N, packed.data());
    check(bytes == N / 2, "сжатие ровно вчетверо: 16 бит -> 4 бита");
    printf("       %zu отсчётов -> %zu байт (было %zu)\n", N, bytes, N * 2);

    std::vector<int16_t> back(N + 8);
    AdpcmState dec{};
    const size_t got = adpcmDecode(dec, packed.data(), bytes, back.data());
    check(got == N, "развернулось столько же отсчётов");

    // Считаем отношение сигнала к шуму: для речи приемлемо от 20 дБ.
    double se = 0, ne = 0;
    for (size_t i = 0; i < N; ++i) {
        const double s = src[i], d = double(back[i]) - s;
        se += s * s; ne += d * d;
    }
    const double snr = 10 * std::log10(se / (ne > 0 ? ne : 1e-9));
    printf("       отношение сигнал/шум после сжатия: %.1f дБ\n", snr);
    check(snr > 20.0, "качество достаточно для разборчивой речи");

    // Заголовок WAV — им телефон и любой проигрыватель понимают наш звук.
    uint8_t wav[64];
    const size_t hl = writeWavHeader(wav, 32000, kSampleRate);
    check(hl == 44, "заголовок WAV занимает 44 байта");
    check(memcmp(wav, "RIFF", 4) == 0 && memcmp(wav + 8, "WAVE", 4) == 0,
          "заголовок WAV опознаётся");
    const uint32_t rate = uint32_t(wav[24]) | (uint32_t(wav[25]) << 8) |
                          (uint32_t(wav[26]) << 16) | (uint32_t(wav[27]) << 24);
    check(rate == uint32_t(kSampleRate), "частота записана верно");

    // Рация по радио невозможна физически — это должно быть видно из расчёта, а не
    // выясняться в поле.
    check(pttFitsIn(1000000), "рация помещается в Wi-Fi");
    check(!pttFitsIn(1758),   "рация НЕ помещается в LoRa — и это арифметика, не лень");
}

// ── транспорт ──────────────────────────────────────────────────────────────────────────

static void testTransport() {
    using namespace transport;

    PeerRoute p{};
    p.wifiSeen = true;  p.wifiLastMs = 1000;
    p.loraSeen = true;  p.loraLastMs = 1000;

    check(pickRail(p, 2000, false) == RAIL_WIFI, "при обоих путях выбирается Wi-Fi");
    check(pickRail(p, 2000, true)  == RAIL_WIFI, "файлы идут по Wi-Fi");

    // Wi-Fi устарел — переходим на радио, но только для текста.
    check(pickRail(p, 1000 + kWifiStaleMs + 1, false) == RAIL_LORA,
          "Wi-Fi пропал — текст уходит по радио");
    check(pickRail(p, 1000 + kWifiStaleMs + 1, true) == RAIL_NONE,
          "файл по радио не отправляется — честный отказ вместо часа эфира");

    PeerRoute none{};
    check(pickRail(none, 5000, false) == RAIL_NONE, "неизвестный собеседник — пути нет");

    // Оценка времени: фотография по радио должна быть признана неразумной.
    const auto photo = estimate(RAIL_LORA, 200 * 1024);
    check(!photo.sane, "фотография 200 КБ по радио отвергается");
    printf("       фото по радио: %u частей, ~%u с\n", photo.parts, photo.seconds);

    const auto text = estimate(RAIL_LORA, 200);
    check(text.sane && text.parts == 1, "короткий текст по радио — одна часть");

    const auto photoWifi = estimate(RAIL_WIFI, 200 * 1024);
    check(photoWifi.sane, "та же фотография по Wi-Fi проходит");

    check(capsOf(RAIL_WIFI).ptt,  "рация есть по Wi-Fi");
    check(!capsOf(RAIL_LORA).ptt, "рации по радио нет");
}

// ── хранилище ──────────────────────────────────────────────────────────────────────────

static void testStore() {
    using namespace store;

    const char* body = "привет из радиоэфира";
    const size_t bl = strlen(body);

    RecHeader h{};
    h.len = uint16_t(bl);
    h.crc = crc16(reinterpret_cast<const uint8_t*>(body), bl);
    h.kind = REC_TEXT;
    h.flags = kFlagMine | kFlagDelivered;
    h.ts = 1754300000u;
    h.seq = 42;

    uint8_t buf[256];
    const size_t n = writeRecHeader(buf, h);
    memcpy(buf + n, body, bl);
    check(n == kRecHdrLen, "заголовок записи 12 байт");

    RecHeader r{};
    check(readRecHeader(buf, n, r), "заголовок читается");
    check(r.len == h.len && r.crc == h.crc && r.ts == h.ts && r.seq == h.seq,
          "поля сохранились");
    check((r.flags & kFlagMine) && (r.flags & kFlagDelivered), "признаки сохранились");

    size_t consumed = 0;
    check(recordValid(buf, n + bl, consumed), "целая запись принимается");
    check(consumed == n + bl, "длина записи посчитана верно");

    // Обрыв питания посреди записи: заголовок есть, содержимого нет.
    check(!recordValid(buf, n + bl - 3, consumed), "оборванная запись отвергается");

    // Порча сектора: содержимое изменилось, сумма не сходится.
    buf[n + 2] ^= 0xFF;
    check(!recordValid(buf, n + bl, consumed), "испорченная запись отвергается");
    buf[n + 2] ^= 0xFF;

    check(recordValid(buf, n + bl, consumed), "после восстановления снова принимается");

    // Сумма должна реагировать на любое изменение.
    const uint16_t c1 = crc16(reinterpret_cast<const uint8_t*>("абв"), 6);
    const uint16_t c2 = crc16(reinterpret_cast<const uint8_t*>("абг"), 6);
    check(c1 != c2, "контрольная сумма различает содержимое");
}

// ── картинки ───────────────────────────────────────────────────────────────────────────

static void testMedia() {
    using namespace media;
    const uint8_t jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0};
    check(detect(jpeg, sizeof(jpeg)) == IMG_JPEG, "JPEG опознаётся");
    const uint8_t junk[] = {1, 2, 3, 4};
    check(detect(junk, sizeof(junk)) == IMG_UNSUPPORTED, "чужой формат отвергается");

    // Большой снимок с телефона разбираем сразу уменьшенным: полный кадр не влез бы
    // в разумное время и память.
    check(pickScale(4000, 3000) == 8, "снимок 4000x3000 уменьшается восьмикратно");
    // 640x480 при уменьшении вдвое даёт ровно размер экрана — это допустимо и выгодно.
    check(pickScale(640, 480) == 2, "640x480 уменьшается вдвое — ровно в экран");
    check(pickScale(300, 200) == 1, "картинка мельче экрана не уменьшается");
    printf("       4000x3000 -> разбор в 1/%u\n", pickScale(4000, 3000));

    // Пропорции обязаны сохраняться: сплющенное лицо хуже, чем поля по краям.
    const Fit f = fitCenter(1000, 500);
    check(f.w == 320 && f.h == 160, "широкая картинка вписана по ширине");
    check(f.y == 40, "и отцентрована по высоте");
    const Fit v = fitCenter(500, 1000);
    check(v.h == 240 && v.w == 120, "высокая картинка вписана по высоте");
    check(v.x == 100, "и отцентрована по ширине");

    char path[64];
    thumbPath("a1b2c3", path, sizeof(path));
    check(strcmp(path, "/vual/thumbs/a1b2c3.jpg") == 0, "путь обложки собирается верно");
}

int main() {
    printf("── звук ──\n");      testAudio();
    printf("── транспорт ──\n"); testTransport();
    printf("── хранилище ──\n"); testStore();
    printf("── картинки ──\n");  testMedia();
    printf("\n%s\n", fails ? "ЕСТЬ СБОИ" : "всё сошлось");
    return fails ? 1 : 0;
}
