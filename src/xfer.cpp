#include "xfer.h"

#include <Arduino.h>
#include <SD.h>
#include <string.h>

#include "net.h"
#include "store_sd.h"
#include "voile_frame.h"

namespace xfer {

namespace {

/**
 * Размер куска.
 *
 * По сети берём килобайт: крупнее — и всплеск памяти при обёртке, мельче — лишние
 * накладные расходы на каждый кусок.
 *
 * По радио — то, что влезает в кадр за вычетом заголовка передачи. Больше физически
 * не поместится: предел кадра задан самим радиомодулем.
 */
constexpr size_t kNetChunk   = 1024;
constexpr size_t kRadioChunk = 230;   // впритык к кадру: меньше кадров — быстрее

/** Заголовок куска. Восемь байт на каждый — немного, но по радио заметно, поэтому
 *  ничего лишнего: только то, без чего кусок не собрать. */
struct ChunkHead {
    uint8_t  magic;      // 'X' — отличить от прочего в потоке
    uint8_t  kind;       // вид содержимого
    uint16_t id;         // номер передачи: их может идти несколько
    uint16_t index;      // номер куска
    uint16_t total;      // сколько всего кусков
};
constexpr size_t kHeadLen = 8;

struct Slot {
    bool     active = false;
    bool     outgoing = false;
    Kind     kind = K_FILE;
    Route    route = R_NET;
    char     peer[40] = {};
    char     path[80] = {};
    File     file;
    uint16_t id = 0;
    uint16_t index = 0;
    uint16_t total = 0;
    uint32_t bytes = 0;
    uint32_t done = 0;
    uint32_t lastSent = 0;
};

Slot g_slot[kMaxActive];
OnComplete g_onComplete = nullptr;
uint16_t g_nextId = 1;

uint8_t g_pkt[1200];

Slot* freeSlot() {
    for (auto& s : g_slot) if (!s.active) return &s;
    return nullptr;
}

Slot* findIncoming(const char* peer, uint16_t id) {
    for (auto& s : g_slot) {
        if (s.active && !s.outgoing && s.id == id && strcmp(s.peer, peer) == 0) return &s;
    }
    return nullptr;
}

void writeHead(uint8_t* out, const ChunkHead& h) {
    out[0] = h.magic;
    out[1] = h.kind;
    out[2] = uint8_t(h.id >> 8);    out[3] = uint8_t(h.id);
    out[4] = uint8_t(h.index >> 8); out[5] = uint8_t(h.index);
    out[6] = uint8_t(h.total >> 8); out[7] = uint8_t(h.total);
}

bool readHead(const uint8_t* in, size_t len, ChunkHead& h) {
    if (len < kHeadLen || in[0] != 'X') return false;
    h.magic = in[0];
    h.kind  = in[1];
    h.id    = uint16_t((in[2] << 8) | in[3]);
    h.index = uint16_t((in[4] << 8) | in[5]);
    h.total = uint16_t((in[6] << 8) | in[7]);
    return true;
}

/** Путь для принимаемого файла. Имя из номера передачи: настоящее придёт отдельно, а до
 *  тех пор надо куда-то писать. */
void incomingPath(const char* peer, uint16_t id, Kind kind, char* out, size_t cap) {
    const char* ext = kind == K_VOICE ? "vua" : (kind == K_PHOTO ? "jpg" : "bin");
    snprintf(out, cap, "/vual/media/in_%s_%u.%s", peer, unsigned(id), ext);
}

}  // namespace

uint32_t radioEstimateMs(uint32_t bytes) {
    const uint32_t chunks = (bytes + kRadioChunk - 1) / kRadioChunk;
    // Пять кадров в секунду — осторожная оценка для узкой полосы с разнесением копий.
    // Лучше пообещать больше и уложиться, чем наоборот.
    return chunks * 200;
}

bool send(const char* peer, const char* path, Kind kind, Route route) {
    Slot* s = freeSlot();
    if (!s) { store::log("xfer", "нет свободного места под передачу"); return false; }

    s->file = SD.open(path, FILE_READ);
    if (!s->file) { store::log("xfer", "файл не открылся"); return false; }

    s->bytes = uint32_t(s->file.size());
    if (s->bytes == 0) { s->file.close(); return false; }

    const size_t chunk = route == R_NET ? kNetChunk : kRadioChunk;
    s->total = uint16_t((s->bytes + chunk - 1) / chunk);
    if (s->total == 0) { s->file.close(); return false; }

    s->active = true;
    s->outgoing = true;
    s->kind = kind;
    s->route = route;
    s->id = g_nextId++;
    s->index = 0;
    s->done = 0;
    s->lastSent = 0;
    snprintf(s->peer, sizeof(s->peer), "%s", peer);
    snprintf(s->path, sizeof(s->path), "%s", path);

    char msg[96];
    snprintf(msg, sizeof(msg), "отправка %s: %lu Б, %u кусков, %s",
             path, (unsigned long)s->bytes, unsigned(s->total),
             route == R_NET ? "сеть" : "радио");
    store::log("xfer", msg);
    return true;
}

namespace { RadioSender g_radioSend = nullptr; }
void setRadioSender(RadioSender fn) { g_radioSend = fn; }

void pump() {
    // Заглохший приём бросаем: отправитель давно умолк, а занятый слот не пускал бы
    // следующие передачи. Полминуты тишины при кадре в секунду — уже не пауза.
    for (auto& s : g_slot) {
        if (s.active && !s.outgoing && millis() - s.lastSent > 30000) {
            s.file.close();
            SD.remove(s.path);
            s.active = false;
            store::log("xfer", "приём заглох — брошено");
        }
    }

    for (auto& s : g_slot) {
        if (!s.active || !s.outgoing) continue;

        // По радио шлём размеренно: эфир общий, и забивать его сплошным потоком значит
        // глушить всех вокруг, включая собеседника.
        const uint32_t gap = s.route == R_RADIO ? 200 : 5;
        if (millis() - s.lastSent < gap) continue;
        s.lastSent = millis();

        const size_t chunk = s.route == R_NET ? kNetChunk : kRadioChunk;
        ChunkHead h{'X', uint8_t(s.kind), s.id, s.index, s.total};
        writeHead(g_pkt, h);

        s.file.seek(uint32_t(s.index) * chunk);
        const int n = s.file.read(g_pkt + kHeadLen, chunk);
        if (n <= 0) {
            s.file.close();
            s.active = false;
            store::log("xfer", "чтение оборвалось — передача прекращена");
            continue;
        }

        bool ok = false;
        if (s.route == R_NET) {
            // По имени узла: собеседник уже найден, путь известен.
            for (size_t i = 0; i < net::neighbourCount(); ++i) {
                const net::Neighbour* nb = net::neighbourAt(i);
                if (nb) ok = net::sendTo(nb->id, g_pkt, size_t(n) + kHeadLen) || ok;
            }
        } else {
            // По радио — через зарегистрированный отправитель. Раньше здесь стояло
            // голое «ok = true» со ссылкой на код в main, которого не существовало:
            // передача «завершалась», не выйдя в эфир ни единым кадром.
            ok = g_radioSend && g_radioSend(s.peer, g_pkt, size_t(n) + kHeadLen);
        }

        // Не ушло — НЕ продвигаемся: тот же кусок пойдёт следующим кругом. Молча
        // пропустить кусок значит отдать получателю дырявый файл.
        if (!ok) continue;

        s.done += uint32_t(n);
        ++s.index;

        if (s.index >= s.total) {
            s.file.close();
            s.active = false;
            store::log("xfer", "передача завершена");
            if (g_onComplete) g_onComplete(s.peer, s.path, s.kind, false);
        }
    }
}

void onChunk(const char* peer, const uint8_t* data, size_t len, Route route) {
    ChunkHead h{};
    if (!readHead(data, len, h)) return;

    Slot* s = findIncoming(peer, h.id);
    if (!s) {
        // Новый кусок — заводим приём. Если это не первый кусок, начало потеряно, и
        // собирать нечего: просим прислать заново, а не пишем дырявый файл.
        if (h.index != 0) return;
        s = freeSlot();
        if (!s) return;

        s->active = true;
        s->outgoing = false;
        s->kind = Kind(h.kind);
        s->route = route;
        s->id = h.id;
        s->index = 0;
        s->total = h.total;
        s->done = 0;
        snprintf(s->peer, sizeof(s->peer), "%s", peer);
        incomingPath(peer, h.id, s->kind, s->path, sizeof(s->path));
        SD.remove(s->path);
        s->file = SD.open(s->path, FILE_WRITE);
        if (!s->file) { s->active = false; return; }
        s->bytes = 0;
        s->lastSent = millis();       // для приёма это «последний раз слышали»
    }

    // Кусок не по порядку — пропускаем. Досылка придёт следующим кругом; писать его не
    // на своё место значило бы получить перемешанный файл, который не откроется.
    if (h.index != s->index) return;

    s->lastSent = millis();
    const size_t payload = len - kHeadLen;
    s->file.write(data + kHeadLen, payload);
    s->done += uint32_t(payload);
    ++s->index;

    if (s->index >= s->total) {
        s->file.close();
        s->active = false;
        char msg[96];
        snprintf(msg, sizeof(msg), "принято %s: %lu Б", s->path, (unsigned long)s->done);
        store::log("xfer", msg);
        if (g_onComplete) g_onComplete(s->peer, s->path, s->kind, true);
    }
}

size_t activeCount() {
    size_t n = 0;
    for (const auto& s : g_slot) if (s.active) ++n;
    return n;
}

const Progress* activeAt(size_t i) {
    static Progress p;
    size_t seen = 0;
    for (const auto& s : g_slot) {
        if (!s.active) continue;
        if (seen++ != i) continue;
        p.active = true;
        p.kind = s.kind;
        p.route = s.route;
        p.outgoing = s.outgoing;
        p.total = s.bytes ? s.bytes : uint32_t(s.total) * 180;
        p.done = s.done;
        snprintf(p.name, sizeof(p.name), "%s", s.path);
        return &p;
    }
    return nullptr;
}

void setOnComplete(OnComplete cb) { g_onComplete = cb; }

}  // namespace xfer
