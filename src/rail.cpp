#include "rail.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "ws.h"
#include <string.h>

#include "relays.h"
#include "psram.h"
#include "store_sd.h"
#include <esp_heap_caps.h>

namespace rail {

namespace {

/**
 * Брокеры. Те же, что у телефонной версии, но порт защищённого MQTT вместо
 * веб-сокета: на плате поднимать ещё и веб-сокет поверх защищённого соединения — лишний
 * слой, а комната и содержимое от этого не меняются. Стороны встречаются в одной теме
 * на одном брокере независимо от того, как каждая к нему подключилась.
 */
// Список брокеров общий — см. relays.h.
struct Conn {
    WiFiClientSecure cli;
    ws::Conn sock;
    // Читается из ДРУГОГО потока — того, что рисует значки в шапке. Пометка обязывает
    // компилятор перечитывать значение, а не держать его в регистре.
    volatile bool open = false;
    uint32_t lastPing = 0;
    uint32_t nextTry = 0;
    uint16_t packetId = 1;
    uint16_t tries = 0;
};

Conn g_conn[kMaxBrokers];
uint8_t g_peerId[20] = {};
OnMessage g_onMessage = nullptr;

constexpr size_t kMaxRooms = 4;
char g_rooms[kMaxRooms][41] = {};
size_t g_roomCount = 0;

/** Рабочий буфер пакетов — во внешней памяти по той же причине, что у прочих рельс. */
uint8_t* g_buf = nullptr;
constexpr size_t kBufSize = 4096;

// ── упаковка чисел переменной длины ────────────────────────────────────────────────────
//
// В MQTT длина пакета пишется по семь бит на байт: старший бит означает «есть
// продолжение». Иначе короткие пакеты тратили бы четыре байта на длину.

size_t writeLen(uint8_t* out, size_t len) {
    size_t n = 0;
    do {
        uint8_t b = len % 128;
        len /= 128;
        if (len) b |= 0x80;
        out[n++] = b;
    } while (len);
    return n;
}

size_t readLen(const uint8_t* in, size_t avail, size_t& lenOut) {
    lenOut = 0; size_t mult = 1, n = 0;
    while (n < avail && n < 4) {
        const uint8_t b = in[n++];
        lenOut += (b & 0x7F) * mult;
        if (!(b & 0x80)) return n;
        mult *= 128;
    }
    return 0;
}

size_t writeStr(uint8_t* out, const char* s) {
    const size_t n = strlen(s);
    out[0] = uint8_t(n >> 8); out[1] = uint8_t(n & 0xFF);
    memcpy(out + 2, s, n);
    return n + 2;
}

/** Тема комнаты. Формат тот же, что в телефонной версии, — иначе не встретимся. */
void topicOf(const char* room, char* out, size_t cap) {
    snprintf(out, cap, "vual1/%s", room);
}

void sendConnect(Conn& c) {
    uint8_t body[64];
    size_t o = 0;
    o += writeStr(body + o, "MQTT");
    body[o++] = 4;              // версия 3.1.1
    body[o++] = 0x02;           // чистая сессия
    body[o++] = 0x00; body[o++] = 60;   // держать связь 60 секунд

    // Имя участника случайное: постоянное было бы приметой, по которой нас узнают
    // между сеансами.
    char cid[24];
    snprintf(cid, sizeof(cid), "v%08lx%04x", (unsigned long)esp_random(),
             unsigned(esp_random() & 0xFFFF));
    o += writeStr(body + o, cid);

    uint8_t hdr[8];
    size_t h = 0;
    hdr[h++] = 0x10;
    h += writeLen(hdr + h, o);
    // Собираем пакет целиком и отдаём ОДНИМ кадром: веб-сокет передаёт сообщениями, и
    // разбить пакет на два кадра значило бы, что брокер получит половину.
    uint8_t pkt[128];
    memcpy(pkt, hdr, h);
    memcpy(pkt + h, body, o);
    ws::sendBinary(c.sock, pkt, h + o);
}

void sendSubscribe(Conn& c, const char* room) {
    char topic[64];
    topicOf(room, topic, sizeof(topic));

    uint8_t body[80];
    size_t o = 0;
    const uint16_t pid = c.packetId++;
    body[o++] = uint8_t(pid >> 8); body[o++] = uint8_t(pid & 0xFF);
    o += writeStr(body + o, topic);
    body[o++] = 0;              // без подтверждений: знакомство переспросит само

    // Пакет уходит ОДНИМ кадром: веб-сокет передаёт сообщениями, и разбить пакет на
    // два кадра значило бы, что брокер получит половину.
    uint8_t pkt[160];
    size_t h = 0;
    pkt[h++] = 0x82;
    h += writeLen(pkt + h, o);
    memcpy(pkt + h, body, o);
    ws::sendBinary(c.sock, pkt, h + o);
}

void sendUnsubscribe(Conn& c, const char* room) {
    char topic[64];
    topicOf(room, topic, sizeof(topic));
    uint8_t body[80];
    size_t o = 0;
    const uint16_t pid = c.packetId++;
    body[o++] = uint8_t(pid >> 8); body[o++] = uint8_t(pid & 0xFF);
    o += writeStr(body + o, topic);

    // Пакет уходит ОДНИМ кадром: веб-сокет передаёт сообщениями, и разбить пакет на
    // два кадра значило бы, что брокер получит половину.
    uint8_t pkt[160];
    size_t h = 0;
    pkt[h++] = 0xA2;
    h += writeLen(pkt + h, o);
    memcpy(pkt + h, body, o);
    ws::sendBinary(c.sock, pkt, h + o);
}

void sendPublish(Conn& c, const char* room, const char* payload) {
    char topic[64];
    topicOf(room, topic, sizeof(topic));

    const size_t plen = strlen(payload);
    const size_t tlen = strlen(topic) + 2;
    const size_t total = tlen + plen;
    if (total > kBufSize) return;

    static uint8_t pkt[1024];
    size_t h = 0;
    pkt[h++] = 0x30;            // публикация, без подтверждений
    h += writeLen(pkt + h, total);
    h += writeStr(pkt + h, topic);
    if (h + plen > sizeof(pkt)) return;
    memcpy(pkt + h, payload, plen);
    ws::sendBinary(c.sock, pkt, h + plen);
}

/** Простейший разбор значения из содержимого. Полноценный разборщик здесь не нужен:
 *  поля известны заранее и лежат плоско. */
bool field(const char* json, const char* key, char* out, size_t cap) {
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char* p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    const char* e = strchr(p, '"');
    if (!e) return false;
    const size_t n = size_t(e - p) < cap - 1 ? size_t(e - p) : cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return true;
}

/** Расшифровка из основания 64 — идентификаторы передаются именно так. */
size_t unb64(const char* s, uint8_t* out, size_t cap) {
    static const char* kAlpha =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t acc = 0; int bits = 0; size_t n = 0;
    for (const char* p = s; *p && *p != '='; ++p) {
        const char* q = strchr(kAlpha, *p);
        if (!q) continue;
        acc = (acc << 6) | uint32_t(q - kAlpha);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n < cap) out[n++] = uint8_t(acc >> bits);
        }
    }
    return n;
}

