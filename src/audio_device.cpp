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

#include "board.h"

namespace audio {
namespace device {

namespace {

constexpr i2s_port_t kPort = I2S_NUM_0;
constexpr size_t kChunk = kFrameSamples * 4;   // отсчётов за один заход

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
    h.sampleRate = uint16_t(kSampleRate);
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

    const size_t n = got / sizeof(int16_t);
    const size_t bytes = adpcmEncode(g_encState, g_pcm, n, g_packed);
    g_file.write(g_packed, bytes);
    g_samples += n;
}

uint32_t stopRecording() {
    if (!g_rec) return 0;
    g_rec = false;

    // Дописываем настоящее число отсчётов в заголовок.
    i2s_zero_dma_buffer(kPort);
    i2s_stop(kPort);               // запись кончилась — шину гасим

    const uint32_t ms = uint32_t(uint64_t(g_samples) * 1000 / kSampleRate);
    g_file.seek(offsetof(VoiceHeader, samples));
    g_file.write(reinterpret_cast<const uint8_t*>(&g_samples), sizeof(g_samples));
    g_file.close();
    return ms;
}

bool isRecording() { return g_rec; }

bool play(const char* path) {
    if (!g_ready || g_play) return false;
    g_file = SD.open(path, FILE_READ);
    if (!g_file) return false;
    g_file.seek(sizeof(VoiceHeader));
    g_decState = AdpcmState{};
    i2s_start(kPort);
    g_play = true;
    return true;
}

void pumpPlayback() {
    if (!g_play) return;
    const int read = g_file.read(g_packed, sizeof(g_packed));
    if (read <= 0) { stopPlayback(); return; }

    const size_t n = adpcmDecode(g_decState, g_packed, size_t(read), g_pcm);
    size_t written = 0;
    i2s_write(kPort, g_pcm, n * sizeof(int16_t), &written, portMAX_DELAY);
}

void stopPlayback() {
    if (!g_play) return;
    g_play = false;
    g_file.close();
    i2s_zero_dma_buffer(kPort);
}

bool isPlaying() { return g_play; }

}  // namespace device
}  // namespace audio
