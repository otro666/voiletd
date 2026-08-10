#include "tracker.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <string.h>
#include <esp_random.h>

#include "relays.h"
#include "psram.h"
#include "store_sd.h"
#include "ws.h"

namespace tracker {

namespace {

struct Node {
    WiFiClientSecure cli;
    ws::Conn sock;
    volatile bool open = false;
    uint32_t nextTry = 0;
    uint32_t lastAnnounce = 0;
    uint16_t tries = 0;
    size_t   which = 0;
};

Node g_node[kMaxOpen];
uint8_t g_peerId[20] = {};
OnMessage g_onMessage = nullptr;
TaskHandle_t g_task = nullptr;

constexpr size_t kMaxRooms = 4;
char g_rooms[kMaxRooms][41] = {};
size_t g_roomCount = 0;

/** Приёмный буфер — во ВНЕШНЕЙ памяти: живёт всё время работы, а внутренняя нужна
 *  защищённым соединениям. */
char* g_buf = nullptr;
constexpr size_t kBufSize = 4096;

/** Объявляемся не чаще раза в полминуты: трекеры за назойливость отключают. */
constexpr uint32_t kAnnounceMs = 30000;

/**
 * Двоичное значение в строку протокола.
 *
 * Каждый байт становится символом с тем же кодом. Байты выше 0x7F занимают в записи два
 * места — так требует кодировка текста, и трекер разбирает их обратно в один байт.
 * Кавычки и обратную косую экранируем: без этого строка порвёт разбор на стороне трекера.
 */
size_t binToJsonString(const uint8_t* in, size_t len, char* out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < len && o + 6 < cap; ++i) {
        const uint8_t b = in[i];
        if (b == '"' || b == '\\') { out[o++] = '\\'; out[o++] = char(b); }
        else if (b < 0x20) { o += size_t(snprintf(out + o, cap - o, "\\u%04X", b)); }
        else if (b < 0x80) { out[o++] = char(b); }
        else {
            // Двухбайтовая запись символа с кодом 0x80..0xFF.
            out[o++] = char(0xC0 | (b >> 6));
            out[o++] = char(0x80 | (b & 0x3F));
        }
    }
    out[o] = 0;
    return o;
}

/** Обратно: из строки протокола в байты. */
size_t jsonStringToBin(const char* in, size_t len, uint8_t* out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < len && o < cap; ) {
        const uint8_t c = uint8_t(in[i]);
        if (c == '\\' && i + 1 < len) {
            if (in[i + 1] == 'u' && i + 5 < len) {
                unsigned v = 0;
                sscanf(in + i + 2, "%4x", &v);
                out[o++] = uint8_t(v);
                i += 6;
            } else { out[o++] = uint8_t(in[i + 1]); i += 2; }
        } else if ((c & 0xE0) == 0xC0 && i + 1 < len) {
            out[o++] = uint8_t(((c & 0x1F) << 6) | (uint8_t(in[i + 1]) & 0x3F));
            i += 2;
        } else { out[o++] = c; ++i; }
    }
    return o;
}

/** Имя раздачи — двадцать байт, выведенные из комнаты. Комната приходит в
 *  шестнадцатеричном виде, разбираем обратно. */
void roomToBin(const char* roomHex, uint8_t out[20]) {
    for (int i = 0; i < 20; ++i) {
        unsigned v = 0;
        sscanf(roomHex + i * 2, "%2x", &v);
        out[i] = uint8_t(v);
    }
}

bool field(const char* json, const char* key, char* out, size_t cap) {
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char* p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < cap) {
        if (*p == '\\' && p[1]) { out[o++] = *p++; }
        out[o++] = *p++;
    }
    out[o] = 0;
    return o > 0;
}