/** Упаковка в основание 64 — для наших идентификаторов. */
void b64(const uint8_t* in, size_t len, char* out, size_t cap) {
    static const char* kAlpha =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t v = (uint32_t(in[i]) << 16) |
                           (i + 1 < len ? uint32_t(in[i + 1]) << 8 : 0) |
                           (i + 2 < len ? uint32_t(in[i + 2]) : 0);
        if (o + 4 >= cap) break;
        out[o++] = kAlpha[(v >> 18) & 63];
        out[o++] = kAlpha[(v >> 12) & 63];
        out[o++] = i + 1 < len ? kAlpha[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? kAlpha[v & 63] : '=';
    }
    out[o] = 0;
}

void handlePublish(const uint8_t* body, size_t len) {
    if (len < 2) return;
    const size_t tl = (size_t(body[0]) << 8) | body[1];
    if (2 + tl > len) return;

    char payload[600];
    const size_t plen = len - 2 - tl;
    const size_t take = plen < sizeof(payload) - 1 ? plen : sizeof(payload) - 1;
    memcpy(payload, body + 2 + tl, take);
    payload[take] = 0;

    Incoming in{};
    char tmp[128];
    if (!field(payload, "t", tmp, sizeof(tmp))) return;
    in.kind = tmp[0];
    if (!field(payload, "ih", in.room, sizeof(in.room))) return;

    if (field(payload, "pid", tmp, sizeof(tmp))) unb64(tmp, in.peerId, sizeof(in.peerId));
    // Своё же объявление пропускаем: иначе устройство знакомилось бы само с собой.
    if (memcmp(in.peerId, g_peerId, sizeof(g_peerId)) == 0) return;

    if (field(payload, "oid", tmp, sizeof(tmp))) {
        unb64(tmp, in.offerId, sizeof(in.offerId));
        in.hasOffer = true;
    }
    field(payload, "sdp", in.body, sizeof(in.body));

    if (g_onMessage) g_onMessage(in);
}

