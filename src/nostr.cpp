#include "nostr.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <string.h>
#include <esp_random.h>
#include <mbedtls/sha256.h>
#include <time.h>

#include "relays.h"
#include "store_sd.h"
#include "psram.h"
#include <esp_heap_caps.h>
#include "schnorr.h"
#include "ws.h"

namespace nostr {

namespace {

struct Node {
    WiFiClientSecure cli;
    ws::Conn sock;
    volatile bool open = false;
    uint32_t nextTry = 0;
    uint16_t tries = 0;
    size_t   which = 0;      // какой узел из списка сейчас в этой ячейке
};

Node g_node[kMaxOpen];
uint8_t g_peerId[20] = {};
OnMessage g_onMessage = nullptr;
TaskHandle_t g_task = nullptr;

/** Ключ подписи. Свой, отдельный от переписки: узлы видят его открыто, и связывать его с
 *  личностью значило бы выдавать её всякому, кто читает поток событий. */
uint8_t g_sk[32] = {};
uint8_t g_pk[32] = {};
char    g_pkHex[65] = {};

constexpr size_t kMaxRooms = 4;
char g_rooms[kMaxRooms][41] = {};
size_t g_roomCount = 0;

/** Приёмный буфер — во ВНЕШНЕЙ памяти: он держится всё время работы и занимает место,
 *  которого во внутренней не хватало защищённым соединениям. */
char* g_buf = nullptr;
constexpr size_t kBufSize = 4096;

void toHex(const uint8_t* in, size_t len, char* out) {
    static const char* d = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = d[in[i] >> 4];
        out[i * 2 + 1] = d[in[i] & 0x0F];
    }
    out[len * 2] = 0;
}

/** Метка комнаты. Формат из телефонной версии — иначе не встретимся. */
void tagOf(const char* room, char* out, size_t cap) {
    snprintf(out, cap, "vual1-%s", room);
}

/**
 * Признак события: хеш от его же полей, выложенных в строго определённом порядке.
 *
 * Порядок и формат заданы стандартом до пробела: лишняя запятая или другой порядок —
 * и узел отвергнет событие, ничего не объяснив.
 */
void eventId(long createdAt, int kind, const char* tag, const char* content,
             uint8_t out[32]) {
    static char ser[1600];
    const int n = snprintf(ser, sizeof(ser),
        "[0,\"%s\",%ld,%d,[[\"t\",\"%s\"]],\"%s\"]",
        g_pkHex, createdAt, kind, tag, content);
    mbedtls_sha256(reinterpret_cast<const uint8_t*>(ser), size_t(n > 0 ? n : 0), out, 0);
}

/** Экранирование для строки внутри события: кавычки и обратная косая ломают разбор. */
void escape(const char* in, char* out, size_t cap) {
    size_t o = 0;
    for (const char* p = in; *p && o + 2 < cap; ++p) {
        if (*p == '"' || *p == '\\') out[o++] = '\\';
        out[o++] = *p;
    }
    out[o] = 0;
}

/** Собрать и отправить событие всем открытым узлам. */
void publish(const char* room, const char* content) {
    if (!g_pkHex[0]) return;

    char tag[56];
    tagOf(room, tag, sizeof(tag));

    char esc[900];
    escape(content, esc, sizeof(esc));

    const long createdAt = long(time(nullptr));
    uint8_t id[32];
    eventId(createdAt, kKindSignal, tag, esc, id);

    uint8_t sig[64];
    if (!schnorr::sign(id, g_sk, sig)) return;

    char idHex[65], sigHex[129];
    toHex(id, 32, idHex);
    toHex(sig, 64, sigHex);

    static char msg[1600];
    const int n = snprintf(msg, sizeof(msg),
        "[\"EVENT\",{\"id\":\"%s\",\"pubkey\":\"%s\",\"created_at\":%ld,\"kind\":%d,"
        "\"tags\":[[\"t\",\"%s\"]],\"content\":\"%s\",\"sig\":\"%s\"}]",
        idHex, g_pkHex, createdAt, kKindSignal, tag, esc, sigHex);
    if (n <= 0) return;

    for (auto& nd : g_node) {
        if (nd.open) ws::sendText(nd.sock, msg, size_t(n));
    }
}

/** Подписаться на метку комнаты. */
void subscribe(Node& nd, const char* room) {
    char tag[56];
    tagOf(room, tag, sizeof(tag));
    const long since = long(time(nullptr)) - 10;

    char msg[240];
    const int n = snprintf(msg, sizeof(msg),
        "[\"REQ\",\"k%.16s\",{\"kinds\":[%d],\"#t\":[\"%s\"],\"since\":%ld}]",
        room, kKindSignal, tag, since);
    if (n > 0) ws::sendText(nd.sock, msg, size_t(n));
}

/** Значение поля из содержимого. Полноценный разбор здесь не нужен: поля известны и
 *  лежат плоско. */
bool field(const char* json, const char* key, char* out, size_t cap) {
    char pat[24];
    snprintf(pat, sizeof(pat), "\\\"%s\\\":\\\"", key);
    const char* p = strstr(json, pat);
    if (!p) {
        // Содержимое может прийти и неэкранированным — пробуем оба вида.
        snprintf(pat, sizeof(pat), "\"%s\":\"", key);
        p = strstr(json, pat);
        if (!p) return false;
    }
    p += strlen(pat);
    size_t o = 0;
    while (*p && o + 1 < cap) {
        if (*p == '\\' && p[1] == '"') break;
        if (*p == '"') break;
        out[o++] = *p++;
    }
    out[o] = 0;
    return o > 0;
}

size_t unb64(const char* s, uint8_t* out, size_t cap) {
    static const char* kAlpha =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t acc = 0; int bits = 0; size_t n = 0;
    for (const char* p = s; *p && *p != '='; ++p) {
        const char* q = strchr(kAlpha, *p);
        if (!q) continue;
        acc = (acc << 6) | uint32_t(q - kAlpha);
        bits += 6;
        if (bits >= 8) { bits -= 8; if (n < cap) out[n++] = uint8_t(acc >> bits); }
    }
    return n;
}

void handleEvent(const char* text) {
    // Нас интересует только содержимое: остальное — обёртка узла.
    const char* c = strstr(text, "\"content\":\"");
    if (!c) return;

    Incoming in{};
    char tmp[128];
    if (!field(c, "t", tmp, sizeof(tmp))) return;
    in.kind = tmp[0];
    if (!field(c, "ih", in.room, sizeof(in.room))) return;

    if (field(c, "pid", tmp, sizeof(tmp))) unb64(tmp, in.peerId, sizeof(in.peerId));
    // Своё же объявление пропускаем: иначе устройство знакомилось бы само с собой.
    if (memcmp(in.peerId, g_peerId, sizeof(g_peerId)) == 0) return;

    if (field(c, "oid", tmp, sizeof(tmp))) {
        unb64(tmp, in.offerId, sizeof(in.offerId));
        in.hasOffer = true;
    }
    field(c, "sdp", in.body, sizeof(in.body));

    if (g_onMessage) g_onMessage(in);
}

void pumpNode(size_t slot) {
    Node& nd = g_node[slot];
    const uint32_t now = millis();

    if (nd.sock.state != ws::OPEN) {
        if (nd.open) { nd.open = false; Serial.printf("nostr %s отвалился\n",
                                                      relays::kNostr[nd.which]); }
        if (now < nd.nextTry) return;

        nd.tries = nd.tries < 200 ? uint16_t(nd.tries + 1) : nd.tries;
        nd.nextTry = now + (nd.tries <= 3 ? 1500 : (nd.tries <= 8 ? 6000 : 30000));

        // Перебираем список по кругу, каждая ячейка со своим смещением: так три ячейки
        // не толкутся на одних и тех же узлах и вместе перебирают все двадцать пять.
        nd.which = (nd.which + kMaxOpen) % relays::kNostrCount;

        if (!ws::open(nd.sock, nd.cli, relays::kNostr[nd.which], 443, "/", nullptr)) {
            return;
        }
        nd.open = true;
        nd.tries = 0;
        Serial.printf("nostr %s поднят\n", relays::kNostr[nd.which]);
        for (size_t r = 0; r < g_roomCount; ++r) subscribe(nd, g_rooms[r]);
        return;
    }

    ws::keepAlive(nd.sock);

    bool bin = false;
    size_t n = ws::receive(nd.sock, reinterpret_cast<uint8_t*>(g_buf), kBufSize - 1, &bin);
    while (n > 0) {
        g_buf[n] = 0;
        if (strncmp(g_buf, "[\"EVENT\"", 8) == 0) handleEvent(g_buf);
        n = ws::receive(nd.sock, reinterpret_cast<uint8_t*>(g_buf), kBufSize - 1, &bin);
    }
}

/** Свой поток на втором ядре — по той же причине, что и у первой рельсы: подключение
 *  занимает секунды, и держать ими главный цикл нельзя. */
void nostrTask(void*) {
    for (;;) {
        if (WiFi.isConnected()) {
            for (size_t i = 0; i < kMaxOpen; ++i) pumpNode(i);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

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

}  // namespace

bool begin(const uint8_t myPeerId[20]) {
    if (!g_buf) g_buf = static_cast<char*>(psram::alloc(kBufSize));
    if (!g_buf) { store::log("nostr", "нет памяти под буфер"); return false; }
    memcpy(g_peerId, myPeerId, sizeof(g_peerId));

    // Ключ подписи создаём заново при каждом запуске. Постоянный связывал бы все наши
    // появления в одну цепочку, а знакомство — дело разовое, помнить нас узлам незачем.
    esp_fill_random(g_sk, sizeof(g_sk));
    if (!schnorr::publicKey(g_sk, g_pk)) {
        // Без ключа подписи рельса не запускается ВООБЩЕ: поток не создаётся, и на
        // экране состояния вечный ноль без единого слова о причине.
        store::log("nostr", "не вышел ключ подписи — рельса не запущена");
        return false;
    }
    toHex(g_pk, sizeof(g_pk), g_pkHex);

    for (size_t i = 0; i < kMaxOpen; ++i) {
        g_node[i].which = i;             // разводим ячейки по списку
        g_node[i].nextTry = 0;
    }

    if (g_task) return true;
    // Результат создания потока ПРОВЕРЯЕТСЯ. Стек потока живёт только во внутренней
    // памяти (ограничение системы), и когда её в момент старта не хватает, поток молча
    // не рождается: begin рапортует успех, на экране вечные нули, в журнале тишина —
    // ровно та картина, которую мы наблюдали. Отказ теперь называет себя.
    if (xTaskCreatePinnedToCore(nostrTask, "vual-nostr", 10240, nullptr, 1,
                                &g_task, 0) != pdPASS) {
        g_task = nullptr;
        char m[80];
        snprintf(m, sizeof(m), "поток не создался (внутренней свободно %lu Б)",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        store::log("nostr", m);
        return false;
    }
    store::log("nostr", "рельса поднята");
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
    for (auto& nd : g_node) if (nd.open) subscribe(nd, roomHex);
}

void leaveRoom(const char* roomHex) {
    for (size_t i = 0; i < g_roomCount; ++i) {
        if (strcmp(g_rooms[i], roomHex) != 0) continue;
        // Отписку узлам не шлём: соединение всё равно недолгое, а лишний обмен только
        // добавил бы заметности.
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
             "{\\\"v\\\":1,\\\"t\\\":\\\"p\\\",\\\"ih\\\":\\\"%s\\\",\\\"pid\\\":\\\"%s\\\"}",
             roomHex, pid);
    publish(roomHex, msg);
}

void sendOffer(const char* roomHex, const uint8_t offerId[20], const char* body) {
    char pid[32], oid[32];
    b64(g_peerId, sizeof(g_peerId), pid, sizeof(pid));
    b64(offerId, 20, oid, sizeof(oid));
    char msg[700];
    snprintf(msg, sizeof(msg),
             "{\\\"v\\\":1,\\\"t\\\":\\\"o\\\",\\\"ih\\\":\\\"%s\\\",\\\"pid\\\":\\\"%s\\\","
             "\\\"oid\\\":\\\"%s\\\",\\\"sdp\\\":\\\"%s\\\"}",
             roomHex, pid, oid, body ? body : "");
    publish(roomHex, msg);
}

void sendAnswer(const char* roomHex, const uint8_t offerId[20], const char* body) {
    char pid[32], oid[32];
    b64(g_peerId, sizeof(g_peerId), pid, sizeof(pid));
    b64(offerId, 20, oid, sizeof(oid));
    char msg[700];
    snprintf(msg, sizeof(msg),
             "{\\\"v\\\":1,\\\"t\\\":\\\"a\\\",\\\"ih\\\":\\\"%s\\\",\\\"pid\\\":\\\"%s\\\","
             "\\\"oid\\\":\\\"%s\\\",\\\"sdp\\\":\\\"%s\\\"}",
             roomHex, pid, oid, body ? body : "");
    publish(roomHex, msg);
}

void setOnMessage(OnMessage cb) { g_onMessage = cb; }

}  // namespace nostr