void sendAnnounce(Node& nd, const char* room, const uint8_t* offerId,
                  const char* body, char kind) {
    uint8_t ihBin[20];
    roomToBin(room, ihBin);

    char ih[64], pid[64];
    binToJsonString(ihBin, 20, ih, sizeof(ih));
    binToJsonString(g_peerId, 20, pid, sizeof(pid));

    static char msg[1200];
    int n;
    if (kind == 'p') {
        // Просто объявляемся и просим показать соседей.
        n = snprintf(msg, sizeof(msg),
            "{\"action\":\"announce\",\"info_hash\":\"%s\",\"peer_id\":\"%s\",\"numwant\":10}",
            ih, pid);
    } else {
        char oid[64];
        binToJsonString(offerId, 20, oid, sizeof(oid));
        if (kind == 'o') {
            n = snprintf(msg, sizeof(msg),
                "{\"action\":\"announce\",\"info_hash\":\"%s\",\"peer_id\":\"%s\",\"numwant\":10,"
                "\"offers\":[{\"offer_id\":\"%s\",\"offer\":{\"type\":\"offer\",\"sdp\":\"%s\"}}]}",
                ih, pid, oid, body ? body : "");
        } else {
            n = snprintf(msg, sizeof(msg),
                "{\"action\":\"announce\",\"info_hash\":\"%s\",\"peer_id\":\"%s\","
                "\"offer_id\":\"%s\",\"answer\":{\"type\":\"answer\",\"sdp\":\"%s\"}}",
                ih, pid, oid, body ? body : "");
        }
    }
    if (n > 0) ws::sendText(nd.sock, msg, size_t(n));
}

void handleMessage(const char* text) {
    // Трекер шлёт и служебное — оно нам не нужно.
    if (!strstr(text, "\"info_hash\"")) return;

    char tmp[256];
    Incoming in{};

    // Кто прислал.
    if (!field(text, "peer_id", tmp, sizeof(tmp))) return;
    uint8_t pid[20];
    if (jsonStringToBin(tmp, strlen(tmp), pid, sizeof(pid)) != 20) return;
    // Своё же объявление пропускаем: иначе устройство знакомилось бы само с собой.
    if (memcmp(pid, g_peerId, sizeof(pid)) == 0) return;
    memcpy(in.peerId, pid, sizeof(pid));

    // Из какой раздачи.
    if (!field(text, "info_hash", tmp, sizeof(tmp))) return;
    uint8_t ih[20];
    if (jsonStringToBin(tmp, strlen(tmp), ih, sizeof(ih)) != 20) return;
    for (int i = 0; i < 20; ++i) snprintf(in.room + i * 2, 3, "%02x", ih[i]);

    // Что именно: предложение, ответ или просто присутствие.
    if (strstr(text, "\"offer\"")) in.kind = 'o';
    else if (strstr(text, "\"answer\"")) in.kind = 'a';
    else in.kind = 'p';

    if (field(text, "offer_id", tmp, sizeof(tmp))) {
        jsonStringToBin(tmp, strlen(tmp), in.offerId, sizeof(in.offerId));
        in.hasOffer = true;
    }
    field(text, "sdp", in.body, sizeof(in.body));

    if (g_onMessage) g_onMessage(in);
}

