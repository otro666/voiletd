#include "ws.h"

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <string.h>
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>

namespace ws {

namespace {

/** Постоянная из описания протокола. Сервер приклеивает её к нашему признаку и
 *  возвращает хеш — так обе стороны убеждаются, что говорят на одном языке. */
constexpr const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

void randomKey(char* out, size_t cap) {
    uint8_t raw[16];
    esp_fill_random(raw, sizeof(raw));
    size_t n = 0;
    mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out), cap, &n, raw, sizeof(raw));
    out[n] = 0;
}

/** Ожидаемый ответ сервера — по нему проверяем, что это действительно веб-сокет, а не
 *  что-то, случайно ответившее на нашем порту. */
void expectedAccept(const char* key, char* out, size_t cap) {
    char joined[96];
    snprintf(joined, sizeof(joined), "%s%s", key, kGuid);
    uint8_t sha[20];
    mbedtls_sha1(reinterpret_cast<const uint8_t*>(joined), strlen(joined), sha);
    size_t n = 0;
    mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out), cap, &n, sha, sizeof(sha));
    out[n] = 0;
}

/**
 * Отправить кадр.
 *
 * Маска обязательна: по протоколу всё, что идёт ОТ клиента, накладывается на случайное
 * число. Без неё сервер обязан разорвать соединение — и разрывает.
 */
bool sendFrame(Conn& c, uint8_t opcode, const uint8_t* data, size_t len) {
    if (c.state != OPEN || !c.cli || !c.cli->connected()) return false;

    uint8_t hdr[14];
    size_t h = 0;
    hdr[h++] = uint8_t(0x80 | opcode);          // последний кадр сообщения

    // Длина: короткая, средняя или длинная — три вида записи, как в протоколе.
    if (len < 126) {
        hdr[h++] = uint8_t(0x80 | len);
    } else if (len < 65536) {
        hdr[h++] = 0x80 | 126;
        hdr[h++] = uint8_t(len >> 8); hdr[h++] = uint8_t(len);
    } else {
        hdr[h++] = 0x80 | 127;
        for (int i = 7; i >= 0; --i) hdr[h++] = uint8_t((uint64_t(len) >> (i * 8)) & 0xFF);
    }

    uint8_t mask[4];
    esp_fill_random(mask, sizeof(mask));
    memcpy(hdr + h, mask, 4); h += 4;

    if (c.cli->write(hdr, h) != h) return false;

    // Накладываем маску порциями: копия всего сообщения ради этого была бы лишней тратой
    // памяти, которой на плате мало.
    uint8_t chunk[128];
    size_t off = 0;
    while (off < len) {
        const size_t n = (len - off) < sizeof(chunk) ? (len - off) : sizeof(chunk);
        for (size_t i = 0; i < n; ++i) chunk[i] = data[off + i] ^ mask[(off + i) & 3];
        if (c.cli->write(chunk, n) != n) return false;
        off += n;
    }
    return true;
}

}  // namespace