void pumpConn(size_t i) {
    Conn& c = g_conn[i];
    const uint32_t now = millis();

    if (c.sock.state != ws::OPEN) {
        if (c.open) {
            c.open = false;
            Serial.printf("рельса %s отвалилась\n", relays::kBrokers[i].host);
        }
        if (now < c.nextTry) return;

        // Первые попытки часто, дальше реже: связь нужна сразу после подключения к сети,
        // а если брокер недоступен из этой сети вовсе, долбиться постоянно — впустую
        // жечь батарею.
        c.tries = c.tries < 200 ? uint16_t(c.tries + 1) : c.tries;
        c.nextTry = now + (c.tries <= 3 ? 1000 : (c.tries <= 8 ? 5000 : 30000));

        // Через веб-сокет — так же, как телефонная версия. Название языка обязательно:
        // по нему брокер понимает, что внутри кадров, и без него отказывает.
        if (!ws::open(c.sock, c.cli, relays::kBrokers[i].host, relays::kBrokers[i].port,
                      relays::kBrokerPath, "mqtt")) {
            return;
        }

        sendConnect(c);
        c.open = true;
        c.tries = 0;
        c.lastPing = now;
        Serial.printf("рельса %s поднята\n", relays::kBrokers[i].host);
        for (size_t r = 0; r < g_roomCount; ++r) sendSubscribe(c, g_rooms[r]);
        return;
    }

    ws::keepAlive(c.sock);

    // Поддержание связи на своём уровне: брокер разрывает соединение по тишине, даже
    // когда веб-сокет под ним жив.
    if (now - c.lastPing > 30000) {
        c.lastPing = now;
        const uint8_t ping[2] = {0xC0, 0x00};
        ws::sendBinary(c.sock, ping, 2);
    }

    // В каждом кадре — целый пакет брокера, разбираем сразу.
    bool bin = false;
    size_t n = ws::receive(c.sock, g_buf, kBufSize, &bin);
    while (n > 0) {
        if (n >= 2) {
            const uint8_t type = g_buf[0];
            size_t len = 0;
            const size_t used = readLen(g_buf + 1, n - 1, len);
            if (used && 1 + used + len <= n && (type & 0xF0) == 0x30) {
                handlePublish(g_buf + 1 + used, len);
            }
        }
        n = ws::receive(c.sock, g_buf, kBufSize, &bin);
    }
}

}  // namespace

