// Запись и воспроизведение голосовых на плате.
//
// Звук пишется на карту СРАЗУ СЖАТЫМ: минута речи в сыром виде — почти два мегабайта, а
// свободной памяти на плате куда меньше. Сжатие вчетверо и запись порциями позволяют
// держать в памяти лишь небольшой буфер независимо от длины записи.
#include "audio.h"

#include <Arduino.h>
#include <stddef.h>
#include <string.h>
#include <SD.h>
#include <driver/i2s.h>
#include <Wire.h>
#include <math.h>

#include "board.h"

namespace audio {
namespace device {

namespace {

constexpr i2s_port_t kPort = I2S_NUM_0;
constexpr size_t kChunk = kFrameSamples * 4;   // отсчётов за один заход

/** Частота голосовых СООБЩЕНИЙ — вдвое ниже частоты шины. Для радио каждый байт на
 *  счету: 8 кГц режет поток пополам, а разборчивость речи почти не страдает — телефонные
 *  линии десятилетиями жили на этой частоте. Шина остаётся на 16 кГц ради рации,
 *  совместимой с телефоном: сообщения прореживаются при записи и раздваиваются при
 *  проигрывании. */
constexpr int kVoiceRate = 8000;

/** Усиление на проигрывании. Динамик платы тихий, а записи с микрофона ещё и негромкие;
 *  умножение с насыщением — самый дешёвый способ сделать речь слышимой. */
inline int16_t amplify(int32_t v) {
    v *= 3;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return int16_t(v);
}

bool      g_ready = false;
bool      g_rec = false;
bool      g_play = false;
File      g_file;
AdpcmState g_encState;
AdpcmState g_decState;
uint32_t  g_samples = 0;
int16_t   g_pcm[kChunk];
uint8_t   g_packed[kChunk / 2];

}  // namespace

/** Порт микрофона — отдельный от динамика: у ES7210 свои ножки. */
constexpr i2s_port_t kMicPort = I2S_NUM_1;
bool g_micReady = false;

/** Записать регистр кодека ES7210 по I2C. */
bool es7210Write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(BOARD_ES7210_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

/**
 * Поднять кодек микрофона ES7210.
 *
 * Это отдельная микросхема со своим набором регистров: без настройки она молчит, что бы
 * ни делал контроллер. Последовательность — минимально необходимая: сброс, тактирование
 * в ведомом режиме (такты даём мы), формат 16 бит, питание аналоговой части, включение
 * и усиление первых двух микрофонов.
 */
bool es7210Init() {
    // Кодек вообще на шине? Молчание здесь — сразу честный отказ с маячком.
    Wire.beginTransmission(BOARD_ES7210_ADDR);
    if (Wire.endTransmission() != 0) {
        ets_printf("[vual] микрофон: кодек ES7210 не отвечает по I2C\n");
        return false;
    }
    bool ok = true;
    ok &= es7210Write(0x00, 0xFF);   // полный сброс
    delay(10);
    ok &= es7210Write(0x00, 0x41);   // рабочее состояние
    ok &= es7210Write(0x01, 0x1F);   // все внутренние такты включены
    ok &= es7210Write(0x08, 0x10);   // ведомый режим: такты приходят снаружи
    ok &= es7210Write(0x11, 0x60);   // формат данных: I2S, 16 бит
    ok &= es7210Write(0x12, 0x00);   // обычный порядок каналов
    ok &= es7210Write(0x40, 0x42);   // питание аналоговой части
    ok &= es7210Write(0x41, 0x70);   // опорные цепи
    ok &= es7210Write(0x42, 0x70);
    ok &= es7210Write(0x43, 0x1B);   // микрофон 1: включён, усиление среднее
    ok &= es7210Write(0x44, 0x1B);   // микрофон 2: так же
    ok &= es7210Write(0x47, 0x08);   // смещение микрофонов
    ok &= es7210Write(0x48, 0x08);
    ok &= es7210Write(0x4B, 0x00);   // микрофоны 1-2 запитаны
    ok &= es7210Write(0x4C, 0xFF);   // микрофоны 3-4 выключены — их нет на плате
    ets_printf("[vual] микрофон: настройка %s\n", ok ? "прошла" : "СБИЛАСЬ");
    return ok;
}

bool begin() {
    // ── динамик: только передача, свои ножки ───────────────────────────────────────────
    {
        i2s_config_t cfg = {};
        cfg.mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_TX);
        cfg.sample_rate = kSampleRate;
        cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
        // Стерео: отсчёт кладётся в оба слота, и раскладка слотов усилителя не наша забота.
        cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
        cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
        cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
        cfg.dma_buf_count = 4;
        cfg.dma_buf_len = kFrameSamples;
        cfg.use_apll = false;

        i2s_pin_config_t pins = {};
        pins.bck_io_num = BOARD_SPK_BCK;
        pins.ws_io_num = BOARD_SPK_WS;
        pins.data_out_num = BOARD_SPK_DOUT;
        pins.data_in_num = I2S_PIN_NO_CHANGE;

        if (i2s_driver_install(kPort, &cfg, 0, nullptr) != ESP_OK) return false;
        if (i2s_set_pin(kPort, &pins) != ESP_OK) return false;

        // Гасим шину сразу после настройки и держим остановленной, пока звук не нужен:
        // иначе динамик непрерывно получает мусор из неочищенных буферов и шипит.
        i2s_zero_dma_buffer(kPort);
        i2s_stop(kPort);
    }

    // ── микрофон: отдельный порт приёма + настройка кодека ────────────────────────────
    //
    // Неудача микрофона НЕ валит звук целиком: динамик важнее — без него не слышно
    // ни мелодии, ни принятых голосовых.
    do {
        if (!es7210Init()) break;

        i2s_config_t cfg = {};
        cfg.mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_RX);
        cfg.sample_rate = kSampleRate;
        cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
        cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
        cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
        cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
        cfg.dma_buf_count = 4;
        cfg.dma_buf_len = kFrameSamples;
        cfg.use_apll = false;
        cfg.fixed_mclk = kSampleRate * 256;   // опорная частота кодеку

        i2s_pin_config_t pins = {};
        pins.mck_io_num = BOARD_MIC_MCLK;
        pins.bck_io_num = BOARD_MIC_SCK;
        pins.ws_io_num = BOARD_MIC_WS;
        pins.data_out_num = I2S_PIN_NO_CHANGE;
        pins.data_in_num = BOARD_MIC_DIN;

        if (i2s_driver_install(kMicPort, &cfg, 0, nullptr) != ESP_OK) break;
        if (i2s_set_pin(kMicPort, &pins) != ESP_OK) break;
        i2s_stop(kMicPort);
        g_micReady = true;
    } while (false);
    if (!g_micReady) ets_printf("[vual] микрофон: НЕ поднялся — запись недоступна\n");

