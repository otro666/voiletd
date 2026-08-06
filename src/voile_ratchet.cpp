// Двойной храповик. Код общий: на плате под ним лежит mbedTLS, на компьютере — тестовая
// реализация тех же функций. Благодаря этому логика проверяется настоящими тестами до
// заливки в железо, а не «на глаз».
#include "voile_crypto.h"

#include <string.h>

namespace voile {

// Метки вывода ключей. Обязаны совпадать с телефонной версией побайтово — иначе стороны
// выведут разные ключи и не расшифруют друг друга.
static const char kRootInfo[]  = "Voile root";
static const char kChainMsg[]  = "\x01";
static const char kChainNext[] = "\x02";

// Шаг цепочки: из ключа цепочки получаем ключ сообщения и следующий ключ цепочки.
static void kdfChain(const uint8_t ck[kKeyLen], uint8_t nextCk[kKeyLen],
                     uint8_t msgKey[kKeyLen]) {
    hkdf(ck, kKeyLen, nullptr, 0,
         reinterpret_cast<const uint8_t*>(kChainNext), 1, nextCk, kKeyLen);
    hkdf(ck, kKeyLen, nullptr, 0,
         reinterpret_cast<const uint8_t*>(kChainMsg), 1, msgKey, kKeyLen);
}

// Шаг корня: общий секрет от нового обмена ключами даёт новый корень и новую цепочку.
static void kdfRoot(const uint8_t root[kKeyLen], const uint8_t dh[kSharedLen],
                    uint8_t newRoot[kKeyLen], uint8_t newChain[kKeyLen]) {
    uint8_t out[kKeyLen * 2];
    hkdf(dh, kSharedLen, root, kKeyLen,
         reinterpret_cast<const uint8_t*>(kRootInfo), sizeof(kRootInfo) - 1,
         out, sizeof(out));
    memcpy(newRoot,  out,            kKeyLen);
    memcpy(newChain, out + kKeyLen,  kKeyLen);
}

bool Ratchet::initInitiator(const uint8_t root[kKeyLen], const uint8_t* ad, size_t adLen,
                            const uint8_t peerPubComp[kPubComp]) {
    memcpy(root_, root, kKeyLen);
    adLen_ = adLen > sizeof(ad_) ? sizeof(ad_) : adLen;
    memcpy(ad_, ad, adLen_);
    memcpy(peerPub_, peerPubComp, kPubComp);
    hasPeer_ = true;
    return stepSend();
}

bool Ratchet::initResponder(const uint8_t root[kKeyLen], const uint8_t* ad, size_t adLen,
                            const KeyPair& own) {
    memcpy(root_, root, kKeyLen);
    adLen_ = adLen > sizeof(ad_) ? sizeof(ad_) : adLen;
    memcpy(ad_, ad, adLen_);
    own_ = own;
    // Цепочки отправки пока нет: она появится после первого принятого сообщения. Это
    // норма алгоритма, а не сбой — отвечающая сторона до ответа шифровать не может.
    hasSendChain_ = false;
    return true;
}

bool Ratchet::stepSend() {
    if (!hasPeer_) return false;
    KeyPair fresh;
    if (!genKeyPair(fresh)) return false;
    uint8_t dh[kSharedLen];
    if (!ecdh(fresh.priv, peerPub_, dh)) return false;

    uint8_t newRoot[kKeyLen], newChain[kKeyLen];
    kdfRoot(root_, dh, newRoot, newChain);
    memcpy(root_, newRoot, kKeyLen);
    memcpy(ckSend_, newChain, kKeyLen);
    own_ = fresh;
    hasSendChain_ = true;
    pubPending_ = true;      // собеседник ещё не знает нового ключа — вложим в кадр
    ns_ = 0;
    return true;
}

bool Ratchet::stepRecv(const uint8_t newPub[kPubComp]) {
    uint8_t dh[kSharedLen];
    if (!ecdh(own_.priv, newPub, dh)) return false;

    uint8_t newRoot[kKeyLen], newChain[kKeyLen];
    kdfRoot(root_, dh, newRoot, newChain);
    memcpy(root_, newRoot, kKeyLen);
    memcpy(ckRecv_, newChain, kKeyLen);
    memcpy(peerPub_, newPub, kPubComp);
    hasPeer_ = true;
    hasRecvChain_ = true;
    nr_ = 0;
    // Собеседник сменил цепочку — при следующей отправке сменим и свою.
    hasSendChain_ = false;
    return true;
}

bool Ratchet::nextSendKey(uint8_t out[kKeyLen], uint16_t& counter, bool& needKey,
                          uint8_t pubOut[kPubComp]) {
    if (!hasSendChain_) {
        if (!stepSend()) return false;
    }
    uint8_t next[kKeyLen];
    kdfChain(ckSend_, next, out);
    memcpy(ckSend_, next, kKeyLen);
    counter = ns_++;

    // Ключ вкладываем только пока собеседник его не подтвердил приёмом. В телефонной
    // версии он идёт с каждым сообщением, но там нет предела в 255 байт на пакет.
    needKey = pubPending_;
    if (needKey) memcpy(pubOut, own_.pubComp, kPubComp);
    return true;
}

bool Ratchet::recvKey(const uint8_t* newPub, uint16_t counter, uint8_t out[kKeyLen]) {
    // Собеседник прислал новый ключ — начинаем новую принимающую цепочку.
    if (newPub && (!hasRecvChain_ || memcmp(newPub, peerPub_, kPubComp) != 0)) {
        if (!stepRecv(newPub)) return false;
    }
    if (!hasRecvChain_) return false;

    // Сообщение из уже пропущенных: копии приходят не по порядку, и без этого выпавшее
    // сообщение не расшифровать никогда.
    for (size_t i = 0; i < kSkipMax; ++i) {
        if (skipped_[i].used && skipped_[i].n == counter) {
            memcpy(out, skipped_[i].key, kKeyLen);
            skipped_[i].used = false;
            return true;
        }
    }

    if (counter < nr_) return false;              // уже прошли и не сохранили
    // Догоняем цепочку, откладывая ключи пропущенных сообщений.
    while (nr_ < counter) {
        uint8_t next[kKeyLen], mk[kKeyLen];
        kdfChain(ckRecv_, next, mk);
        memcpy(ckRecv_, next, kKeyLen);
        skipped_[skipNext_] = Skipped{nr_, {}, true};
        memcpy(skipped_[skipNext_].key, mk, kKeyLen);
        skipNext_ = (skipNext_ + 1) % kSkipMax;
        ++nr_;
    }
    uint8_t next[kKeyLen];
    kdfChain(ckRecv_, next, out);
    memcpy(ckRecv_, next, kKeyLen);
    ++nr_;
    // Встречное сообщение — тоже доказательство, что наш ключ дошёл: собеседник смог
    // построить свою цепочку, а значит наш ключ у него есть.
    pubPending_ = false;
    return true;
}

}  // namespace voile
