#include "net.h"
#include "psram.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <string.h>
#include <esp_random.h>

#include "cloak.h"
#include "relays.h"
#include "contacts.h"
#include "voile_crypto.h"

namespace net {

namespace {

WiFiUDP g_lan;     // приём объявлений и обмен
WiFiUDP g_stun;    // отдельный сокет для внешнего адреса
bool    g_ready = false;
uint16_t g_port = 0;
uint8_t g_myId[kIdLen] = {};
char    g_external[32] = {};
uint32_t g_lastAnnounce = 0;
OnFrame g_onFrame = nullptr;
void (*g_toRadio)(const uint8_t[4], const uint8_t*, size_t) = nullptr;

constexpr size_t kMaxNeighbours = 16;
Neighbour g_neighbours[kMaxNeighbours];
size_t    g_neighbourCount = 0;

/** Собеседник считается доступным, если объявлялся недавно. Порог щедрый: объявления
 *  идут раз в пять секунд, но в загруженной сети часть теряется. */
constexpr uint32_t kFreshMs = 45000;

// Три постоянных буфера обмена — во внешней памяти: вместе это почти пять килобайт,
// которые во внутренней нужнее защищённым соединениям рельс.
uint8_t* g_rx = nullptr;
uint8_t* g_plain = nullptr;
uint8_t* g_out = nullptr;
constexpr size_t kNetBuf = 1600;

/** Имя узла выводим из долговременного ключа: оно должно быть тем же и после
 *  перезапуска, иначе собеседники нас не узнают. */
void computeMyId() {
    static const char kInfo[] = "voile node id";
    uint8_t h[32];
    voile::hkdf(contacts::myPub(), voile::kPubComp, nullptr, 0,
                reinterpret_cast<const uint8_t*>(kInfo), sizeof(kInfo) - 1, h, sizeof(h));
    memcpy(g_myId, h, kIdLen);
}

Neighbour* findNeighbour(const uint8_t id[kIdLen]) {
    for (size_t i = 0; i < g_neighbourCount; ++i) {
        if (memcmp(g_neighbours[i].id, id, kIdLen) == 0) return &g_neighbours[i];
    }
    return nullptr;
}

void rememberNeighbour(const uint8_t id[kIdLen], uint32_t ip, uint16_t port) {
    Neighbour* n = findNeighbour(id);
    if (!n) {
        if (g_neighbourCount >= kMaxNeighbours) {
            // Вытесняем самого старого: список не резиновый, а свежие полезнее.
            size_t oldest = 0;
            for (size_t i = 1; i < g_neighbourCount; ++i)
                if (g_neighbours[i].lastSeenMs < g_neighbours[oldest].lastSeenMs) oldest = i;
            n = &g_neighbours[oldest];
        } else {
            n = &g_neighbours[g_neighbourCount++];
        }
        memcpy(n->id, id, kIdLen);
    }
    n->ip = ip;
    n->port = port;
    n->lastSeenMs = millis();
}

/** Объявление о себе: имя узла и порт, всё под обфускацией. Формат из VoileLan.kt. */
void announce() {
    uint8_t p[kIdLen + 2];
    memcpy(p, g_myId, kIdLen);
    p[kIdLen] = uint8_t(g_port >> 8);
    p[kIdLen + 1] = uint8_t(g_port & 0xFF);

    const size_t n = cloak::seal(cloak::discoveryKey(), p, sizeof(p), g_out, kNetBuf);
    if (n == 0) return;                 // часы не выставлены — объявляться бессмысленно

    // Группа — основной путь, широковещание — запасной: в части сетей групповую рассылку
    // режут, и без второго пути связь дома просто не возникала бы.
    g_lan.beginPacket(kLanGroup, kLanPort);
    g_lan.write(g_out, n);
    g_lan.endPacket();

    IPAddress bcast = WiFi.localIP();
    bcast[3] = 255;
    g_lan.beginPacket(bcast, kLanPort);
    g_lan.write(g_out, n);
    g_lan.endPacket();
}

/** Разбор принятого пакета. */
void handlePacket(const uint8_t* buf, size_t len, uint32_t fromIp, uint16_t fromPort) {
    // Сначала пробуем ключ рассылки: так приходят объявления и запросы знакомства.
    size_t n = cloak::open(cloak::discoveryKey(), buf, len, g_plain, kNetBuf);
    if (n == kIdLen + 2) {
        const uint16_t port = uint16_t((g_plain[kIdLen] << 8) | g_plain[kIdLen + 1]);
        if (memcmp(g_plain, g_myId, kIdLen) != 0) {   // своё объявление пропускаем
            rememberNeighbour(g_plain, fromIp, port);
        }
        return;
    }

    // Затем — ключ, выведенный из НАШЕГО имени: так приходят сообщения лично нам.
    uint8_t key[32];
    cloak::keyForNode(g_myId, key);
    n = cloak::open(key, buf, len, g_plain, kNetBuf);
    if (n == 0) return;                 // чужое или мусор — молчим, не отвечаем

    // ── ответ на опрос DHT ────────────────────────────────────────────────────────────
    //
    // Телефон, получив наше объявление, добавляет нас как узел и НАЧИНАЕТ ОПРАШИВАТЬ по
    // своему протоколу. Не ответив, мы выглядим мёртвыми, и он нас выбрасывает — именно
    // поэтому обнаружение «работало», а связи не возникало.
    //
    // Полный протокол нам не нужен: достаточно отзываться на опрос и отдавать пустой
    // список соседей. Тогда телефон держит нас в таблице и мы остаёмся достижимы.
    if (n >= 3 + 4 + kIdLen && g_plain[0] == 0x56 /* 'V' */) {
        const uint8_t type = g_plain[2];
        const uint8_t* tx  = g_plain + 3;
        const uint8_t* who = g_plain + 7;

        constexpr uint8_t T_PING = 1, T_PONG = 2, T_FIND_NODE = 3, T_NODES = 4;
        if (type == T_PING || type == T_FIND_NODE) {
            uint8_t reply[3 + 4 + kIdLen + 1];
            size_t o = 0;
            reply[o++] = 0x56;          // та же метка
            reply[o++] = g_plain[1];    // та же версия, какую прислали
            reply[o++] = (type == T_PING) ? T_PONG : T_NODES;
            memcpy(reply + o, tx, 4); o += 4;              // тот же номер обращения
            memcpy(reply + o, g_myId, kIdLen); o += kIdLen;
            if (type == T_FIND_NODE) reply[o++] = 0;       // соседей не знаем — пусто

            // Отвечаем ключом СПРАШИВАВШЕГО: он вывел его из своего имени и только им
            // сможет наш ответ раскрыть.
            uint8_t rk[32];
            cloak::keyForNode(who, rk);
            const size_t sealed = cloak::seal(rk, reply, o, g_out, kNetBuf);
            if (sealed) {
                g_lan.beginPacket(IPAddress(fromIp), fromPort);
                g_lan.write(g_out, sealed);
                g_lan.endPacket();
            }
            return;
        }
    }

    // Мост в обратную сторону: пришедший из сети запрос знакомства уходит в эфир.
    // Так устройство в интернете достаёт того, кто слышен только по радио.
    if (n >= 8 && g_plain[0] == 'B' && g_plain[1] == 'R') {
        const size_t inner = (size_t(g_plain[6]) << 8) | g_plain[7];
        if (inner + 8 <= n && g_toRadio) {
            Serial.println("мост: запрос из сети уходит в эфир");
            g_toRadio(g_plain + 2, g_plain + 8, inner);
        }
        return;
    }

    // Отправитель узнаётся по адресу: связь уже установлена, и имя есть в списке.
    for (size_t i = 0; i < g_neighbourCount; ++i) {
        if (g_neighbours[i].ip == fromIp && g_neighbours[i].port == fromPort) {
            g_neighbours[i].lastSeenMs = millis();
            if (g_onFrame) g_onFrame(g_neighbours[i].id, g_plain, n);
            return;
        }
    }
}

}  // namespace

bool begin() {
    if (!WiFi.isConnected()) return false;
    if (!contacts::haveIdentity()) return false;   // без личности имя узла не вывести

    if (!g_rx)    g_rx    = static_cast<uint8_t*>(psram::alloc(kNetBuf));
    if (!g_plain) g_plain = static_cast<uint8_t*>(psram::alloc(kNetBuf));
    if (!g_out)   g_out   = static_cast<uint8_t*>(psram::alloc(kNetBuf));
    if (!g_rx || !g_plain || !g_out) return false;

    computeMyId();

    // Порт выбираем случайный из верхнего диапазона: постоянный номер был бы приметой,
    // по которой трафик легко выделить.
    g_port = uint16_t(40000 + (esp_random() % 20000));
    if (!g_lan.beginMulticast(IPAddress(239, 255, 77, 77), kLanPort)) return false;

    g_ready = true;
    g_lastAnnounce = 0;
    return true;
}

bool ready() { return g_ready; }
uint16_t localPort() { return g_port; }
const uint8_t* myId() { return g_myId; }

void pump() {
    if (!g_ready) return;

    // Приём
    int size = g_lan.parsePacket();
    while (size > 0) {
        const int n = g_lan.read(g_rx, kNetBuf);
        if (n > 0) {
            handlePacket(g_rx, size_t(n),
                         uint32_t(g_lan.remoteIP()), uint16_t(g_lan.remotePort()));
        }
        size = g_lan.parsePacket();
    }

    // Рассылка объявлений. Часто в начале — быстрое знакомство, реже потом — экономия
    // батареи: пока никого не нашли, каждая секунда ожидания заметна.
    const uint32_t now = millis();
    const uint32_t period = g_neighbourCount == 0 ? kAnnounceMs : kAnnounceMs * 6;
    if (now - g_lastAnnounce >= period) {
        g_lastAnnounce = now;
        announce();
    }
}

size_t neighbourCount() {
    // Считаем только свежих: устаревшая запись означала бы, что мы уверенно шлём в
    // никуда, а человек видит «в сети» у того, кто давно ушёл.
    size_t live = 0;
    const uint32_t now = millis();
    for (size_t i = 0; i < g_neighbourCount; ++i) {
        if (now - g_neighbours[i].lastSeenMs < kFreshMs) ++live;
    }
    return live;
}

const Neighbour* neighbourAt(size_t i) {
    return i < g_neighbourCount ? &g_neighbours[i] : nullptr;
}

bool sendTo(const uint8_t id[kIdLen], const uint8_t* data, size_t len) {
    if (!g_ready) return false;
    Neighbour* n = findNeighbour(id);
    if (!n || millis() - n->lastSeenMs > kFreshMs) return false;

    // Каждому узлу — свой ключ, выведенный из его имени. Посторонний, перехвативший
    // пакет, не отличит его от шума и не поймёт, кому он адресован.
    uint8_t key[32];
    cloak::keyForNode(id, key);
    const size_t sealed = cloak::seal(key, data, len, g_out, kNetBuf);
    if (sealed == 0) return false;

    g_lan.beginPacket(IPAddress(n->ip), n->port);
    g_lan.write(g_out, sealed);
    return g_lan.endPacket() == 1;
}

void setOnFrame(OnFrame cb) { g_onFrame = cb; }

void refreshExternalAddress() {
    if (!WiFi.isConnected()) return;

    // Запрос по стандарту STUN: заголовок из двадцати байт с известным числом-меткой,
    // ответ содержит наш адрес, каким его видит сервер.
    // Список общий — см. relays.h. Держать его в одном месте важно: расхождение между
    // платой и телефоном означало бы, что они спрашивают у разных серверов и получают
    // разные ответы о доступности.
    for (size_t i = 0; i < relays::kStunCount; ++i) {
        uint8_t req[20] = {};
        req[0] = 0x00; req[1] = 0x01;                 // запрос привязки
        req[4] = 0x21; req[5] = 0x12; req[6] = 0xA4; req[7] = 0x42;  // метка
        esp_fill_random(req + 8, 12);                 // случайный номер обращения

        g_stun.begin(0);
        g_stun.beginPacket(relays::kStun[i].host, relays::kStun[i].port);
        g_stun.write(req, sizeof(req));
        g_stun.endPacket();

        const uint32_t until = millis() + 1200;
        while (millis() < until) {
            const int n = g_stun.parsePacket();
            if (n <= 0) { delay(10); continue; }
            uint8_t resp[256];
            const int got = g_stun.read(resp, sizeof(resp));
            g_stun.stop();
            if (got < 20) break;

            // Разбор ответа: ищем поле с адресом. Оно закрыто той же меткой — так
            // стандарт обходит домашние маршрутизаторы, которые правят адреса на лету.
            size_t o = 20;
            while (o + 4 <= size_t(got)) {
                const uint16_t type = uint16_t((resp[o] << 8) | resp[o + 1]);
                const uint16_t alen = uint16_t((resp[o + 2] << 8) | resp[o + 3]);
                if (type == 0x0020 && alen >= 8) {    // адрес, закрытый меткой
                    const uint16_t port = uint16_t(((resp[o + 6] << 8) | resp[o + 7]) ^ 0x2112);
                    const uint8_t a = resp[o + 8] ^ 0x21, b = resp[o + 9] ^ 0x12;
                    const uint8_t c = resp[o + 10] ^ 0xA4, d = resp[o + 11] ^ 0x42;
                    snprintf(g_external, sizeof(g_external), "%u.%u.%u.%u:%u", a, b, c, d, port);
                    Serial.printf("внешний адрес: %s\n", g_external);
                    return;
                }
                o += 4 + ((alen + 3) & ~3u);
            }
            break;
        }
        g_stun.stop();
    }
}

const char* externalAddress() { return g_external; }

// ── мост ───────────────────────────────────────────────────────────────────────────────

bool bridgeAvailable() { return g_ready && neighbourCount() > 0; }

bool bridgeToInternet(const uint8_t meetAddr[4], const uint8_t* payload, size_t len) {
    if (!bridgeAvailable()) return false;

    // Запрос знакомства пересылаем ВСЕМ видимым в сети: мы не знаем, кто из них окажется
    // тем самым собеседником, а лишний запрос безвреден — не тот просто не расшифрует.
    uint8_t pkt[512];
    if (len + 8 > sizeof(pkt)) return false;
    pkt[0] = 'B'; pkt[1] = 'R';                  // признак моста
    memcpy(pkt + 2, meetAddr, 4);
    pkt[6] = uint8_t(len >> 8); pkt[7] = uint8_t(len & 0xFF);
    memcpy(pkt + 8, payload, len);

    bool any = false;
    const uint32_t now = millis();
    for (size_t i = 0; i < g_neighbourCount; ++i) {
        if (now - g_neighbours[i].lastSeenMs > kFreshMs) continue;
        if (sendTo(g_neighbours[i].id, pkt, len + 8)) any = true;
    }
    if (any) Serial.printf("мост: запрос знакомства ушёл в сеть\n");
    return any;
}

void setBridgeToRadio(void (*cb)(const uint8_t[4], const uint8_t*, size_t)) {
    g_toRadio = cb;
}

}  // namespace net