void pumpNode(size_t slot) {
    Node& nd = g_node[slot];
    const uint32_t now = millis();

    if (nd.sock.state != ws::OPEN) {
        if (nd.open) {
            nd.open = false;
            Serial.printf("трекер %s отвалился\n", relays::kTrackers[nd.which].host);
        }
        if (now < nd.nextTry) return;

        nd.tries = nd.tries < 200 ? uint16_t(nd.tries + 1) : nd.tries;
        nd.nextTry = now + (nd.tries <= 3 ? 1500 : (nd.tries <= 8 ? 6000 : 30000));

        // Перебираем список со смещением, чтобы три ячейки не толклись на одних узлах.
        nd.which = (nd.which + kMaxOpen) % relays::kTrackerCount;
        const relays::Tracker& t = relays::kTrackers[nd.which];

        if (!ws::open(nd.sock, nd.cli, t.host, t.port, t.path, nullptr)) return;

        nd.open = true;
        nd.tries = 0;
        nd.lastAnnounce = 0;
        Serial.printf("трекер %s поднят\n", t.host);
        return;
    }

    ws::keepAlive(nd.sock);

    // Объявляемся во всех своих раздачах. Трекер помнит нас недолго, поэтому повторяем.
    if (g_roomCount && now - nd.lastAnnounce > kAnnounceMs) {
        nd.lastAnnounce = now;
        for (size_t r = 0; r < g_roomCount; ++r) sendAnnounce(nd, g_rooms[r], nullptr, nullptr, 'p');
    }

    bool bin = false;
    size_t n = ws::receive(nd.sock, reinterpret_cast<uint8_t*>(g_buf), kBufSize - 1, &bin);
    while (n > 0) {
        g_buf[n] = 0;
        handleMessage(g_buf);
        n = ws::receive(nd.sock, reinterpret_cast<uint8_t*>(g_buf), kBufSize - 1, &bin);
    }
}

/** Свой поток на втором ядре — как у двух других рельс. */
void trackerTask(void*) {
    for (;;) {
        if (WiFi.isConnected()) {
            for (size_t i = 0; i < kMaxOpen; ++i) pumpNode(i);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

}  // namespace

bool begin(const uint8_t myPeerId[20]) {
    if (!g_buf) g_buf = static_cast<char*>(psram::alloc(kBufSize));
    if (!g_buf) { store::log("tracker", "нет памяти под буфер"); return false; }
    memcpy(g_peerId, myPeerId, sizeof(g_peerId));
    for (size_t i = 0; i < kMaxOpen; ++i) {
        g_node[i].which = i;
        g_node[i].nextTry = 0;
    }
    if (g_task) return true;
    xTaskCreatePinnedToCore(trackerTask, "vual-tracker", 10240, nullptr, 1, &g_task, 0);
    return g_task != nullptr;
}

bool connected() {
    for (const auto& nd : g_node) if (nd.open) return true;
    return false;
}

size_t openCount() {
    size_t n = 0;
    for (const auto& nd : g_node) if (nd.open) ++n;
    return n;
}

void joinRoom(const char* roomHex) {
    if (!roomHex || !*roomHex) return;
    for (size_t i = 0; i < g_roomCount; ++i)
        if (strcmp(g_rooms[i], roomHex) == 0) return;
    if (g_roomCount >= kMaxRooms) return;
    snprintf(g_rooms[g_roomCount], sizeof(g_rooms[0]), "%s", roomHex);
    ++g_roomCount;
    // Объявляемся сразу, не дожидаясь очередного круга: знакомство ждут прямо сейчас.
    for (auto& nd : g_node) {
        if (nd.open) { sendAnnounce(nd, roomHex, nullptr, nullptr, 'p'); nd.lastAnnounce = millis(); }
    }
}

void leaveRoom(const char* roomHex) {
    for (size_t i = 0; i < g_roomCount; ++i) {
        if (strcmp(g_rooms[i], roomHex) != 0) continue;
        for (size_t j = i; j + 1 < g_roomCount; ++j)
            memcpy(g_rooms[j], g_rooms[j + 1], sizeof(g_rooms[0]));
        --g_roomCount;
        return;
    }
}

void announce(const char* roomHex) {
    for (auto& nd : g_node) if (nd.open) sendAnnounce(nd, roomHex, nullptr, nullptr, 'p');
}

void sendOffer(const char* roomHex, const uint8_t offerId[20], const char* body) {
    for (auto& nd : g_node) if (nd.open) sendAnnounce(nd, roomHex, offerId, body, 'o');
}

void sendAnswer(const char* roomHex, const uint8_t offerId[20], const char* body) {
    for (auto& nd : g_node) if (nd.open) sendAnnounce(nd, roomHex, offerId, body, 'a');
}

void setOnMessage(OnMessage cb) { g_onMessage = cb; }

}  // namespace tracker