bool open(Conn& c, WiFiClientSecure& cli, const char* host, uint16_t port,
          const char* path, const char* protocol) {
    close(c);
    c.cli = &cli;
    c.state = CONNECTING;

    // Сертификат не проверяем: узлы публичные, содержимое защищено сверху своим слоем,
    // а хранилище сертификатов на плате пришлось бы обновлять руками.
    cli.setInsecure();
    cli.setTimeout(4);
    if (!cli.connect(host, port)) { c.state = CLOSED; return false; }

    randomKey(c.key, sizeof(c.key));

    char req[420];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n",
        path && *path ? path : "/", host, c.key);
    if (protocol && *protocol) {
        n += snprintf(req + n, sizeof(req) - n, "Sec-WebSocket-Protocol: %s\r\n", protocol);
    }
    n += snprintf(req + n, sizeof(req) - n, "\r\n");
    cli.write(reinterpret_cast<const uint8_t*>(req), size_t(n));

    // Ответ: ищем строку подтверждения и сверяем её со своим ожиданием.
    char want[40];
    expectedAccept(c.key, want, sizeof(want));

    bool okStatus = false, okAccept = false;
    const uint32_t until = millis() + 5000;
    while (millis() < until) {
        if (!cli.connected()) break;
        if (!cli.available()) { delay(10); continue; }

        const String line = cli.readStringUntil('\n');
        if (line.length() <= 1) {                 // пустая строка — конец заголовков
            if (okStatus && okAccept) { c.state = OPEN; c.lastPing = millis(); return true; }
            break;
        }
        if (line.startsWith("HTTP/1.1 101")) okStatus = true;
        else if (line.startsWith("Sec-WebSocket-Accept:") ||
                 line.startsWith("sec-websocket-accept:")) {
            okAccept = line.indexOf(want) > 0;
        }
    }

    close(c);
    return false;
}

bool sendText(Conn& c, const char* text, size_t len) {
    return sendFrame(c, 0x1, reinterpret_cast<const uint8_t*>(text), len);
}

bool sendBinary(Conn& c, const uint8_t* data, size_t len) {
    return sendFrame(c, 0x2, data, len);
}

size_t receive(Conn& c, uint8_t* out, size_t outCap, bool* isBinaryOut) {
    if (c.state != OPEN || !c.cli) return 0;
    if (!c.cli->connected()) { close(c); return 0; }
    if (c.cli->available() < 2) return 0;

    const uint8_t b0 = uint8_t(c.cli->read());
    const uint8_t b1 = uint8_t(c.cli->read());
    const uint8_t opcode = b0 & 0x0F;
    size_t len = b1 & 0x7F;

    if (len == 126) {
        if (c.cli->available() < 2) return 0;
        len = size_t(c.cli->read()) << 8;
        len |= size_t(c.cli->read());
    } else if (len == 127) {
        if (c.cli->available() < 8) return 0;
        uint64_t big = 0;
        for (int i = 0; i < 8; ++i) big = (big << 8) | uint64_t(c.cli->read());
        len = size_t(big);
    }

    // От сервера маски быть не должно, но если пришла — читаем и учитываем.
    uint8_t mask[4] = {};
    const bool masked = (b1 & 0x80) != 0;
    if (masked) {
        for (int i = 0; i < 4; ++i) mask[i] = uint8_t(c.cli->read());
    }

    if (len > outCap) {
        // Не помещается — вычитываем и выбрасываем, иначе следующий кадр разберётся
        // неверно: в потоке останется хвост этого.
        for (size_t i = 0; i < len; ++i) c.cli->read();
        return 0;
    }

    size_t got = 0;
    const uint32_t until = millis() + 2000;
    while (got < len && millis() < until) {
        const int r = c.cli->read(out + got, len - got);
        if (r > 0) got += size_t(r);
        else delay(1);
    }
    if (got < len) return 0;
    if (masked) for (size_t i = 0; i < len; ++i) out[i] ^= mask[i & 3];

    switch (opcode) {
    case 0x9:                                  // проверка связи — отвечаем сразу
        sendFrame(c, 0xA, out, len);
        return 0;
    case 0xA:                                  // отклик на нашу проверку
        return 0;
    case 0x8:                                  // сервер закрывает
        close(c);
        return 0;
    default: break;
    }

    if (isBinaryOut) *isBinaryOut = (opcode == 0x2);
    return len;
}

void keepAlive(Conn& c) {
    if (c.state != OPEN) return;
    if (millis() - c.lastPing < 30000) return;
    c.lastPing = millis();
    sendFrame(c, 0x9, nullptr, 0);
}

void close(Conn& c) {
    if (c.cli && c.cli->connected()) c.cli->stop();
    c.state = CLOSED;
    c.cli = nullptr;
}

}  // namespace ws