namespace {

/**
 * Отдельный поток на ВТОРОМ ядре.
 *
 * Подключение к брокеру занимает секунды, и делать его в общем цикле нельзя: пока идёт
 * попытка, устройство не рисует, не читает клавиатуру и не принимает радио. Перебирать по
 * одному брокеру раз в полминуты — тоже негодно: связи с телефоном пришлось бы ждать
 * минутами.
 *
 * У платы два ядра, и второе почти простаивает. Отдаём ему всю сетевую возню: там она
 * может идти хоть непрерывно, никому не мешая, и ко всем шести брокерам мы приходим
 * одновременно — соединение появляется за секунды, а не за минуты.
 */
void railTask(void*) {
    for (;;) {
        if (WiFi.isConnected()) {
            for (size_t i = 0; i < kMaxBrokers; ++i) pumpConn(i);
        }
        // Пауза короткая: поток свой, тормозить он никого не может, а отклик от этого
        // становится заметно живее.
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

TaskHandle_t g_task = nullptr;

}  // namespace

bool begin(const uint8_t myPeerId[20]) {
    if (!g_buf) g_buf = static_cast<uint8_t*>(psram::alloc(kBufSize));
    if (!g_buf) return false;
    memcpy(g_peerId, myPeerId, sizeof(g_peerId));
    for (auto& c : g_conn) { c.open = false; c.nextTry = 0; }

    if (g_task) return true;           // поток уже поднят
    // Ядро 0: на первом живёт основной цикл с экраном и радио, и делить его с сетью
    // незачем. Стека 8 КБ — защищённому соединению нужно заметно больше обычного.
    // Результат создания потока ПРОВЕРЯЕТСЯ. Стек потока живёт только во внутренней
    // памяти (ограничение системы), и когда её в момент старта не хватает, поток молча
    // не рождается: begin рапортует успех, на экране вечные нули, в журнале тишина —
    // ровно та картина, которую мы наблюдали. Отказ теперь называет себя.
    if (xTaskCreatePinnedToCore(railTask, "vual-rail", 8192, nullptr, 1,
                                &g_task, 0) != pdPASS) {
        g_task = nullptr;
        char m[80];
        snprintf(m, sizeof(m), "поток не создался (внутренней свободно %lu Б)",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        store::log("rail", m);
        return false;
    }
    store::log("rail", "рельса поднята");
    return g_task != nullptr;
}


void pump() {
    // Работа идёт в своём потоке — здесь делать нечего. Оставлено, чтобы вызывающему
    // коду не пришлось знать, как именно устроена рельса.
}

bool connected() {
    for (const auto& c : g_conn) if (c.open) return true;
    return false;
}

void joinRoom(const char* roomHex) {
    if (!roomHex || !*roomHex) return;
    for (size_t i = 0; i < g_roomCount; ++i)
        if (strcmp(g_rooms[i], roomHex) == 0) return;
    if (g_roomCount >= kMaxRooms) return;
    snprintf(g_rooms[g_roomCount], sizeof(g_rooms[0]), "%s", roomHex);
    ++g_roomCount;
    // Подписываемся у ВСЕХ поднятых брокеров: собеседник мог прийти к любому из них,
    // и угадывать, к какому именно, нечем.
    for (auto& c : g_conn) if (c.open) sendSubscribe(c, roomHex);
}

void leaveRoom(const char* roomHex) {
    for (size_t i = 0; i < g_roomCount; ++i) {
        if (strcmp(g_rooms[i], roomHex) != 0) continue;
        for (auto& c : g_conn) if (c.open) sendUnsubscribe(c, roomHex);
        for (size_t j = i; j + 1 < g_roomCount; ++j)
            memcpy(g_rooms[j], g_rooms[j + 1], sizeof(g_rooms[0]));
        --g_roomCount;
        return;
    }
}

void announce(const char* roomHex) {
    char pid[32];
    b64(g_peerId, sizeof(g_peerId), pid, sizeof(pid));
    char msg[256];
    snprintf(msg, sizeof(msg),
             "{\"v\":1,\"t\":\"p\",\"ih\":\"%s\",\"pid\":\"%s\"}", roomHex, pid);
    for (auto& c : g_conn) if (c.open) sendPublish(c, roomHex, msg);
}

void sendOffer(const char* roomHex, const uint8_t offerId[20], const char* body) {
    char pid[32], oid[32];
    b64(g_peerId, sizeof(g_peerId), pid, sizeof(pid));
    b64(offerId, 20, oid, sizeof(oid));
    char msg[700];
    snprintf(msg, sizeof(msg),
             "{\"v\":1,\"t\":\"o\",\"ih\":\"%s\",\"pid\":\"%s\",\"oid\":\"%s\",\"sdp\":\"%s\"}",
             roomHex, pid, oid, body ? body : "");
    for (auto& c : g_conn) if (c.open) sendPublish(c, roomHex, msg);
}

void sendAnswer(const char* roomHex, const uint8_t offerId[20], const char* body) {
    char pid[32], oid[32];
    b64(g_peerId, sizeof(g_peerId), pid, sizeof(pid));
    b64(offerId, 20, oid, sizeof(oid));
    char msg[700];
    snprintf(msg, sizeof(msg),
             "{\"v\":1,\"t\":\"a\",\"ih\":\"%s\",\"pid\":\"%s\",\"oid\":\"%s\",\"sdp\":\"%s\"}",
             roomHex, pid, oid, body ? body : "");
    for (auto& c : g_conn) if (c.open) sendPublish(c, roomHex, msg);
}

void setOnMessage(OnMessage cb) { g_onMessage = cb; }

size_t brokerCount() { return kMaxBrokers; }
const char* brokerName(size_t i) { return i < kMaxBrokers ? relays::kBrokers[i].host : ""; }
bool brokerOnline(size_t i) { return i < kMaxBrokers && g_conn[i].open; }

}  // namespace rail