    g_ready = true;
    return true;
}

bool startRecording(const char* path) {
    if (!g_ready || !g_micReady || g_rec) return false;
    g_file = SD.open(path, FILE_WRITE);
    if (!g_file) return false;

    // Заголовок: по нему принимающая сторона узнаёт формат и длительность. Число
    // отсчётов пока неизвестно — допишем в конце, вернувшись к началу файла.
    VoiceHeader h{};
    memcpy(h.magic, "VUAV", 4);
    h.codec = 1;                       // сжатый
    h.sampleRate = uint16_t(kVoiceRate);
    h.samples = 0;
    g_file.write(reinterpret_cast<uint8_t*>(&h), sizeof(h));

    g_encState = AdpcmState{};
    g_samples = 0;
    i2s_start(kMicPort);           // включаем микрофонную шину только на время записи
    g_rec = true;
    return true;
}

void pumpRecording() {
    if (!g_rec) return;
    size_t got = 0;
    // Не ждём: если данных ещё нет, выходим и вернёмся следующим кругом. Ожидание здесь
    // остановило бы весь интерфейс — а он должен рисовать полоску записи.
    if (i2s_read(kMicPort, g_pcm, sizeof(g_pcm), &got, 0) != ESP_OK || got == 0) return;

    // Из стерео-кадров в моно: голос лежит в одном из слотов (в каком — зависит от
    // платы), сумма обоих ловит его в любом случае, а пустой слот добавляет ноль.
    const size_t frames = got / (2 * sizeof(int16_t));
    for (size_t i = 0; i < frames; ++i)
        g_pcm[i] = int16_t((int32_t(g_pcm[i * 2]) + g_pcm[i * 2 + 1]) / 2);

    // Прореживание вдвое усреднением пар: шина даёт 16 кГц, в файл идёт 8. Усреднение,
    // а не выбрасывание каждого второго: выброшенные отсчёты возвращаются свистом на
    // высоких — это зовётся наложением, и лечится оно именно усреднением.
    const size_t half = frames / 2;
    for (size_t i = 0; i < half; ++i)
        g_pcm[i] = int16_t((int32_t(g_pcm[i * 2]) + g_pcm[i * 2 + 1]) / 2);
    const size_t bytes = adpcmEncode(g_encState, g_pcm, half, g_packed);
    g_file.write(g_packed, bytes);
    g_samples += half;
}

uint32_t stopRecording() {
    if (!g_rec) return 0;
    g_rec = false;

    // Дописываем настоящее число отсчётов в заголовок.
    i2s_zero_dma_buffer(kMicPort);
    i2s_stop(kMicPort);            // запись кончилась — микрофонную шину гасим

    const uint32_t ms = uint32_t(uint64_t(g_samples) * 1000 / kVoiceRate);
    g_file.seek(offsetof(VoiceHeader, samples));
    g_file.write(reinterpret_cast<const uint8_t*>(&g_samples), sizeof(g_samples));
    g_file.close();
    return ms;
}

bool isRecording() { return g_rec; }

uint16_t g_playRate = kVoiceRate;

bool play(const char* path) {
    if (!g_ready || g_play) return false;
    g_file = SD.open(path, FILE_READ);
    if (!g_file) return false;
    // Частоту берём из заголовка: старые записи сделаны на 16 кГц, новые — на 8, и обе
    // должны звучать правильно, а не вдвое быстрее или медленнее.
    VoiceHeader h{};
    if (g_file.read(reinterpret_cast<uint8_t*>(&h), sizeof(h)) == sizeof(h) &&
        memcmp(h.magic, "VUAV", 4) == 0 && h.sampleRate > 0) {
        g_playRate = h.sampleRate;
    } else {
        g_playRate = kVoiceRate;
        g_file.seek(sizeof(VoiceHeader));
    }
    g_decState = AdpcmState{};
    i2s_start(kPort);
    g_play = true;
    return true;
}

void pumpPlayback() {
    if (!g_play) return;
    // Четверть буфера сжатого: после развёртки каждый отсчёт займёт ЧЕТЫРЕ места —
    // дважды по времени (8 кГц файла против 16 кГц шины) и в оба слота стерео-кадра.
    const int read = g_file.read(g_packed, sizeof(g_packed) / 4);
    if (read <= 0) { stopPlayback(); return; }

    const size_t n = adpcmDecode(g_decState, g_packed, size_t(read), g_pcm);
    size_t written = 0;
    if (g_playRate <= kVoiceRate) {
        for (size_t i = n; i-- > 0; ) {
            const int16_t v = amplify(g_pcm[i]);
            g_pcm[i * 4]     = v;
            g_pcm[i * 4 + 1] = v;
            g_pcm[i * 4 + 2] = v;
            g_pcm[i * 4 + 3] = v;
        }
        i2s_write(kPort, g_pcm, n * 4 * sizeof(int16_t), &written, portMAX_DELAY);
    } else {
        // Старый файл на 16 кГц: только раздвоение по слотам.
        for (size_t i = n; i-- > 0; ) {
            const int16_t v = amplify(g_pcm[i]);
            g_pcm[i * 2]     = v;
            g_pcm[i * 2 + 1] = v;
        }
        i2s_write(kPort, g_pcm, n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
    }
}

void stopPlayback() {
    if (!g_play) return;
    g_play = false;
    g_file.close();
    i2s_zero_dma_buffer(kPort);
}

bool isPlaying() { return g_play; }

void bootMelody() {
    // Проверка динамика на слух при каждом запуске: если мелодии нет — путь звука
    // мёртв на уровне железа, и искать причину тишины в проигрывании голосовых
    // бессмысленно. Двухголосие: нота и терция над ней, три ступени вверх.
    if (!g_ready) return;
    i2s_start(kPort);
    const struct { float a, b; int ms; } notes[3] = {
        {523.25f, 659.25f, 140},   // до-ми
        {659.25f, 783.99f, 140},   // ми-соль
        {783.99f, 987.77f, 220},   // соль-си
    };
    for (const auto& nt : notes) {
        const int total = kSampleRate * nt.ms / 1000;
        int made = 0;
        while (made < total) {
            // Пары на каждый отсчёт: буфер вмещает вдвое меньше моно-отсчётов.
            const int cap = int(kChunk) / 2;
            const int n = (total - made) < cap ? (total - made) : cap;
            for (int i = 0; i < n; ++i) {
                const float t = float(made + i);
                const float env = 1.0f - t / float(total);
                const float v = sinf(6.2831853f * nt.a * t / kSampleRate) +
                                0.6f * sinf(6.2831853f * nt.b * t / kSampleRate);
                const int16_t sv = int16_t(6500.0f * env * v);
                g_pcm[i * 2]     = sv;
                g_pcm[i * 2 + 1] = sv;
            }
            size_t written = 0;
            i2s_write(kPort, g_pcm, size_t(n) * 2 * sizeof(int16_t), &written,
                      portMAX_DELAY);
            made += n;
        }
    }
    i2s_zero_dma_buffer(kPort);
    i2s_stop(kPort);
}

void chime() {
    // Сигнал входящего: два коротких восходящих тона, как принято у мессенджеров.
    // Поверх записи или проигрывания не лезем — там шина занята делом.
    if (!g_ready || g_rec || g_play) return;
    i2s_start(kPort);
    const struct { float hz; int ms; } notes[2] = {{880.0f, 90}, {1174.7f, 130}};
    for (const auto& nt : notes) {
        const int total = kSampleRate * nt.ms / 1000;
        int made = 0;
        while (made < total) {
            const int cap = int(kChunk) / 2;      // пары: моно-отсчётов вдвое меньше
            const int n = (total - made) < cap ? (total - made) : cap;
            for (int i = 0; i < n; ++i) {
                const float t = float(made + i);
                // Плавный спад к концу ноты, чтобы не щёлкало на границе.
                const float env = 1.0f - t / float(total);
                const int16_t sv = int16_t(9000.0f * env *
                                   sinf(6.2831853f * nt.hz * t / kSampleRate));
                g_pcm[i * 2]     = sv;
                g_pcm[i * 2 + 1] = sv;
            }
            size_t written = 0;
            i2s_write(kPort, g_pcm, size_t(n) * 2 * sizeof(int16_t), &written,
                      portMAX_DELAY);
            made += n;
        }
    }
    i2s_zero_dma_buffer(kPort);
    i2s_stop(kPort);
}

}  // namespace device
}  // namespace audio
