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

bool begin() {
    i2s_config_t cfg = {};
    cfg.mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX);
    cfg.sample_rate = kSampleRate;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;   // моно: речь, стерео ни к чему
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 4;
    cfg.dma_buf_len = kFrameSamples;
    cfg.use_apll = false;

    i2s_pin_config_t pins = {};
    pins.bck_io_num = BOARD_I2S_BCK;
    pins.ws_io_num = BOARD_I2S_WS;
    pins.data_out_num = BOARD_I2S_DOUT;
    pins.data_in_num = BOARD_I2S_DIN;

    if (i2s_driver_install(kPort, &cfg, 0, nullptr) != ESP_OK) return false;
    if (i2s_set_pin(kPort, &pins) != ESP_OK) return false;

    // Гасим шину сразу после настройки и держим остановленной, пока звук не нужен.
    //
    // Без этого динамик всё время получает содержимое неочищенных буферов — то есть
    // мусор, и плата непрерывно шипит и трещит. Заметнее всего это при работе с
    // трекболом: прерывания идут часто, и шум становится ритмичным, будто озвучка
    // действий. Никакой озвучки нет — это именно неубранная шина.
    i2s_zero_dma_buffer(kPort);
    i2s_stop(kPort);

    g_ready = true;
    return true;
}

bool startRecording(const char* path) {
    if (!g_ready || g_rec) return false;
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
    i2s_start(kPort);              // включаем шину только на время записи
    g_rec = true;
    return true;
}

void pumpRecording() {
    if (!g_rec) return;
    size_t got = 0;
    // Не ждём: если данных ещё нет, выходим и вернёмся следующим кругом. Ожидание здесь
    // остановило бы весь интерфейс — а он должен рисовать полоску записи.
    if (i2s_read(kPort, g_pcm, sizeof(g_pcm), &got, 0) != ESP_OK || got == 0) return;

    // Прореживание вдвое усреднением пар: шина даёт 16 кГц, в файл идёт 8. Усреднение,
    // а не выбрасывание каждого второго: выброшенные отсчёты возвращаются свистом на
    // высоких — это зовётся наложением, и лечится оно именно усреднением.
    const size_t n = got / sizeof(int16_t);
    const size_t half = n / 2;
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
    i2s_zero_dma_buffer(kPort);
    i2s_stop(kPort);               // запись кончилась — шину гасим

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
    // Половина буфера: развёрнутый и раздвоенный звук должен помещаться в тот же g_pcm.
    const int read = g_file.read(g_packed, sizeof(g_packed) / 2);
    if (read <= 0) { stopPlayback(); return; }

    const size_t n = adpcmDecode(g_decState, g_packed, size_t(read), g_pcm);
    size_t written = 0;
    if (g_playRate <= kVoiceRate) {
        // Файл на 8 кГц, шина на 16: каждый отсчёт дважды, с усилением.
        for (size_t i = n; i-- > 0; ) {
            const int16_t v = amplify(g_pcm[i]);
            g_pcm[i * 2] = v;
            g_pcm[i * 2 + 1] = v;
        }
        i2s_write(kPort, g_pcm, n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
    } else {
        for (size_t i = 0; i < n; ++i) g_pcm[i] = amplify(g_pcm[i]);
        i2s_write(kPort, g_pcm, n * sizeof(int16_t), &written, portMAX_DELAY);
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
            const int n = (total - made) < int(kChunk) ? (total - made) : int(kChunk);
            for (int i = 0; i < n; ++i) {
                const float t = float(made + i);
                const float env = 1.0f - t / float(total);
                const float v = sinf(6.2831853f * nt.a * t / kSampleRate) +
                                0.6f * sinf(6.2831853f * nt.b * t / kSampleRate);
                g_pcm[i] = int16_t(6500.0f * env * v);
            }
            size_t written = 0;
            i2s_write(kPort, g_pcm, size_t(n) * sizeof(int16_t), &written, portMAX_DELAY);
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
            const int n = (total - made) < int(kChunk) ? (total - made) : int(kChunk);
            for (int i = 0; i < n; ++i) {
                const float t = float(made + i);
                // Плавный спад к концу ноты, чтобы не щёлкало на границе.
                const float env = 1.0f - t / float(total);
                g_pcm[i] = int16_t(9000.0f * env *
                                   sinf(6.2831853f * nt.hz * t / kSampleRate));
            }
            size_t written = 0;
            i2s_write(kPort, g_pcm, size_t(n) * sizeof(int16_t), &written, portMAX_DELAY);
            made += n;
        }
    }
    i2s_zero_dma_buffer(kPort);
    i2s_stop(kPort);
}

}  // namespace device
}  // namespace audio
