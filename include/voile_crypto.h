// Криптография Voile для ESP32-S3.
//
// Совместимость с телефонной версией — обязательное требование: те же кривая, вывод
// ключей и шифр, чтобы T-Deck и телефон понимали друг друга напрямую. Всё это есть в
// mbedTLS, который идёт в составе ESP-IDF, а AES и SHA у S3 аппаратно ускорены.
//
// Отличие от Wi-Fi-профиля одно: тег AEAD укорочен до 8 байт (см. voile_frame.h) и точка
// кривой передаётся в сжатом виде — 33 байта вместо 65.
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace voile {

constexpr size_t kKeyLen    = 32;   // AES-256 и ключи цепочки
constexpr size_t kSharedLen = 32;   // общий секрет ECDH (координата X)
constexpr size_t kPubRaw    = 65;   // несжатая точка P-256
constexpr size_t kPubComp   = 33;   // сжатая точка

// ── пара ключей ────────────────────────────────────────────────────────────────────────

struct KeyPair {
    uint8_t priv[32];
    uint8_t pubComp[kPubComp];      // хранится сразу сжатой: в эфир идёт именно она
};

/** Сгенерировать эфемерную пару на P-256. */
bool genKeyPair(KeyPair& out);

/** Общий секрет: ECDH со сжатой точкой собеседника. */
bool ecdh(const uint8_t priv[32], const uint8_t peerPubComp[kPubComp],
          uint8_t out[kSharedLen]);

// ── вывод ключей ───────────────────────────────────────────────────────────────────────

/**
 * HKDF на SHA-256 — тот же, что в телефонной версии.
 * Метки обязаны совпадать побайтово, иначе ключи разъедутся.
 */
void hkdf(const uint8_t* secret, size_t secretLen,
          const uint8_t* salt,   size_t saltLen,
          const uint8_t* info,   size_t infoLen,
          uint8_t* out, size_t outLen);

// ── шифрование кадра ───────────────────────────────────────────────────────────────────

/**
 * AES-256-GCM с укороченным тегом.
 *
 * Одноразовый вектор выводится из номера сообщения и номера части, а не хранится в кадре:
 * это экономит 12 байт на каждом пакете, а восстановить его получатель может из заголовка,
 * который и так передаётся.
 */
bool seal(const uint8_t key[kKeyLen], uint64_t nonceCounter,
          const uint8_t* aad, size_t aadLen,
          const uint8_t* plain, size_t plainLen,
          uint8_t* out, uint8_t tag[8]);

bool open(const uint8_t key[kKeyLen], uint64_t nonceCounter,
          const uint8_t* aad, size_t aadLen,
          const uint8_t* cipher, size_t cipherLen,
          const uint8_t tag[8], uint8_t* out);

// ── двойной храповик ───────────────────────────────────────────────────────────────────

/**
 * Свой ключ на каждое сообщение, старые уничтожаются сразу.
 *
 * Смысл тот же, что в телефонной версии: изъятое устройство не раскрывает уже отправленное,
 * а после утечки ключей связь самозаживает, как только собеседник ответит.
 *
 * Отличие от Wi-Fi-профиля: новый публичный ключ уходит в эфир НЕ с каждым сообщением, а
 * только при смене цепочки (кадр FT_MSG_KEY). Иначе 33 байта на каждом пакете при пределе
 * в 255 — непозволительная роскошь.
 */
class Ratchet {
public:
    /** Сторона, которая шлёт первой. */
    bool initInitiator(const uint8_t root[kKeyLen], const uint8_t* ad, size_t adLen,
                       const uint8_t peerPubComp[kPubComp]);

    /** Отвечающая сторона: до первого принятого сообщения шифровать не может — так
     *  устроен алгоритм, и это штатное состояние, а не сбой. */
    bool initResponder(const uint8_t root[kKeyLen], const uint8_t* ad, size_t adLen,
                       const KeyPair& own);

    bool canSend() const { return hasSendChain_; }

    /**
     * Взять ключ для следующего исходящего сообщения.
     * needKey получает true, если в этом кадре надо передать новый публичный ключ.
     */
    bool nextSendKey(uint8_t out[kKeyLen], uint16_t& counter, bool& needKey,
                     uint8_t pubOut[kPubComp]);

    /** Ключ для принятого сообщения. newPub != nullptr — собеседник сменил цепочку. */
    bool recvKey(const uint8_t* newPub, uint16_t counter, uint8_t out[kKeyLen]);

    /**
     * Собеседник подтвердил приём кадра с нашим новым ключом — вкладывать его больше
     * не нужно.
     *
     * Без явного подтверждения ключ пришлось бы вкладывать в КАЖДЫЙ кадр: пока мы не
     * знаем, что он дошёл, отказ от него означал бы, что собеседник не расшифрует уже
     * ничего. Тридцать три байта на каждом пакете при пределе в 255 — непозволительно,
     * поэтому подтверждение здесь не украшение, а необходимость.
     */
    void keyAcked() { pubPending_ = false; }

private:
    bool stepSend();
    bool stepRecv(const uint8_t newPub[kPubComp]);

    uint8_t root_[kKeyLen]  = {};
    uint8_t ckSend_[kKeyLen] = {};
    uint8_t ckRecv_[kKeyLen] = {};
    uint8_t ad_[64]  = {};
    size_t  adLen_   = 0;
    KeyPair own_{};
    uint8_t peerPub_[kPubComp] = {};
    bool hasSendChain_ = false;
    bool hasRecvChain_ = false;
    bool hasPeer_      = false;
    bool pubPending_   = false;   // новый ключ ещё не отправлен собеседнику
    uint16_t ns_ = 0;
    uint16_t nr_ = 0;

    // Пропущенные ключи: копии сообщения приходят не по порядку, часть теряется —
    // без этого выпавшее сообщение уже не расшифровать никогда.
    static constexpr size_t kSkipMax = 32;
    struct Skipped { uint16_t n; uint8_t key[kKeyLen]; bool used; };
    Skipped skipped_[kSkipMax] = {};
    size_t  skipNext_ = 0;
};

}  // namespace voile
