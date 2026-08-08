// Вуаль на LilyGo T-Deck — первая версия: обмен текстом по LoRa с шифрованием Voile.
//
// Что делает: знакомство по кодовому слову, переписка, разнесение копий во времени и по
// частоте, восстановление потерь, двойной храповик.
//
// Чего НЕ делает: звонков и кружков (камеры на плате нет физически), рельс через
// интернет, DHT. Всё это следующие этапы, см. docs/plan-tdeck.md.

#include <Arduino.h>
#include <RadioLib.h>
#include <Wire.h>
#include <WiFi.h>
#include <time.h>
#include <SD.h>

#include "board.h"
#include "voile_frame.h"
#include "voile_diversity.h"
#include "voile_crypto.h"
#include "ui.h"
#include "audio.h"
#include "net.h"
#include "rail.h"
#include "nostr.h"
#include "tracker.h"
#include "xfer.h"
#include "store_sd.h"
#include "emoji.h"
#include "display.h"
#include "boot.h"
#include "input.h"
#include "contacts.h"

// ── радио ──────────────────────────────────────────────────────────────────────────────
// Одна шина на экран, карту и радио — и ОДИН аппаратный блок.
//
// Отдельный блок на тех же выводах создавать нельзя: выводы перенаправляются к нему, и
// экран, уже настроенный на другой блок, замолкает. Берём общий объект SPI — тот же,
// которым пользуется библиотека экрана, а очередь между устройствами обеспечивают
// выводы выбора.
static SPIClass& radioSpi = SPI;
static SX1262 radio = new Module(BOARD_LORA_CS, BOARD_LORA_IRQ,
                                 BOARD_LORA_RST, BOARD_LORA_BUSY, radioSpi);

// Флаг из обработчика прерывания: внутри него ничего тяжёлого делать нельзя, только
// отметить событие и выйти.
static volatile bool packetReady = false;
/** Радио подтвердило, что оно на месте. Без этого не передаём НИЧЕГО: слепая передача
 *  при неверной распиновке или отсутствующем модуле выжигает усилитель. */
static bool radioReady = false;
/** Отсев повторов: каждый кадр идёт тремя копиями, и без этого одно сообщение
 *  показывалось бы трижды. */

/**
 * ОДИН номер кадра на всё устройство.
 *
 * Раньше счётчиков было три — у объявления, у сообщения и у моста, — и каждый начинался
 * с нуля. Получатель отсеивает повторы по паре «отправитель и номер», поэтому сообщение
 * с номером 0 выбрасывалось как копия объявления с тем же номером. Одна сторона видела
 * другую, обратно почти ничего не доходило.
 *
 * Начинаем со СЛУЧАЙНОГО места, а не с нуля: после перезапуска счётчик пошёл бы заново,
 * а у собеседника прежние номера ещё помнятся — и всё новое опять отбрасывалось бы.
 */
static uint16_t nextSeq() {
    static uint16_t seq = 0;
    static bool init = false;
    if (!init) { seq = uint16_t(esp_random()); init = true; }
    return seq++;
}
// IRAM_ATTR, а не ICACHE_RAM_ATTR: второе — наследие ESP8266, и у ESP32 работает лишь
// как псевдоним, который в новых версиях фреймворка может исчезнуть. Обработчик обязан
// лежать в быстрой памяти: во время записи во флеш код из неё недоступен, и прерывание
// из флеша уронило бы плату.
static void IRAM_ATTR onRadioIrq() { packetReady = true; }

// ── очередь отправки с разнесением ─────────────────────────────────────────────────────
//
// Каждое сообщение уходит несколькими копиями, разнесёнными во времени и по частоте.
// Это главное средство против городских переотражений: три копии дают тот же эффект,
// что прибавка 14 дБ мощности, которую взять неоткуда.
struct Pending {
    uint8_t  data[voile::kMaxPacket];
    size_t   len;
    uint8_t  copiesLeft;
    uint8_t  copyIndex;
    uint32_t nextAt;      // время следующей копии, мс от старта
    bool     used;
};

static constexpr size_t kQueueSize = 8;
static Pending queue_[kQueueSize];

static voile::SeenCache seen_;

/** Поставить кадр в очередь на отправку со всеми копиями. */
static bool enqueue(const uint8_t* data, size_t len, uint8_t copies) {
    for (size_t i = 0; i < kQueueSize; ++i) {
        if (queue_[i].used) continue;
        memcpy(queue_[i].data, data, len);
        queue_[i].len        = len;
        queue_[i].copiesLeft = copies;
        queue_[i].copyIndex  = 0;
        queue_[i].nextAt     = millis();
        queue_[i].used       = true;
        return true;
    }
    return false;   // очередь полна — сообщение не принято, о чём надо сказать человеку
}

/** Сколько слотов очереди свободно. Куски файлов ограничиваются по этому числу:
 *  займи они всё — тексты и подтверждения выбрасывались бы молча всю перекачку. */
static size_t queueFree() {
    size_t n = 0;
    for (size_t i = 0; i < kQueueSize; ++i) if (!queue_[i].used) ++n;
    return n;
}

// ── учёт эфирного времени ──────────────────────────────────────────────────────────────
//
// Счётчик эфирного времени оставлен только для сведения — ОГРАНИЧЕНИЯ НЕТ.
//
// Раньше передача переставала работать при исчерпании доли, и это выглядело как поломка:
// сообщения молча переставали уходить, а причина была не видна. Решение о том, укладываться
// ли в чью-то долю, принимает владелец устройства, а не прошивка.
static uint32_t airtimeUsedMs = 0;

// ── отправка ───────────────────────────────────────────────────────────────────────────

/**
 * Идёт ли передача прямо сейчас.
 *
 * ПОЧЕМУ ЭТО ВАЖНО. На нынешних настройках (125 кГц, SF9) полный кадр занимает эфир
 * меньше секунды, но настройки могут ужесточаться, и на узкой полосе тот же кадр
 * растягивается до минуты. Передача обязана оставаться неблокирующей при любых настройках.
 *
 * Раньше передача была блокирующей: всё это время плата стояла мёртвой — не рисовала,
 * не читала сенсор, не отвечала на клавиатуру. Отсюда и «всё висит»: устройство не
 * зависало, оно честно передавало, просто ничего больше не могло.
 *
 * Теперь передача идёт сама, а мы лишь проверяем, закончилась ли она.
 */
static volatile bool txBusy = false;
static uint32_t txStarted = 0;

/** Слот очереди, чья копия сейчас в эфире. Нужен, чтобы назначить время СЛЕДУЮЩЕЙ копии
 *  от конца передачи, а не от начала: раньше задержка (4–11 с) истекала прямо во время
 *  передачи, которая длиннее её самой, и копии уходили встык — разнесение во времени
 *  не работало вовсе. */
static int txSlot = -1;

/** «Ждём конца передачи»: с таким значением nextAt слот не трогается, пока checkTxDone
 *  не назначит настоящий срок. */
static constexpr uint32_t kWaitTxEnd = 0xFFFFFFFFu;

/** Начать передачу копии. Возвращает false, если радио занято. */
static bool sendCopy(Pending& p) {
    if (!radioReady || txBusy) return false;
    // Принятый, но ещё не разобранный кадр важнее нашей передачи: старт передачи
    // затирает приёмный буфер модуля, и кадр пропадает молча. Отдаём круг приёму.
    if (packetReady) return false;

    // Все копии идут на ОДНОЙ частоте. Раньше копии смещались на ±250 кГц, но приёмник
    // всегда слушает только основную частоту — смещённые копии физически некому было
    // принять, а эфирное время они занимали. Смещения убраны и здесь, и в
    // voile::copyFreqOffsetKhz; частоту перед передачей не трогаем вовсе.

    const int st = radio.startTransmit(p.data, p.len);
    if (st != RADIOLIB_ERR_NONE) {
        ets_printf("[vual] передача не началась: %d\n", st);
        radio.startReceive();
        return false;
    }
    // Маячок каждого ухода в эфир: по парным логам двух плат видно, вышел ли кадр
    // и услышан ли — спор «уходят непонятно куда» решается строками, а не верой.
    ets_printf("[vual] -> эфир: тип %u, номер %u, копия %u, %u байт\n",
               unsigned(p.data[0]), unsigned(p.data[5] | (p.data[6] << 8)),
               unsigned(p.copyIndex), unsigned(p.len));
    txBusy = true;
    txStarted = millis();
    return true;
}

/**
 * Закончилась ли передача. Зовётся из общего цикла.
 *
 * Признак ставит то же прерывание, что сообщает о приёме: у модуля это одна линия на оба
 * события, и различаем их по тому, передавали мы или слушали.
 */
static void checkTxDone() {
    if (!txBusy) return;

    // Предел на всякий случай: если прерывание почему-то не пришло, радио осталось бы
    // занятым навсегда и передача встала бы совсем. Берём с запасом от расчётного времени.
    const bool timeout = millis() - txStarted > 90000;

    if (packetReady || timeout) {
        packetReady = false;
        txBusy = false;
        airtimeUsedMs += millis() - txStarted;
        if (timeout) ets_printf("[vual] передача не завершилась вовремя — сброс\n");
        else ets_printf("[vual] передано за %lu мс\n",
                        (unsigned long)(millis() - txStarted));
        radio.finishTransmit();
        // Вернуться в приём обязательно: без этого следующий пакет будет потерян.
        radio.startReceive();

        // Только теперь, от КОНЦА передачи, назначаем срок следующей копии этого кадра.
        if (txSlot >= 0 && size_t(txSlot) < kQueueSize) {
            Pending& p = queue_[txSlot];
            if (p.used && p.nextAt == kWaitTxEnd) {
                p.nextAt = millis() +
                           voile::copyDelayMs(p.copyIndex, uint32_t(txSlot * 7919));
            }
        }
        txSlot = -1;
    }
}

static void pumpQueue() {
    // Радио занято передачей — ждём. Одна передача длится десятки секунд, и лезть в
    // модуль в это время бессмысленно: он всё равно откажет.
    if (txBusy) return;

    const uint32_t now = millis();
    for (size_t i = 0; i < kQueueSize; ++i) {
        Pending& p = queue_[i];
        if (!p.used || now < p.nextAt) continue;

        // Не удалось начать — оставляем в очереди и пробуем следующим кругом. Списывать
        // копию, которая не ушла, значило бы терять её молча.
        if (!sendCopy(p)) return;
        if (--p.copiesLeft == 0) { p.used = false; continue; }
        ++p.copyIndex;
        // Промежуток обязан быть достаточным, чтобы картина переотражений успела
        // измениться. Отсчитывать его надо от КОНЦА передачи, а передача ещё идёт —
        // поэтому здесь ставим метку «ждём конца», а срок назначит checkTxDone.
        txSlot = int(i);
        p.nextAt = kWaitTxEnd;
    }
}

// ── приём ──────────────────────────────────────────────────────────────────────────────

// Объявлены заранее: мост вызывается из приёма радио, а сами функции ниже — им нужны
// очередь отправки и состояние сети.
// ── состояние экрана ───────────────────────────────────────────────────────────────────

static ui::Screen screen_ = ui::SCR_CHATS;
static size_t     selected_ = 0;
/** Непрочитанное по собеседникам. Живёт рядом с интерфейсом, а не в контактах:
 *  это состояние экрана, и переживать перезагрузку ему незачем. */
static uint8_t    unread_[17] = {};
/** Отложенный звонок входящего: путь приёма только взводит, звенит главный цикл. */
static volatile bool chimePending_ = false;
// Первым выделен пункт личности: без неё ничего остального не работает, и человек
// должен упереться в неё сразу.
static ui::MenuItem menuSel_ = ui::MENU_IDENTITY;
/** Кодовое слово, которое человек набирает на экране знакомства. */
static char       phrase_[64] = {};
static size_t     phraseLen_ = 0;
static char       myName_[24] = {};
/** Комната текущего знакомства и ключ, которым закрыт обмен в ней. */
static char       pairRoom_[41] = {};
/** Журнал для показа: строки и буфер под них. */
static char       logBuf_[3000] = {};
static const char* logLines_[64] = {};
static size_t     logCount_ = 0;
static size_t     logFrom_ = 0;
static size_t     logPage_ = 0;
static bool       logHasMore_ = false;
static uint8_t    pairWrap_[32] = {};
/** Поправка сенсора. Хранится на карте: спрашивать её при каждом включении — издевательство. */
static uint16_t   touchCal_[8] = {};
static bool       haveCal_ = false;
/** Внешний адрес ещё не спрошен. Спрашиваем из общего цикла, а не при подключении:
 *  опрос семи серверов занимает секунды, и в момент подключения он не нужен. */
static bool       needExternal_ = false;

static void bridgeNetToRadio(const uint8_t meetAddr[4], const uint8_t* payload, size_t len);
static void announceRadio();
static void refreshPeers();
static void refreshPeersIfChanged();
static int  contactIndex(contacts::Contact* c);
static void deliverText(contacts::Contact* c, const char* who, const char* text);
static void bridgeRadioToNet(const uint8_t meetAddr[4], const uint8_t* payload, size_t len);

/**
 * Разбор принятого кадра. ВОЗВРАТ В ПРИЁМ ГАРАНТИРУЕТ ОБЁРТКА НИЖЕ — здесь о нём
 * думать не нужно, и ни одна ветка больше не обязана помнить про startReceive.
 *
 * История ошибки, ради которой это переделано. Радио полудуплексное: после чтения
 * кадра его надо явно вернуть в приём. Раньше это происходило «само» — плата отвечала
 * объявлением на КАЖДОЕ услышанное объявление, и конец ответной передачи возвращал
 * приём. Когда ответ остался только для НОВЫХ собеседников, ветка объявления от
 * знакомого стала завершаться голым return — и плата ГЛОХЛА до своего следующего
 * объявления, до двух минут. Доставка превратилась в лотерею окон глухоты, а заплатки
 * в отдельных ветках (подтверждения, куски файлов) были войной с симптомами.
 * Правило одно: обещание уровня «мы всегда слушаем» держит ОДНО место, а не дисциплина
 * каждой ветки.
 */
static void handlePacketInner() {
    uint8_t buf[voile::kMaxPacket];
    const int len = radio.getPacketLength();
    if (len <= 0 || size_t(len) > sizeof(buf)) { radio.startReceive(); return; }
    if (radio.readData(buf, len) != RADIOLIB_ERR_NONE) { radio.startReceive(); return; }

    voile::Header h;
    if (!voile::readHeader(buf, size_t(len), h)) { radio.startReceive(); return; }

    ets_printf("[vual] <- эфир: тип %u, номер %u, %d дБм\n",
               unsigned(h.type), unsigned(h.seq), int(radio.getRSSI()));

    // Копии одного сообщения приходят несколько раз — принять надо ровно один.
    if (seen_.seen(h.src, h.seq)) {
        ets_printf("[vual]    повтор — отсеян\n");
        radio.startReceive();
        return;
    }

    ui::Status st{};
    st.rssi = int(radio.getRSSI());
    st.snr  = radio.getSNR();
    st.linkUp = true;
    ui::drawStatus(st);

    // ── объявление из эфира ───────────────────────────────────────────────────────────
    if (h.type == voile::FT_HELLO) {
        const uint8_t* body = buf + voile::kHdrLen;
        const size_t bodyLen = size_t(len) - voile::kHdrLen;

        // Зовут ли нас? Либо это адрес встречи нашей фразы, либо объявление всем.
        bool forUs = (h.dst[0] == 0xFF && h.dst[1] == 0xFF &&
                      h.dst[2] == 0xFF && h.dst[3] == 0xFF);
        if (!forUs && pairRoom_[0] && contacts::haveIdentity()) {
            contacts::Rendezvous rv;
            contacts::deriveRendezvous(phrase_, rv);
            forUs = memcmp(h.dst, rv.meetAddr, 4) == 0;
        }

        if (forUs && bodyLen >= 1) {
            // Разбираем имя и ключ. Ключ и есть собеседник: по нему мы его узнаём и
            // с ним же считаем общий секрет.
            const uint8_t nameLen = body[0];
            if (size_t(1 + nameLen + voile::kPubComp) <= bodyLen) {
                char name[24] = {};
                const size_t n = nameLen < sizeof(name) - 1 ? nameLen : sizeof(name) - 1;
                memcpy(name, body + 1, n);

                const uint8_t* pub = body + 1 + nameLen;
                const size_t before = contacts::count();
                if (contacts::Contact* c = contacts::upsert(name, pub)) {
                    // Объявление и есть признак жизни: собеседник в эфире прямо сейчас,
                    // и точка присутствия обязана позеленеть — раньше она зеленела
                    // только от личного сообщения, и все выглядели «не в сети».
                    contacts::markSeen(c->addr, false, millis());
                    refreshPeersIfChanged();

                    if (contacts::count() != before) {
                        char msg[64];
                        snprintf(msg, sizeof(msg), "в эфире отозвался «%s»", name);
                        store::log("radio", msg);
                        // Новый собеседник переживает перезагрузку: без сохранения
                        // знакомиться пришлось бы после каждого выключения.
                        contacts::save();
                        refreshPeers();
                    }

                    // Знакомство состоялось — из комнат уходим и отвечаем своим
                    // объявлением, чтобы собеседник тоже завёл нас у себя.
                    if (pairRoom_[0]) {
                        ui::drawAdd(phrase_, "Собеседник найден в эфире");
                        rail::leaveRoom(pairRoom_);
                        nostr::leaveRoom(pairRoom_);
                        tracker::leaveRoom(pairRoom_);
                        pairRoom_[0] = 0;
                    }
                    if (contacts::count() != before) announceRadio();
                }
            }
        }

        // И передаём дальше в сеть: там может оказаться тот, кого ищут, и без нас они
        // не встретились бы.
        if (net::bridgeAvailable()) bridgeRadioToNet(h.dst, body, bodyLen);
        return;
    }

    // ── подтверждение доставки ────────────────────────────────────────────────────────
    //
    // Раньше подтверждения отправлялись, но НЕ принимались: вторая галочка не появлялась
    // никогда, и отправитель не мог отличить дошедшее сообщение от потерянного.
    if (h.type == voile::FT_ACK) {
        contacts::markSeen(h.src, false, millis());
        refreshPeersIfChanged();
        ui::markDelivered();
        // В приём возвращаемся сами: другие ветки отвечают передачей, и приём им
        // восстанавливает конец передачи, — а на подтверждение ответа нет.
        radio.startReceive();
        return;
    }

    // ── разбор принятого ──────────────────────────────────────────────────────────────
    //
    // Раньше здесь всё и обрывалось: кадр приходил, а дальше ничего не делалось — и
    // переписка между платами не замыкалась.
    if (h.type == voile::FT_MSG || h.type == voile::FT_MSG_KEY) {
        const uint8_t* body = buf + voile::kHdrLen;
        const size_t bodyLen = size_t(len) - voile::kHdrLen;
        if (bodyLen == 0) return;

        // Кусок файла отличаем по метке в первом байте — так же, как в сети.
        if (body[0] == 'X') {
            contacts::Contact* c = contacts::byAddr(h.src);
            xfer::onChunk(c ? c->name : "?", body, bodyLen, xfer::R_RADIO);
            contacts::markSeen(h.src, false, millis());
            // В приём возвращаемся сами: на кусок нет ответного кадра, который вернул
            // бы нас туда концом передачи. Без этого приёмник ГЛОХ после первого же
            // куска — потому голосовые и «не принимались».
            radio.startReceive();
            return;
        }

        static char text[240];
        const size_t n = bodyLen < sizeof(text) - 1 ? bodyLen : sizeof(text) - 1;
        memcpy(text, body, n);
        text[n] = 0;

        contacts::Contact* c = contacts::byAddr(h.src);
        const char* who = c ? c->name : "неизвестный";
        deliverText(c, who, text);
        contacts::markSeen(h.src, false, millis());
        refreshPeersIfChanged();

        // Подтверждаем приём: у отправителя появится вторая галочка.
        uint8_t ack[voile::kMaxPacket];
        voile::Header a{};
        a.type = voile::FT_ACK;
        memcpy(a.dst, h.src, 4);
        contacts::myAddr(a.src);       // и подтверждение тоже называет отправителя
        a.seq = h.seq;
        a.part = voile::packPart(0, 1);
        a.copy = voile::packPart(0, 1);
        const size_t al = voile::writeHeader(ack, a);
        enqueue(ack, al, 1);
        return;
    }

    radio.startReceive();
}

// ── запуск ─────────────────────────────────────────────────────────────────────────────

static size_t     myNameLen_ = 0;
static size_t     wifiCount_ = 0;
static size_t     wifiSel_ = 0;
/** Карта памяти на месте. Проверяется при запуске: без неё переписка не переживёт
 *  выключение, и человек должен об этом знать. */
static bool       sdOk_ = false;
static char       wifiPass_[64] = {};
static size_t     wifiPassLen_ = 0;
static bool       wifiReveal_ = false;
static char       wifiSsid_[33] = {};

/** Показать список найденных сетей. Данные хранятся между вызовами: повторный поиск при
 *  каждом возврате занимал бы секунды и раздражал. */
static char        wifiNames_[5][33];
static const char* wifiPtrs_[5];
static int         wifiLevels_[5];

static void showWifiList(const char* status) {
    ui::drawWifi(wifiPtrs_, wifiLevels_, wifiCount_, wifiSel_, status);
}

static char       draft_[160] = {};
static size_t     draftLen_ = 0;

/** Номер контакта в списке — для счётчиков непрочитанного. */
static int contactIndex(contacts::Contact* c) {
    for (size_t i = 0; i < contacts::count() && i < 16; ++i)
        if (contacts::at(i) == c) return int(i);
    return -1;
}

/**
 * Единый путь входящего текста: с радио и из сети сообщение проходит одинаково.
 *
 * Показ в ленту — БЕЗУСЛОВНЫЙ, ровно как в доказанно рабочей версии: лента чистится
 * при каждом входе в переписку и перечитывается с карты, поэтому запись в невидимый
 * буфер безвредна. Моя «умная» проверка открытого чата отсюда убрана: любое условие в
 * критическом пути — это способ потерять сообщение, и терять его из-за красоты нельзя.
 * Счётчик непрочитанного и звук — ВДОБАВОК, а не вместо.
 */
static void deliverText(contacts::Contact* c, const char* who, const char* text) {
    ui::addMessage(text, false, uint32_t(millis() / 1000), true);
    if (c && !(screen_ == ui::SCR_CHAT && contacts::at(selected_) == c)) {
        const int idx = contactIndex(c);
        if (idx >= 0 && unread_[idx] < 255) ++unread_[idx];
    }
    // Пишем ВСЕГДА и с маячком в порт: спор «сохраняется или нет» решают факты из
    // лога, а не воспоминания — и мои, и чьи угодно.
    const bool saved = sdOk_ && store::appendMessage(who, false, uint32_t(time(nullptr)), text);
    ets_printf("[vual] входящее от «%s»: %s\n", who,
               saved ? "записано в историю" : "НЕ записано (карта?)");
    {
        char l[64];
        snprintf(l, sizeof(l), "принято от «%s»%s", who, saved ? "" : " — НЕ записано");
        store::log("msg", l);
    }
    // Звон — НЕ здесь. Мы в пути приёма радио: четверть секунды звука здесь — это
    // задержанное подтверждение и потерянные копии. Флаг, а звенит главный цикл.
    chimePending_ = true;
}

/** Сколько молчания терпим, прежде чем считать собеседника ушедшим из эфира.
 *  Объявления в покое уходят раз в две минуты — три пропущенных подряд уже не случайность. */
constexpr uint32_t kPresenceMs = 390000;

static void refreshPeers() {
    const char* names[16];
    bool online[16];
    const size_t n = contacts::count() > 16 ? 16 : contacts::count();
    for (size_t i = 0; i < n; ++i) {
        contacts::Contact* c = contacts::at(i);
        names[i]  = c ? c->name : "?";
        // «В сети» — значит СЛЫШЕН НЕДАВНО, а не «был слышен когда-то»: липкий флаг
        // показывал бы живыми всех, кого хоть раз поймали, и терял смысл.
        online[i] = c && (c->viaWifi || c->viaLora) && c->lastSeenMs != 0 &&
                    millis() - c->lastSeenMs < kPresenceMs;
    }
    ui::setPeers(names, online, n, selected_, unread_);
}

/** Пересчитать присутствие и перерисовать список, только если что-то поменялось:
 *  вызывается и по таймеру, и на каждый принятый кадр, а перерисовка без изменений —
 *  это мигание списка на ровном месте. */
static void refreshPeersIfChanged() {
    static uint32_t lastState_ = 0;
    uint32_t state = uint32_t(contacts::count()) << 24;
    const size_t n = contacts::count() > 16 ? 16 : contacts::count();
    for (size_t i = 0; i < n; ++i) {
        contacts::Contact* c = contacts::at(i);
        const bool on = c && (c->viaWifi || c->viaLora) && c->lastSeenMs != 0 &&
                        millis() - c->lastSeenMs < kPresenceMs;
        if (on) state |= 1u << i;
        state += uint32_t(unread_[i]) * 131u;   // счётчики тоже часть картинки
    }
    if (state == lastState_) return;
    lastState_ = state;
    refreshPeers();
}

/** Отправить набранное выбранному собеседнику. */
static void sendDraft() {
    if (draftLen_ == 0) return;
    contacts::Contact* c = contacts::at(selected_);
    if (!c) return;

    // Кадр: заголовок плюс текст. Шифрование храповиком подключается здесь же —
    // ключ на сообщение берётся из цепочки, публичный вкладывается только при её смене.
    uint8_t pkt[voile::kMaxPacket];
    voile::Header h{};
    h.type = voile::FT_MSG;
    memcpy(h.dst, c->addr, 4);
    // СВОЙ адрес — обязателен. Без него кадр уходил с отправителем 00:00:00:00:
    // доставке это не мешало (потому и не замечалось), но получатель не мог понять,
    // ОТ КОГО кадр: историю писал в файл «неизвестный» — и чужие сообщения «пропадали»
    // после перезахода, присутствие не отмечалось — и все были «не в сети», а куски
    // голосовых складывались под именем «?», путь из которого не открывался, — и
    // выбрасывались молча, без единой строки в журнале.
    contacts::myAddr(h.src);
    h.seq = nextSeq();
    h.part = voile::packPart(0, 1);
    h.copy = voile::packPart(0, voile::kDefaultCopies);
    const size_t hl = voile::writeHeader(pkt, h);

    const size_t room = voile::payloadCapacity(false);
    const size_t n = draftLen_ > room ? room : draftLen_;
    memcpy(pkt + hl, draft_, n);

    // Копии с разнесением во времени и по частоте — главное средство против городских
    // переотражений: три копии дают эффект прибавки 14 дБ мощности.
    //
    // Отказ очереди — не молчание: черновик остаётся на месте, а строка ввода говорит,
    // что эфир занят. Раньше отказ глотался, и сообщение просто исчезало.
    if (!enqueue(pkt, hl + n, voile::kDefaultCopies)) {
        ui::setInput("Эфир занят — попробуйте через секунду", 0, false);
        return;
    }

    // Дубля в сеть здесь БОЛЬШЕ НЕТ. Сетевой кадр не несёт подписи отправителя, и
    // приёмник приписывал такой текст ПЕРВОМУ контакту в списке — сообщение оседало в
    // чужой ленте и чужой истории, а в нужном чате «не сохранялось». Пока у сетевых
    // кадров не появится подпись (нужна договорённость с телефонной версией), текст
    // ходит только по радио, где отправитель известен из заголовка.

    ui::addMessage(draft_, true, millis() / 1000, false);
    // Сохраняем и своё: иначе история была бы односторонней — видно, что отвечали, а
    // что писали сами, нет.
    if (sdOk_) store::appendMessage(c->name, true, uint32_t(time(nullptr)), draft_);
    draftLen_ = 0; draft_[0] = 0;
    if (ui::emojiOpen()) ui::setEmojiOpen(false);
    ui::setInput("", 0, input::keyboard::layout() == input::keyboard::LAYOUT_CYRILLIC);
}

/**
 * Отправить кусок файла по радио.
 *
 * Раньше этого не было ВООБЩЕ: перекачка помечала «отправлено», а в эфир не уходило
 * ничего — комментарий в xfer ссылался на код, которого никто не написал. Получатель
 * честно ждал куски, которых не существовало, — потому голосовые и «не принимались».
 *
 * Кусок едет обычным кадром сообщения: получатель отличает его по метке в первом байте.
 * Возврат false — очередь радио занята; перекачка попробует тот же кусок позже, это её
 * способ подстроиться под скорость эфира.
 */
static bool radioSendChunk(const char* peer, const uint8_t* data, size_t len) {
    if (!radioReady) return false;
    // Последние ТРИ слота очереди куску не отдаются — они за текстами, подтверждениями
    // и объявлениями. Иначе многоминутная перекачка голосового съедала очередь целиком,
    // и всё остальное «не доходило в принципе»: enqueue отвечал отказом, а отказ глотался.
    if (queueFree() <= 3) return false;
    contacts::Contact* c = nullptr;
    for (size_t i = 0; i < contacts::count(); ++i) {
        contacts::Contact* k = contacts::at(i);
        if (k && strcmp(k->name, peer) == 0) { c = k; break; }
    }
    if (!c) return false;

    uint8_t pkt[voile::kMaxPacket];
    voile::Header h{};
    h.type = voile::FT_MSG;
    memcpy(h.dst, c->addr, 4);
    contacts::myAddr(h.src);           // тот же закон: кадр обязан назвать отправителя
    h.seq = nextSeq();
    h.part = voile::packPart(0, 1);
    // Кускам — ДВЕ копии вместо трёх. Потеря куска и так редка при разнесении, а третья
    // копия удлиняла бы каждую перекачку в полтора раза. Тексты остаются при трёх:
    // они короткие, и там копии почти ничего не стоят.
    h.copy = voile::packPart(0, 2);
    const size_t hl = voile::writeHeader(pkt, h);
    if (hl + len > sizeof(pkt)) return false;
    memcpy(pkt + hl, data, len);
    return enqueue(pkt, hl + len, 2);
}

/** Собрать состояние устройства для экрана «Состояние». */
static ui::Status collectStatus() {
    ui::Status st{};
    st.linkUp = radioReady && contacts::count() > 0;
    // Слышимые узлы: и по радио, и в сети. Человеку важно общее число — через кого
    // вообще можно связаться прямо сейчас.
    st.neighbours = int(contacts::count()) + int(net::neighbourCount());
    st.rssi = radioReady ? int(radio.getRSSI()) : 0;
    st.snr = radioReady ? radio.getSNR() : 0.0f;
    // Заряд: делитель на выводе даёт половину напряжения батареи.
    const int mv = analogReadMilliVolts(BOARD_BAT_ADC) * 2;
    st.battery = uint8_t(((mv - 3300) * 100 / (4200 - 3300)));
    if (mv <= 0) st.battery = 0;
    st.battery = st.battery > 100 ? 100 : st.battery;
    return st;
}

/** Открыть выбранный пункт меню. */
/** Объявлены заранее: вызываются из меню, а определены ниже — там им нужны состояние
 *  личности и функции отрисовки. */
/** Прочитать поправку сенсора с карты. */
static bool loadTouchCal() {
    File f = SD.open("/vual/touch.cal", FILE_READ);
    if (!f) return false;
    const bool ok = f.read(reinterpret_cast<uint8_t*>(touchCal_), sizeof(touchCal_))
                    == int(sizeof(touchCal_));
    f.close();
    return ok;
}

/** Сохранить поправку. */
static void saveTouchCal() {
    SD.mkdir("/vual");
    File f = SD.open("/vual/touch.cal", FILE_WRITE);
    if (!f) return;
    f.write(reinterpret_cast<const uint8_t*>(touchCal_), sizeof(touchCal_));
    f.close();
}

/** Настроить сенсор и запомнить результат. */
static void runCalibration() {
    ui::calibrateTouch(touchCal_);
    vualScreen().setTouchCalibrate(touchCal_);
    haveCal_ = true;
    saveTouchCal();
}

/**
 * Встреча в комнате.
 *
 * Собеседник объявился — отвечаем ему своим адресом и ключом. Дальше связь идёт напрямую,
 * а комната больше не нужна: она только для того, чтобы найти друг друга.
 *
 * Содержимое закрыто ключом, выведенным из той же фразы. Брокер видит, что кто-то что-то
 * прислал в комнату, но не видит ни адреса, ни ключа — а без фразы не выведет и их.
 */
/**
 * Завести собеседника по телу его предложения.
 *
 * Тело: адрес, порт, ключ, имя — через вертикальную черту. Ключ настоящий и полный;
 * прежде отправлялись первые восемь байт, по которым нельзя ни зашифровать, ни узнать
 * собеседника, а имя не отправлялось вовсе — отсюда «?» в логах телефона.
 *
 * Если имени нет (старая версия на той стороне), берём короткое обозначение узла: это
 * честнее, чем одинаковая для всех подпись, по которой два собеседника неотличимы.
 */
static void addPeerFromBody(const char* body, const uint8_t peerId[20]) {
    char name[24] = {};
    uint8_t pub[voile::kPubComp] = {};
    bool havePub = false;

    if (body && *body) {
        // Третья часть — ключ, четвёртая — имя.
        const char* p = body;
        int field = 0;
        while (*p && field < 4) {
            const char* end = strchr(p, '|');
            const size_t len = end ? size_t(end - p) : strlen(p);
            if (field == 2 && len >= voile::kPubComp * 2) {
                for (size_t i = 0; i < voile::kPubComp; ++i) {
                    unsigned v = 0;
                    sscanf(p + i * 2, "%2x", &v);
                    pub[i] = uint8_t(v);
                }
                havePub = true;
            } else if (field == 3 && len > 0) {
                const size_t n = len < sizeof(name) - 1 ? len : sizeof(name) - 1;
                memcpy(name, p, n);
            }
            if (!end) break;
            p = end + 1;
            ++field;
        }
    }

    if (name[0] == 0) {
        // Имени не прислали — обозначаем по началу узла, чтобы собеседники не сливались
        // в одинаковых «Собеседник».
        snprintf(name, sizeof(name), "узел %02X%02X", peerId[0], peerId[1]);
    }

    // Без настоящего ключа заводить нечего: переписываться будет нечем.
    if (!havePub) {
        store::log("pair", "собеседник не прислал ключ — не добавляю");
        return;
    }

    contacts::upsert(name, pub);
    contacts::save();          // знакомство должно пережить перезагрузку
    refreshPeers();

    char msg[64];
    snprintf(msg, sizeof(msg), "добавлен «%s»", name);
    store::log("pair", msg);
}

static void onRailMessage(const rail::Incoming& in) {
    // Не наша комната — не наше дело.
    if (pairRoom_[0] == 0 || strcmp(in.room, pairRoom_) != 0) return;

    Serial.printf("рельса: в комнате отозвался собеседник, вид «%c»\n", in.kind);

    switch (in.kind) {
    case 'p': {
        // Присутствие: собеседник здесь. Шлём ему предложение со своим адресом.
        uint8_t offerId[20];
        esp_fill_random(offerId, sizeof(offerId));

        // В теле — наш адрес в сети и наш долговременный ключ. Этого хватает, чтобы
        // связаться напрямую и убедиться, что это тот самый собеседник.
        char body[400];
        // Ключ ЦЕЛИКОМ, а не первые восемь байт: по обрезанному связь не установить —
        // им нельзя ни зашифровать, ни узнать собеседника.
        char pub[voile::kPubComp * 2 + 1];
        for (size_t i = 0; i < voile::kPubComp; ++i)
            snprintf(pub + i * 2, 3, "%02x", contacts::myPub()[i]);

        // И ИМЯ. Без него собеседник видит «?» вместо названия устройства — именно это
        // и было видно в логах телефона.
        snprintf(body, sizeof(body), "%s|%u|%s|%s",
                 net::externalAddress()[0] ? net::externalAddress()
                                           : WiFi.localIP().toString().c_str(),
                 unsigned(net::localPort()), pub, contacts::myName());
        rail::sendOffer(pairRoom_, offerId, body);
        nostr::sendOffer(pairRoom_, offerId, body);
        tracker::sendOffer(pairRoom_, offerId, body);
        break;
    }

    case 'o': {
        // Предложение: отвечаем своим адресом, и на этом знакомство состоялось.
        char body[400];
        // Ключ ЦЕЛИКОМ, а не первые восемь байт: по обрезанному связь не установить —
        // им нельзя ни зашифровать, ни узнать собеседника.
        char pub[voile::kPubComp * 2 + 1];
        for (size_t i = 0; i < voile::kPubComp; ++i)
            snprintf(pub + i * 2, 3, "%02x", contacts::myPub()[i]);

        // И ИМЯ. Без него собеседник видит «?» вместо названия устройства — именно это
        // и было видно в логах телефона.
        snprintf(body, sizeof(body), "%s|%u|%s|%s",
                 net::externalAddress()[0] ? net::externalAddress()
                                           : WiFi.localIP().toString().c_str(),
                 unsigned(net::localPort()), pub, contacts::myName());
        rail::sendAnswer(pairRoom_, in.offerId, body);
        nostr::sendAnswer(pairRoom_, in.offerId, body);
        tracker::sendAnswer(pairRoom_, in.offerId, body);

        addPeerFromBody(in.body, in.peerId);
        ui::drawAdd(phrase_, "Собеседник найден");
        // Из комнаты уходим: она была нужна только чтобы встретиться, а держать её
        // открытой — значит светить факт знакомства дольше необходимого.
        rail::leaveRoom(pairRoom_);
        nostr::leaveRoom(pairRoom_);
        tracker::leaveRoom(pairRoom_);
        pairRoom_[0] = 0;
        break;
    }

    case 'a':
        addPeerFromBody(in.body, in.peerId);
        ui::drawAdd(phrase_, "Собеседник найден");
        rail::leaveRoom(pairRoom_);
        nostr::leaveRoom(pairRoom_);
        tracker::leaveRoom(pairRoom_);
        pairRoom_[0] = 0;
        break;

    default: break;
    }
}

/**
 * Встреча через вторую рельсу.
 *
 * Тело то же, что у первой, — обе приносят одинаковые сообщения, различается только путь.
 * Поэтому просто переупаковываем и отдаём общему обработчику: разводить две одинаковые
 * ветки значило бы чинить потом каждую отдельно.
 */
static void onNostrMessage(const nostr::Incoming& in) {
    rail::Incoming r{};
    r.kind = in.kind;
    snprintf(r.room, sizeof(r.room), "%s", in.room);
    memcpy(r.peerId, in.peerId, sizeof(r.peerId));
    memcpy(r.offerId, in.offerId, sizeof(r.offerId));
    r.hasOffer = in.hasOffer;
    snprintf(r.body, sizeof(r.body), "%s", in.body);
    onRailMessage(r);
}

/** Встреча через трекер. Тело то же — переупаковываем и отдаём общему обработчику. */
static void onTrackerMessage(const tracker::Incoming& in) {
    rail::Incoming r{};
    r.kind = in.kind;
    snprintf(r.room, sizeof(r.room), "%s", in.room);
    memcpy(r.peerId, in.peerId, sizeof(r.peerId));
    memcpy(r.offerId, in.offerId, sizeof(r.offerId));
    r.hasOffer = in.hasOffer;
    snprintf(r.body, sizeof(r.body), "%s", in.body);
    onRailMessage(r);
}

/**
 * Кадр из сети. Если это кусок файла — отдаём сборщику, иначе это обычное сообщение.
 *
 * Различаем по первому байту: у кусков там метка. Отдельного вида кадра не заводим —
 * он потребовал бы согласования с телефонной версией, а метки достаточно.
 */
static void onNetFrame(const uint8_t id[20], const uint8_t* data, size_t len) {
    if (len == 0) return;

    // Ищем имя собеседника по его узлу: сборщику нужно знать, от кого файл.
    const char* who = "?";
    for (size_t i = 0; i < contacts::count(); ++i) {
        contacts::Contact* c = contacts::at(i);
        if (c) { who = c->name; break; }
    }

    if (data[0] == 'X') { xfer::onChunk(who, data, len, xfer::R_NET); return; }

    // Обычное сообщение — общим путём: лента или непрочитанные, звук, карта.
    static char text[256];
    const size_t n = len < sizeof(text) - 1 ? len : sizeof(text) - 1;
    memcpy(text, data, n);
    text[n] = 0;
    // Отправитель сетевого кадра НЕИЗВЕСТЕН: подписи в кадре нет. Раньше текст
    // приписывался первому контакту — оседал в чужой ленте и чужой истории. Честнее
    // сложить под именем «Сеть»: сообщение не потеряно, но и не выдано за чужое.
    deliverText(nullptr, contacts::count() == 1 && contacts::at(0)
                             ? contacts::at(0)->name : "Сеть", text);
    refreshPeers();
}

/** Последнее принятое голосовое — его и проигрываем по нажатию. */
static char lastVoice_[80] = {};

/** Файл собран или отправлен — показываем в ленте. */
static void onFileReady(const char* peer, const char* path, xfer::Kind kind, bool incoming) {
    // Запоминаем путь: принять голосовое и не иметь возможности его послушать —
    // половина дела. Нажатие на ленту проигрывает последнее.
    if (incoming && kind == xfer::K_VOICE) snprintf(lastVoice_, sizeof(lastVoice_), "%s", path);
    const char* what = kind == xfer::K_VOICE ? "голосовое"
                     : (kind == xfer::K_PHOTO ? "снимок" : "файл");
    if (incoming) {
        if (kind == xfer::K_VOICE) {
            // Длительность — из размера файла: 8000 отсчётов в секунду, по полбайта
            // на отсчёт после сжатия.
            int secs = 0;
            File f = SD.open(path, FILE_READ);
            if (f) { secs = int((f.size() > 12 ? f.size() - 12 : 0) * 2 / 8000); f.close(); }
            contacts::Contact* c = nullptr;
            for (size_t i = 0; i < contacts::count(); ++i) {
                contacts::Contact* k = contacts::at(i);
                if (k && strcmp(k->name, peer) == 0) { c = k; break; }
            }
            // Показ безусловный — тем же правилом, что у текста: условие в пути
            // доставки — способ потерять сообщение. Непрочитанное — вдобавок.
            ui::addVoiceMessage(path, secs, false, true);
            if (c && !(screen_ == ui::SCR_CHAT && contacts::at(selected_) == c)) {
                const int idx = contactIndex(c);
                if (idx >= 0 && unread_[idx] < 255) ++unread_[idx];
                refreshPeers();
            }
            chimePending_ = true;
        } else {
            ui::addMessage(what, false, uint32_t(millis() / 1000), true);
        }
        if (kind == xfer::K_VOICE) {
            char rec[96];
            snprintf(rec, sizeof(rec), "voice:%s", path);
            store::appendMessage(peer, false, uint32_t(time(nullptr)), rec);
        } else {
            store::appendMessage(peer, false, uint32_t(time(nullptr)), what);
        }
    } else {
        // Своё сообщение «голосовое» уже стоит в ленте с момента записи — добавлять
        // второе нельзя, их и так путали. Завершение передачи даёт ему вторую галочку.
        ui::markDelivered();
    }
    char msg[120];
    snprintf(msg, sizeof(msg), "%s %s: %s", incoming ? "принят" : "отправлен", what, path);
    store::log("xfer", msg);
}

static void saveIdentity();
static void showIdentity();
static void showLogPage();

static void openMenuItem() {
    switch (menuSel_) {
    case ui::MENU_IDENTITY: {
        snprintf(myName_, sizeof(myName_), "%s", contacts::myName());
        myNameLen_ = strlen(myName_);
        screen_ = ui::SCR_IDENTITY;
        showIdentity();
        break;
    }

    case ui::MENU_CALIBRATE:
        runCalibration();
        screen_ = ui::SCR_MENU;
        ui::drawMenu(menuSel_);
        break;

    case ui::MENU_LOG:
        logPage_ = 0;
        showLogPage();
        screen_ = ui::SCR_LOG;
        break;

    case ui::MENU_ADD_RADIO:
        if (!contacts::haveIdentity()) {
            screen_ = ui::SCR_IDENTITY; showIdentity(); break;
        }
        phrase_[0] = 0; phraseLen_ = 0;
        screen_ = ui::SCR_ADD_RADIO;
        ui::drawAddRadio("", nullptr, int(contacts::count()));
        break;

    case ui::MENU_ADD_NET:
        // Без личности знакомиться не с чем: собеседник узнаёт устройство по ключу, а
        // ключа ещё нет. Ведём человека туда, где он заводится.
        if (!contacts::haveIdentity()) {
            snprintf(myName_, sizeof(myName_), "%s", contacts::myName());
            myNameLen_ = strlen(myName_);
            screen_ = ui::SCR_IDENTITY;
            showIdentity();
            break;
        }
        phrase_[0] = 0; phraseLen_ = 0;
        screen_ = ui::SCR_ADD;
        ui::drawAdd("", nullptr);
        break;

    case ui::MENU_WIFI: {
        screen_ = ui::SCR_WIFI;
        ui::drawWifi(nullptr, nullptr, 0, 0, "Поиск сетей…");
        // Поиск занимает пару секунд и останавливает всё остальное. Для устройства без
        // многозадачности это приемлемо: человек всё равно ждёт список.
        WiFi.mode(WIFI_STA);
        const int n = WiFi.scanNetworks();
        const int shown = n > 5 ? 5 : (n < 0 ? 0 : n);
        for (int i = 0; i < shown; ++i) {
            snprintf(wifiNames_[i], sizeof(wifiNames_[i]), "%s", WiFi.SSID(i).c_str());
            wifiPtrs_[i] = wifiNames_[i];
            wifiLevels_[i] = WiFi.RSSI(i);
        }
        wifiCount_ = size_t(shown);
        wifiSel_ = 0;
        showWifiList(shown == 0 ? "Сетей не найдено" : nullptr);
        break;
    }

    case ui::MENU_STATUS:
        screen_ = ui::SCR_STATUS;
        // В строке сети показываем и внешний адрес, если он известен: по нему видно,
        // доступно ли устройство из других сетей.
        static char netLine[64];
        if (WiFi.isConnected()) {
            const char* ext = net::externalAddress();
            if (ext && *ext) snprintf(netLine, sizeof(netLine), "%s · снаружи %s",
                                      WiFi.localIP().toString().c_str(), ext);
            else snprintf(netLine, sizeof(netLine), "%s", WiFi.localIP().toString().c_str());
        } else netLine[0] = 0;
        ui::drawStatus2(collectStatus(), netLine[0] ? netLine : nullptr, radioReady, sdOk_);
        break;

    case ui::MENU_BACK:
    default:
        screen_ = ui::SCR_CHATS;
        refreshPeers();
        ui::draw(screen_);
        break;
    }
}


/** Приёмник строк истории: интерфейс складывает их в ленту. */
static void onHistoryLine(bool mine, uint32_t ts, const char* text) {
    // Голосовые в истории хранятся маркером с путём: после перезахода они остаются
    // ПРОИГРЫВАЕМЫМИ пузырями. Раньше писался текст со значком, которого нет в шрифте, —
    // и вместо иконки вставал квадрат-заглушка.
    if (strncmp(text, "voice:", 6) == 0) {
        const char* path = text + 6;
        int secs = 0;
        File f = SD.open(path, FILE_READ);
        if (f) { secs = int((f.size() > 12 ? f.size() - 12 : 0) * 2 / 8000); f.close(); }
        ui::addVoiceMessage(path, secs, mine, true);
        return;
    }
    ui::addMessage(text, mine, ts, true);
}

/** Открыть переписку и подтянуть историю с карты.
 *
 *  Без этого каждая перепрошивка и даже перезапуск начинали ленту с чистого листа, хотя
 *  сама переписка лежала на карте — просто её никто не читал. */
static void openChat(size_t idx) {
    selected_ = idx;
    screen_ = ui::SCR_CHAT;
    if (idx < 16) unread_[idx] = 0;   // вошёл — значит прочитал
    // Чистим ленту ОБЯЗАТЕЛЬНО: иначе к истории этого собеседника примешивались бы
    // сообщения предыдущего — лента общая, а переписки разные.
    ui::clearMessages();
    contacts::Contact* c = contacts::at(idx);
    // Подгружаем столько, сколько лента вообще держит: раньше при пределе ленты в 30
    // подтягивалось лишь 20, и хвост истории терялся без причины.
    if (c && sdOk_) store::loadMessages(c->name, 28, onHistoryLine);
    ui::draw(screen_);
}

/** Показать страницу журнала. Одиннадцать строк — столько помещается читаемо. */
static void showLogPage() {
    logCount_ = store::logPage(logPage_, 11, logBuf_, sizeof(logBuf_),
                               logLines_, &logHasMore_);
    logFrom_ = 0;
    ui::drawLog(logLines_, logCount_, 0);
}

/** Показать экран личности с текущим адресом. */
static void showIdentity() {
    char addr[40] = {};
    if (contacts::haveIdentity()) {
        uint8_t a[4];
        contacts::myAddr(a);
        snprintf(addr, sizeof(addr), "Адрес %02X%02X%02X%02X", a[0], a[1], a[2], a[3]);
    }
    ui::drawIdentity(myName_, addr, contacts::haveIdentity());
}

static void saveIdentity() {
    if (myNameLen_ == 0) return;
    if (contacts::setMyName(myName_)) {
        uint8_t a[4];
        contacts::myAddr(a);
        Serial.printf("личность: «%s», адрес %02X%02X%02X%02X\n", myName_,
                      a[0], a[1], a[2], a[3]);
        // Сохраняем на карту: ключ и есть личность, и терять его при каждой прошивке
        // означало бы заново заводить все знакомства.
        store::saveIdentity(contacts::myName(),
                            contacts::myPrivMutable(), 32,
                            contacts::myPubMutable(), voile::kPubComp);
        store::log("identity", "личность сохранена на карту");
    } else {
        Serial.println("не удалось создать ключ");
    }
    showIdentity();
}

/** Предел записи — ОДНА МИНУТА.
 *
 *  Не произвол: минута сжатой речи это около 240 КБ, и такой файл уходит по радио
 *  ощутимое время. Длиннее — и передача занимала бы больше, чем сама запись. */
constexpr uint32_t kVoiceMaxMs = 60000;
static uint32_t voiceStart_ = 0;
static char     voicePath_[80] = {};

/** Начать запись голосового. */
static void startVoice() {
    if (audio::device::isRecording()) return;
    if (!sdOk_) { store::log("voice", "нет карты — записывать некуда"); return; }

    snprintf(voicePath_, sizeof(voicePath_), "/vual/media/v%lu.vua",
             (unsigned long)millis());
    if (!audio::device::startRecording(voicePath_)) {
        store::log("voice", "не удалось начать запись");
        return;
    }
    voiceStart_ = millis();
    ui::setRecording(true, 0);
    store::log("voice", "запись начата");
}

/** Закончить запись и отправить. */
static void stopVoice() {
    if (!audio::device::isRecording()) return;
    const uint32_t ms = audio::device::stopRecording();
    ui::setRecording(false, 0);

    // Слишком короткое — не отправляем. Случайное касание значка иначе слало бы
    // собеседнику пустое сообщение, и объяснить это было бы нечем.
    if (ms < 700) {
        store::log("voice", "слишком коротко — не отправляем");
        return;
    }
    contacts::Contact* c = contacts::at(selected_);
    if (!c) { store::log("voice", "некому отправлять"); return; }

    // Маршрут выбирается ПО СОБЕСЕДНИКУ, а не по наличию сети вообще. Прежде «есть
    // хоть один сосед по сети» отправляло голосовое в сеть — даже когда адресат жил
    // только в эфире, — и оно улетало кому угодно, кроме него.
    const bool loraFresh = c->viaLora && c->lastSeenMs != 0 &&
                           millis() - c->lastSeenMs < kPresenceMs;
    const bool viaNet = !loraFresh && net::neighbourCount() > 0;
    const xfer::Route route = viaNet ? xfer::R_NET : xfer::R_RADIO;

    if (!viaNet) {
        const uint32_t est = xfer::radioEstimateMs(uint32_t(ms) * 4);   // сжатие вчетверо
        char warn[80];
        snprintf(warn, sizeof(warn), "По радио это займёт %lu с",
                 (unsigned long)(est / 1000));
        ui::setInput(warn, 0, false);
    }

    if (xfer::send(c->name, voicePath_, xfer::K_VOICE, route)) {
        char msg[80];
        snprintf(msg, sizeof(msg), "голосовое %lu мс отправляется по %s",
                 (unsigned long)ms, viaNet ? "сети" : "радио");
        store::log("voice", msg);
        ui::addVoiceMessage(voicePath_, int(ms / 1000), true, false);
        if (sdOk_) {
            char rec[96];
            snprintf(rec, sizeof(rec), "voice:%s", voicePath_);
            store::appendMessage(c->name, true, uint32_t(time(nullptr)), rec);
        }
    }
}

/**
 * Объявить себя в эфире.
 *
 * Без этого две платы не находят друг друга ВООБЩЕ: каждая ждёт, что её позовут, и обе
 * молчат. Объявление несёт наш адрес и, если идёт знакомство, адрес встречи — по нему
 * собеседник и понимает, что зовут именно его.
 *
 * Раз в двадцать секунд: чаще — впустую занимать общий эфир, реже — знакомство растянется
 * настолько, что человек решит, что оно не работает.
 */
static void announceRadio() {
    if (!radioReady || !contacts::haveIdentity()) return;

    uint8_t pkt[voile::kMaxPacket];
    voile::Header h{};
    h.type = voile::FT_HELLO;

    uint8_t me[4];
    contacts::myAddr(me);
    memcpy(h.src, me, 4);

    // Если идёт знакомство — зовём по адресу встречи, иначе объявляемся всем.
    if (pairRoom_[0]) {
        contacts::Rendezvous rv;
        contacts::deriveRendezvous(phrase_, rv);
        memcpy(h.dst, rv.meetAddr, 4);
    } else {
        memset(h.dst, 0xFF, 4);          // всем, кто слышит
    }

    h.seq = nextSeq();
    h.part = voile::packPart(0, 1);
    h.copy = voile::packPart(0, voile::kDefaultCopies);
    const size_t hl = voile::writeHeader(pkt, h);

    // В теле — наше имя и открытый ключ: собеседнику нужно и то, и другое, чтобы завести
    // нас у себя и начать шифрованный обмен.
    size_t o = hl;
    const uint8_t nameLen = uint8_t(strlen(contacts::myName()));
    pkt[o++] = nameLen;
    memcpy(pkt + o, contacts::myName(), nameLen); o += nameLen;
    memcpy(pkt + o, contacts::myPubMutable(), voile::kPubComp); o += voile::kPubComp;

    enqueue(pkt, o, voile::kDefaultCopies);
    store::log("radio", pairRoom_[0] ? "зову по адресу встречи" : "объявляюсь в эфире");
}

/** Запомнить сеть, к которой только что подключились. */
static void saveWifiCreds() {
    store::saveWifi(wifiSsid_, wifiPass_);
}

/** Шаг назад — единый для всех экранов. */
static void goBack() {
    switch (screen_) {
    case ui::SCR_CHAT:
        // Уходим из переписки — панель закрываем: иначе она останется висеть поверх
        // списка собеседников.
        if (ui::emojiOpen()) ui::setEmojiOpen(false);
        screen_ = ui::SCR_CHATS; refreshPeers(); ui::draw(screen_);
        break;
    case ui::SCR_MENU:  screen_ = ui::SCR_CHATS; ui::draw(screen_); break;
    case ui::SCR_WIFI_PASS: screen_ = ui::SCR_WIFI; showWifiList(nullptr); break;
    case ui::SCR_ADD:
    case ui::SCR_ADD_RADIO:
    case ui::SCR_LOG:
    case ui::SCR_IDENTITY:
    case ui::SCR_WIFI:
    case ui::SCR_STATUS: screen_ = ui::SCR_MENU; ui::drawMenu(menuSel_); break;
    default: break;
    }
}

/** Открыть экран ввода пароля для выбранной сети. */
static void openPassScreen() {
    snprintf(wifiSsid_, sizeof(wifiSsid_), "%s", WiFi.SSID(int(wifiSel_)).c_str());
    wifiPass_[0] = 0; wifiPassLen_ = 0; wifiReveal_ = false;
    screen_ = ui::SCR_WIFI_PASS;
    ui::drawWifiPass(wifiSsid_, wifiPass_, wifiReveal_, nullptr);
}

/** Подключиться к выбранной сети с введённым паролем. */
static void connectWifi() {
    ui::drawWifiPass(wifiSsid_, wifiPass_, wifiReveal_, "Подключаюсь…");

    if (wifiPassLen_ > 0) WiFi.begin(wifiSsid_, wifiPass_);
    else WiFi.begin(wifiSsid_);          // пустой пароль — открытая сеть

    // Ждём С ОТРИСОВКОЙ, а не молча. Пятнадцать секунд немого ожидания выглядели как
    // зависание: экран не обновлялся, касания не читались, плата казалась мёртвой.
    const uint32_t until = millis() + 15000;
    int ticks = 0;
    while (millis() < until && !WiFi.isConnected()) {
        delay(200);
        if (++ticks % 5 == 0) {
            char msg[32];
            snprintf(msg, sizeof(msg), "Подключаюсь%.*s", (ticks / 5) % 4, "...");
            ui::drawWifiPass(wifiSsid_, wifiPass_, wifiReveal_, msg);
        }
    }

    if (!WiFi.isConnected()) {
        ui::drawWifiPass(wifiSsid_, wifiPass_, wifiReveal_, "Не подключилось — проверьте пароль");
        return;
    }

    // Сеть запоминаем: вводить пароль заново после каждой прошивки — издевательство,
    // а карта на плате для того и есть.
    saveWifiCreds();

    configTime(0, 0, "pool.ntp.org", "time.google.com");
    net::begin();
    rail::begin(net::myId());
    rail::setOnMessage(onRailMessage);

    // Вторая рельса — запасной путь. Брокеры и узлы Nostr закрывают разными способами и
    // в разных местах: когда не работает одно, обычно работает другое.
    nostr::begin(net::myId());
    nostr::setOnMessage(onNostrMessage);

    // Третья рельса — трекеры. Самая живучая: их закрывают реже всего, потому что они
    // нужны слишком многим и на вид безобидны.
    tracker::begin(net::myId());
    tracker::setOnMessage(onTrackerMessage);

    // Внешний адрес спросим ПОТОМ, из общего цикла: опрос семи серверов — ещё восемь
    // секунд, и в момент подключения они ни к чему.
    needExternal_ = true;

    // Экран подключения закрываем сразу: дело сделано, держать его незачем.
    screen_ = ui::SCR_MENU;
    ui::drawMenu(menuSel_);
}

/**
 * Мост из сети в эфир: полученное по интернету объявление знакомства уходит в радио.
 *
 * Так устройство, у которого есть только интернет, достаёт того, у кого только радио —
 * а мы, имея оба способа, соединяем их. Без моста они не встретились бы никогда: между
 * ними нет ни одного общего пути.
 */
static void bridgeNetToRadio(const uint8_t meetAddr[4], const uint8_t* payload, size_t len) {
    if (!radioReady) return;
    uint8_t pkt[voile::kMaxPacket];
    voile::Header h{};
    h.type = voile::FT_HELLO;
    memcpy(h.dst, meetAddr, 4);
    memcpy(h.src, meetAddr, 4);        // на встрече обе стороны адресуются по фразе
    h.seq = nextSeq();
    h.part = voile::packPart(0, 1);
    h.copy = voile::packPart(0, voile::kDefaultCopies);
    const size_t hl = voile::writeHeader(pkt, h);
    const size_t room = voile::payloadCapacity(false);
    const size_t n = len > room ? room : len;
    memcpy(pkt + hl, payload, n);
    enqueue(pkt, hl + n, voile::kDefaultCopies);
    Serial.println("мост: объявление из сети ушло в эфир");
}

/**
 * Мост из эфира в сеть: услышанное по радио объявление знакомства уходит в интернет.
 *
 * Работает, только если есть через кого — то есть виден хоть один собеседник в сети.
 * Именно этого и просили: устройства из эфира знакомятся с теми, кто в интернете.
 */
static void bridgeRadioToNet(const uint8_t meetAddr[4], const uint8_t* payload, size_t len) {
    if (!net::bridgeAvailable()) return;
    net::bridgeToInternet(meetAddr, payload, len);
}

static void handleEvent(const input::Event& e) {
    using namespace input;
    // Значок раскладки живёт состоянием Shift: буква гасит одноразовый Shift внутри
    // клавиатуры, и значок обязан это показать. Сам вызов сравнивает и не мигает зря.
    ui::setShiftMode(uint8_t(input::keyboard::shiftMode()));
    const bool cyr = keyboard::layout() == keyboard::LAYOUT_CYRILLIC;

    switch (e.type) {
    case EV_CHAR:
        if (screen_ == ui::SCR_ADD_RADIO) {
            if (phraseLen_ + 1 < sizeof(phrase_)) {
                phrase_[phraseLen_++] = e.ch;
                phrase_[phraseLen_] = 0;
                ui::drawAddRadio(phrase_, nullptr, int(contacts::count()));
            }
        } else if (screen_ == ui::SCR_IDENTITY) {
            if (myNameLen_ + 1 < sizeof(myName_)) {
                myName_[myNameLen_++] = e.ch;
                myName_[myNameLen_] = 0;
                showIdentity();
            }
        } else if (screen_ == ui::SCR_WIFI_PASS) {
            if (wifiPassLen_ + 1 < sizeof(wifiPass_)) {
                wifiPass_[wifiPassLen_++] = e.ch;
                wifiPass_[wifiPassLen_] = 0;
                ui::drawWifiPass(wifiSsid_, wifiPass_, wifiReveal_, nullptr);
            }
        } else if (screen_ == ui::SCR_ADD) {
            if (phraseLen_ + 1 < sizeof(phrase_)) {
                phrase_[phraseLen_++] = e.ch;
                phrase_[phraseLen_] = 0;
                ui::drawAdd(phrase_, nullptr);
            }
        } else if (draftLen_ + 1 < sizeof(draft_)) {
            draft_[draftLen_++] = e.ch;
            draft_[draftLen_] = 0;
            ui::setInput(draft_, draftLen_, cyr);
        }
        break;

    case EV_BACKSPACE:
        if (screen_ == ui::SCR_ADD_RADIO) {
            if (phraseLen_ + 1 < sizeof(phrase_)) {
                phrase_[phraseLen_++] = e.ch;
                phrase_[phraseLen_] = 0;
                ui::drawAddRadio(phrase_, nullptr, int(contacts::count()));
            }
        } else if (screen_ == ui::SCR_IDENTITY) {
            if (myNameLen_ + 1 < sizeof(myName_)) {
                myName_[myNameLen_++] = e.ch;
                myName_[myNameLen_] = 0;
                showIdentity();
            }
        } else if (screen_ == ui::SCR_WIFI_PASS) {
            if (wifiPassLen_ > 0) {
                --wifiPassLen_;
                while (wifiPassLen_ > 0 && (uint8_t(wifiPass_[wifiPassLen_]) & 0xC0) == 0x80)
                    --wifiPassLen_;
                wifiPass_[wifiPassLen_] = 0;
                ui::drawWifiPass(wifiSsid_, wifiPass_, wifiReveal_, nullptr);
            }
            break;
        }
        if (screen_ == ui::SCR_ADD) {
            if (phraseLen_ > 0) {
                --phraseLen_;
                while (phraseLen_ > 0 && (uint8_t(phrase_[phraseLen_]) & 0xC0) == 0x80) --phraseLen_;
                phrase_[phraseLen_] = 0;
                ui::drawAdd(phrase_, nullptr);
            }
            break;
        }
        if (draftLen_ > 0) {
            // Удаляем ЦЕЛУЮ букву, а не байт: кириллица в UTF-8 двухбайтовая, и удаление
            // по одному байту оставило бы половину символа и испортило бы всю строку.
            --draftLen_;
            while (draftLen_ > 0 && (uint8_t(draft_[draftLen_]) & 0xC0) == 0x80) --draftLen_;
            draft_[draftLen_] = 0;
            ui::setInput(draft_, draftLen_, cyr);
        }
        break;

    case EV_ENTER:
        switch (screen_) {
        case ui::SCR_CHAT: sendDraft(); break;

        case ui::SCR_ADD: {
            if (phraseLen_ == 0) break;
            if (!contacts::haveIdentity()) {
                ui::drawAdd(phrase_, "Сначала заполните «Кто я»");
                break;
            }
            // Из фразы выводим адрес встречи и ключ обмена — те же, что у телефонной
            // версии, поэтому знакомиться можно и с платой, и с телефоном.
            contacts::Rendezvous rv;
            contacts::deriveRendezvous(phrase_, rv);
            // Приходим в комнату, выведенную из фразы, — там же ждёт телефон. Комната
            // считается по ЕГО формуле, поэтому стороны попадают в одну и ту же.
            char roomHex[41];
            for (int i = 0; i < 20; ++i)
                snprintf(roomHex + i * 2, 3, "%02x", rv.phoneRoom[i]);
            snprintf(pairRoom_, sizeof(pairRoom_), "%s", roomHex);
            memcpy(pairWrap_, rv.wrapKey, sizeof(pairWrap_));

            rail::joinRoom(roomHex);
            rail::announce(roomHex);
            nostr::joinRoom(roomHex);
            nostr::announce(roomHex);
            tracker::joinRoom(roomHex);

            char msg[96];
            if (rail::connected() || nostr::connected() || tracker::connected())
                snprintf(msg, sizeof(msg), "Жду в комнате %.8s…", roomHex);
            else
                snprintf(msg, sizeof(msg), "Нет связи с рельсой — ищу только в эфире");
            ui::drawAdd(phrase_, msg);
            Serial.printf("знакомство: фраза «%s», адрес %02X%02X%02X%02X\n", phrase_,
                          rv.meetAddr[0], rv.meetAddr[1], rv.meetAddr[2], rv.meetAddr[3]);
            break;
        }

        case ui::SCR_WIFI:
            if (wifiCount_ > 0) openPassScreen();
            break;

        case ui::SCR_WIFI_PASS: connectWifi(); break;

        case ui::SCR_IDENTITY: saveIdentity(); break;

        case ui::SCR_ADD_RADIO: {
            if (phraseLen_ == 0) break;
            // Запоминаем фразу как текущее знакомство: по её адресу встречи уходит
            // объявление, и по нему же мы узнаём, что зовут нас.
            contacts::Rendezvous rv;
            contacts::deriveRendezvous(phrase_, rv);
            for (int i = 0; i < 20; ++i) snprintf(pairRoom_ + i * 2, 3, "%02x", rv.phoneRoom[i]);
            announceRadio();
            ui::drawAddRadio(phrase_, "Зову в эфире…", int(contacts::count()));
            break;
        }

        case ui::SCR_MENU: openMenuItem(); break;

        default:
            screen_ = ui::SCR_CHAT; ui::draw(screen_);
            break;
        }
        break;

    case EV_UP:
        if (screen_ == ui::SCR_CHAT) ui::chatScroll(+48);   // вверх — к старому
        else if (screen_ == ui::SCR_CHATS && selected_ > 0) { --selected_; refreshPeers(); }
        else if (screen_ == ui::SCR_LOG && logHasMore_) {
            ++logPage_; showLogPage();          // вверх — вглубь, к более старому
        }
        else if (screen_ == ui::SCR_MENU && menuSel_ > 0) {
            menuSel_ = ui::MenuItem(menuSel_ - 1); ui::drawMenu(menuSel_);
        }
        break;
    case EV_DOWN:
        if (screen_ == ui::SCR_CHAT) ui::chatScroll(-48);   // вниз — к свежему
        else if (screen_ == ui::SCR_CHATS && selected_ + 1 < contacts::count()) {
            ++selected_; refreshPeers();
        } else if (screen_ == ui::SCR_LOG && logPage_ > 0) {
            --logPage_; showLogPage();          // вниз — к свежему
        } else if (screen_ == ui::SCR_MENU && menuSel_ + 1 < ui::MENU_COUNT) {
            menuSel_ = ui::MenuItem(menuSel_ + 1); ui::drawMenu(menuSel_);
        }
        break;

    case EV_SELECT:
        if (screen_ == ui::SCR_CHATS) openChat(selected_);
        else if (screen_ == ui::SCR_MENU) openMenuItem();
        else if (screen_ == ui::SCR_CHAT) {
            // Нажатие трекбола в переписке — Shift: одно даёт одну заглавную, быстрое
            // двойное — верхний регистр до следующего нажатия. Повешено на трекбол
            // потому, что штатная клавиатура про одиночный Shift обычно молчит — мы
            // просто не узнаём о нажатии (см. kShiftKey в hw_input.cpp).
            input::keyboard::shiftTap(millis());
            ui::setShiftMode(uint8_t(input::keyboard::shiftMode()));
        }
        break;

    case EV_BACK:
        goBack();
        break;

    case EV_RIGHT:
        // Вправо из списка — меню. Раньше попасть в него было нечем вовсе.
        if (screen_ == ui::SCR_CHATS) { screen_ = ui::SCR_MENU; ui::drawMenu(menuSel_); }
        break;

    case EV_DRAG:
        // Живое перетаскивание: лента едет вместе с пальцем, шаг за шагом. Знак прямой:
        // палец вниз тянет содержимое вниз — открывается старое сверху.
        if (screen_ == ui::SCR_CHAT) {
            ui::chatScroll(e.y);
        } else if (screen_ == ui::SCR_CHATS) {
            static int acc = 0;
            acc += e.y;
            while (acc >= 40)  { ui::peersScroll(-1); acc -= 40; }   // строка — 40 точек
            while (acc <= -40) { ui::peersScroll(+1); acc += 40; }
        } else if (screen_ == ui::SCR_LOG) {
            static int accLog = 0;
            accLog += e.y;
            if (accLog >= 48)       { if (logHasMore_) { ++logPage_; showLogPage(); } accLog = 0; }
            else if (accLog <= -48) { if (logPage_ > 0) { --logPage_; showLogPage(); } accLog = 0; }
        }
        break;

    case EV_SWIPE_UP:
    case EV_SWIPE_DOWN: {
        // Прокрутка пальцем. Смахивание вниз тянет содержимое вниз — открывается то,
        // что ВЫШЕ, то есть старое; вверх — наоборот. Величина берётся из самого
        // жеста: короткое движение листает чуть-чуть, размашистое — на экран.
        const int dy = e.y;                       // пройденный путь пальца, точки
        const int mag = dy < 0 ? -dy : dy;
        if (screen_ == ui::SCR_CHAT) {
            ui::chatScroll(e.type == input::EV_SWIPE_DOWN ? +mag : -mag);
        } else if (screen_ == ui::SCR_CHATS) {
            const int rows = (mag + 20) / 40;     // строка списка — 40 точек
            ui::peersScroll(e.type == input::EV_SWIPE_DOWN ? -rows : +rows);
        } else if (screen_ == ui::SCR_LOG) {
            if (e.type == input::EV_SWIPE_DOWN) { if (logHasMore_) { ++logPage_; showLogPage(); } }
            else                                { if (logPage_ > 0) { --logPage_; showLogPage(); } }
        }
        break;
    }

    case EV_TAP: {
        // Нажимается ВСЁ видимое: значок меню, стрелка назад, строки списков, кнопки.
        // Раньше касание работало только на списке собеседников, остальное выбиралось
        // трекболом — о чём догадаться было нельзя.
        const ui::HitResult h = ui::hitTest(screen_, e.x, e.y);

        // Взведённое удаление снимается любым касанием, кроме повторного касания того же
        // крестика: случайно задел экран — ничего не потерял.
        if (ui::armedDelete() >= 0 &&
            !(h.what == ui::HIT_DELETE && h.rowIndex == ui::armedDelete())) {
            ui::armDelete(-1);
        }

        switch (h.what) {
        case ui::HIT_BURGER: screen_ = ui::SCR_MENU; ui::drawMenu(menuSel_); break;
        case ui::HIT_BACK:   goBack(); break;

        case ui::HIT_DELETE: {
            if (screen_ != ui::SCR_CHATS) break;
            const size_t idx = size_t(h.rowIndex);
            if (idx >= contacts::count()) break;
            if (ui::armedDelete() == h.rowIndex) {
                // Второе касание — удаляем. Вместе с записью пропадает ключ собеседника,
                // поэтому и нужны два касания, а не одно.
                contacts::Contact* c = contacts::at(idx);
                char msg[64];
                snprintf(msg, sizeof(msg), "удалён «%s»", c ? c->name : "?");
                ui::armDelete(-1);
                if (contacts::removeAt(idx)) {
                    contacts::save();
                    store::log("contacts", msg);
                    if (selected_ >= contacts::count() && selected_ > 0) --selected_;
                    refreshPeers();
                }
            } else {
                ui::armDelete(h.rowIndex);   // первое касание — только взводим
            }
            break;
        }
        case ui::HIT_LAYOUT:
            keyboard::setLayout(cyr ? keyboard::LAYOUT_LATIN : keyboard::LAYOUT_CYRILLIC);
            ui::setInput(draft_, draftLen_, !cyr);
            break;
        case ui::HIT_SEND:   sendDraft(); break;

        case ui::HIT_MIC:
            if (audio::device::isRecording()) stopVoice();
            else startVoice();
            break;

        case ui::HIT_EMOJI:
            // Панель открывается и закрывается тем же значком: отдельная кнопка закрытия
            // на таком экране только отняла бы место.
            ui::setEmojiOpen(!ui::emojiOpen());
            if (!ui::emojiOpen()) ui::setInput(draft_, draftLen_, cyr);
            break;

        case ui::HIT_EMOJI_CELL: {
            // Смайлик уходит собеседнику ОБЫЧНЫМ символом — на телефоне он выглядит
            // привычно. На плате рисуется своей картинкой, но в тексте это те же байты.
            const size_t idx = size_t(h.rowIndex);
            if (idx >= emoji::kCount) break;
            const char* e = emoji::kSet[idx].utf8;
            const size_t n = strlen(e);
            if (draftLen_ + n + 1 < sizeof(draft_)) {
                memcpy(draft_ + draftLen_, e, n);
                draftLen_ += n;
                draft_[draftLen_] = 0;
                ui::setInput(draft_, draftLen_, cyr);
            }
            break;
        }
        case ui::HIT_ROW:
            switch (screen_) {
            case ui::SCR_CHAT: {
                // Касание голосового пузыря проигрывает ИМЕННО его — и своё, и принятое.
                // Маячок в порт: спор «нажатие не работает» решается фактами — куда
                // попало касание и открылся ли файл.
                const char* v = ui::voiceAt(h.rowIndex);
                ets_printf("[vual] касание ленты: сообщение %d, файл «%s»\n",
                           h.rowIndex, v && *v ? v : "-");
                if (v && *v) {
                    if (audio::device::isPlaying()) {
                        audio::device::stopPlayback();
                        store::log("voice", "проигрывание остановлено");
                    } else if (audio::device::play(v)) {
                        store::log("voice", "проигрываю голосовое");
                    } else {
                        ets_printf("[vual] файл голосового НЕ открылся\n");
                        store::log("voice", "не удалось открыть файл голосового");
                    }
                }
                break;
            }
            case ui::SCR_CHATS:
                if (size_t(h.rowIndex) < contacts::count()) openChat(size_t(h.rowIndex));
                break;
            case ui::SCR_MENU:
                if (h.rowIndex < int(ui::MENU_COUNT)) {
                    menuSel_ = ui::MenuItem(h.rowIndex);
                    openMenuItem();
                }
                break;
            case ui::SCR_WIFI:
                if (size_t(h.rowIndex) < wifiCount_) { wifiSel_ = size_t(h.rowIndex); openPassScreen(); }
                break;
            case ui::SCR_IDENTITY: saveIdentity(); break;
            case ui::SCR_LOG:
                store::logClear();
                logPage_ = 0;
                showLogPage();
                break;
            case ui::SCR_WIFI_PASS:
                if (h.rowIndex == 0) {
                    wifiReveal_ = !wifiReveal_;
                    ui::drawWifiPass(wifiSsid_, wifiPass_, wifiReveal_, nullptr);
                } else connectWifi();
                break;
            default: break;
            }
            break;
        default: break;
        }
        break;
    }

    case EV_LONG_PRESS:
        // Удержание — рация. Работает только по Wi-Fi: поток речи требует 8 кбит/с,
        // а радиоканал даёт от 300 бит/с до 5,5 кбит/с.
        break;

    default: break;
    }
}

void setup() {
    Serial.begin(115200);
    // Пауза, чтобы монитор порта успел подключиться: у ESP32-S3 порт создаёт сам чип,
    // и первые строки без неё теряются — а именно они и нужны, когда экран чёрный.
    delay(1500);
    Serial.println();
    Serial.println("=== Вуаль: запуск ===");
    // Штамп — И в аппаратный порт тоже: обычный Serial здесь уходит в USB, а монитор
    // чаще висит на UART. Одна строка отвечает на вопрос «что за прошивка залита».
#ifndef VUAL_BUILD_STAMP
#define VUAL_BUILD_STAMP "без штампа"
#endif
    Serial.printf("сборка: %s\n", VUAL_BUILD_STAMP);
    ets_printf("[vual] сборка: %s\n", VUAL_BUILD_STAMP);
    // Штамп сборки — в порт и (ниже) на экран «Состояние». Отвечает на вечный вопрос
    // «а что вообще прошито?»: без него легко дважды залить один и тот же старый файл
    // и искать несуществующую поломку.
    ets_printf("[vual] сборка: %s %s\n", __DATE__, __TIME__);

    // ── 1. Питание периферии ──────────────────────────────────────────────────────────
    // Первым делом и до всего остального: без него не поднимется ни экран, ни радио, ни
    // клавиатура. Задержка нужна — микроконтроллер клавиатуры грузится сам и не мгновенно.
    pinMode(BOARD_POWERON, OUTPUT);
    digitalWrite(BOARD_POWERON, HIGH);
    delay(500);

    // ── 2. Отключить соседей по шине — ДО экрана ─────────────────────────────────────
    //
    // Экран, карта памяти и радио висят на ОДНОЙ шине. Пока вывод выбора у соседа опущен,
    // тот продолжает её слушать и портит чужой обмен. Именно поэтому экран оставался
    // чёрным: подсветка горела, а команды до контроллера не доходили — карта и радио
    // мешали. Раньше я поднимал эти выводы ПОСЛЕ запуска экрана, то есть слишком поздно.
    pinMode(BOARD_SDCARD_CS, OUTPUT); digitalWrite(BOARD_SDCARD_CS, HIGH);
    pinMode(BOARD_LORA_CS, OUTPUT);   digitalWrite(BOARD_LORA_CS, HIGH);
    pinMode(BOARD_TFT_CS, OUTPUT);    digitalWrite(BOARD_TFT_CS, HIGH);
    delay(20);

    // ── 3. Экран и заставка ───────────────────────────────────────────────────────────
    //
    // Экран поднимаем ПЕРВЫМ из устройств на шине. Библиотека экрана настраивает её сама,
    // и если сделать это до неё, она разбирается с уже занятым блоком — на этом плата
    // и падала при запуске. Радио подключаемся к готовой шине ниже.
    Serial.println("поднимаю экран...");
    boot::begin();
    Serial.println("экран поднят, заставка нарисована");

    boot::step("SPI bus");
    // Библиотека экрана уже настроила выводы; здесь только убеждаемся, что общий объект
    // шины знает о них — радио пользуется тем же.
    radioSpi.begin(BOARD_SPI_SCK, BOARD_SPI_MISO, BOARD_SPI_MOSI);
    boot::done(boot::OK);

    // ── 4. Шина I2C и опрос устройств ────────────────────────────────────────────────
    boot::step("I2C");
    Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
    int found = 0;
    for (uint8_t a = 1; a < 127; ++a) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            Serial.printf("  устройство 0x%02X\n", a);
            ++found;
        }
    }
    if (found > 0) {
        char d[24]; snprintf(d, sizeof(d), "%d dev", found);
        boot::done(boot::OK, d);
    } else {
        // Не останавливаемся: без клавиатуры и сенсора приложение всё равно принимает
        // сообщения по радио, а причину человек уже видит на экране.
        boot::done(boot::WARN, "none");
    }

    // ── 5. Ввод ──────────────────────────────────────────────────────────────────────
    boot::step("keyboard");
    input::keyboard::begin();
    boot::done(boot::OK);

    boot::step("trackball");
    input::trackball::begin();
    boot::done(boot::OK);

    boot::step("touch");
    // Опрашиваем ОДИН раз: повторный вызов — лишняя работа с шиной и, что хуже, разный
    // результат у двух вызовов дал бы противоречивую строку на экране.
    const bool touchOk = input::touch::begin();
    boot::done(touchOk ? boot::OK : boot::WARN, touchOk ? nullptr : "нет");

    // ── 6. Радио ─────────────────────────────────────────────────────────────────────
    boot::step("radio");
    int st = radio.begin(LORA_FREQ_MHZ, LORA_BW_KHZ, LORA_SF, LORA_CR,
                         RADIOLIB_SX126X_SYNC_WORD_PRIVATE,
                         LORA_POWER_DBM, LORA_PREAMBLE, LORA_TCXO_V);
    if (st == RADIOLIB_ERR_NONE) {
        // Радио отозвалось — распиновка верна и модуль на месте. ТОЛЬКО теперь разрешаем
        // передачу: слепая передача при неверных выводах выжигает усилитель необратимо.
        radio.setCurrentLimit(LORA_CURRENT_MA);
        radio.setCRC(true);
        radio.setDio1Action(onRadioIrq);
        radio.startReceive();
        radioReady = true;
        boot::done(boot::OK, "433 MHz");
    } else {
        char d[24]; snprintf(d, sizeof(d), "err %d", st);
        boot::done(boot::FAIL, d);
        // Раньше здесь плата зависала молча. Теперь продолжаем без радио: связи не будет,
        // но интерфейс работает и причина видна. Частые причины — отсутствие TCXO при
        // узкой полосе и расхождение распиновки.
        Serial.println("радио не поднялось — передача запрещена");
    }

    // ── 6б. Карта памяти ─────────────────────────────────────────────────────────────
    boot::step("microSD");
    sdOk_ = SD.begin(BOARD_SDCARD_CS, radioSpi);
    if (sdOk_) {
        store::begin();
        store::log("boot", "запуск");
        // Настоящие эмодзи, если человек положил их на карту в /vual/emoji.
        emoji::scanCard();
        char d[24];
        snprintf(d, sizeof(d), "%llu МБ", SD.cardSize() / (1024ULL * 1024));
        boot::done(boot::OK, d);
    } else {
        // Не останавливаемся: без карты приложение работает, но переписка не переживёт
        // выключение — и человек должен это видеть, а не гадать.
        boot::done(boot::WARN, "нет");
    }

    // ── 6в. Звук ─────────────────────────────────────────────────────────────────────
    boot::step("audio");
    // Без микрофона голосовые не запишутся, но всё остальное работает — не останавливаемся.
    const bool audioOk = audio::device::begin();
    boot::done(audioOk ? boot::OK : boot::WARN, audioOk ? nullptr : "нет");
    if (audioOk) audio::device::bootMelody();   // проверка динамика на слух

    // ── 7. Контакты ──────────────────────────────────────────────────────────────────
    boot::step("contacts");
    contacts::begin();
    // Восстанавливаем личность с карты. Без этого каждая прошивка стирала имя и ключ,
    // а вместе с ними и все знакомства.
    if (sdOk_ && store::loadIdentity(contacts::myNameMutable(), 24,
                                     contacts::myPrivMutable(), 32,
                                     contacts::myPubMutable(), voile::kPubComp)
              && (contacts::markKeysReady(), true)) {
        Serial.printf("личность восстановлена: «%s»\n", contacts::myName());
    }
    // И собеседников тоже: знакомство — это обмен ключами, и «просто познакомиться
    // заново» после каждой перезагрузки нельзя было бы без участия обеих сторон.
    if (sdOk_) contacts::load();
    // Отладочного контакта здесь БОЛЬШЕ НЕТ. Он назывался «Сосед», был подделкой без
    // настоящего собеседника и только сбивал с толку: в списке кто-то есть, а написать
    // ему нельзя. Собеседники добавляются на экране знакомства — меню, «Добавить».
    {
        char d[24];
        snprintf(d, sizeof(d), "%u", unsigned(contacts::count()));
        boot::done(boot::OK, d);
    }

    // ── 8. Интерфейс ─────────────────────────────────────────────────────────────────
    boot::step("UI");
    ui::begin();
    boot::done(boot::OK);

    // Поправка сенсора: без неё палец попадает мимо кнопок — тем сильнее, чем дальше от
    // центра экрана. Сохранённую применяем молча, при первом запуске просим настроить.
    boot::step("сенсор");
    if (touchOk && sdOk_ && loadTouchCal()) {
        vualScreen().setTouchCalibrate(touchCal_);
        haveCal_ = true;
        boot::done(boot::OK, "настроен");
    } else if (touchOk) {
        boot::done(boot::WARN, "нужна настройка");
    } else {
        boot::done(boot::WARN, "нет");
    }

    boot::finish();
    delay(400);                      // дать разглядеть список шагов

    // Мост в обе стороны: из сети в эфир и обратно.
    net::setBridgeToRadio(bridgeNetToRadio);
    net::setOnFrame(onNetFrame);
    xfer::setOnComplete(onFileReady);
    xfer::setRadioSender(radioSendChunk);

    // Сохранённая сеть: подключаемся сами, не спрашивая. Пароль уже введён однажды, и
    // требовать его снова при каждом включении — издевательство. Без этого сохранение
    // паролей было бессмысленным: они записывались и не использовались никогда.
    if (sdOk_) {
        char ssid[33] = {}, pass[64] = {};
        if (store::lastWifi(ssid, sizeof(ssid), pass, sizeof(pass)) && ssid[0]) {
            store::log("wifi", "подключаюсь к сохранённой сети");
            WiFi.begin(ssid, pass[0] ? pass : nullptr);
            for (int i = 0; i < 40 && !WiFi.isConnected(); ++i) delay(200);
            if (WiFi.isConnected()) {
                configTime(0, 0, "pool.ntp.org", "time.google.com");
                net::begin();
                rail::begin(net::myId());
                rail::setOnMessage(onRailMessage);
                nostr::begin(net::myId());
                nostr::setOnMessage(onNostrMessage);
                tracker::begin(net::myId());
                tracker::setOnMessage(onTrackerMessage);
                needExternal_ = true;
                store::log("wifi", "подключено");
            } else {
                store::log("wifi", "не подключилось — сеть недоступна");
            }
        }
    }

    // Первый запуск с работающим сенсором — сразу настраиваем: иначе человек упрётся в
    // не нажимающиеся кнопки и не поймёт почему.
    if (touchOk && !haveCal_) runCalibration();

    refreshPeers();
    ui::draw(ui::SCR_CHATS);
    Serial.println("Вуаль готова");
}

void loop() {
    // Признак от модуля один на приём и передачу — сначала выясняем, наша ли это
    // законченная передача, и только потом читаем как принятый кадр.
    checkTxDone();

    if (packetReady && !txBusy) {
        packetReady = false;
        handlePacketInner();
        // Возврат в приём — безусловный и единственный. Лишний вызов безвреден,
        // пропущенный — глухота до следующей своей передачи.
        radio.startReceive();
    }
    pumpQueue();

    // Запись и воспроизведение голосового подаём порциями из общего цикла, а не в
    // отдельном потоке: так интерфейс продолжает рисоваться, и полоска записи не замирает.
    audio::device::pumpRecording();
    audio::device::pumpPlayback();
    if (chimePending_) { chimePending_ = false; audio::device::chime(); }

    // Запись: обновляем полоску и обрываем по достижении предела. Без предела человек
    // мог бы наговорить сколько угодно, а такой файл не дойдёт.
    if (audio::device::isRecording()) {
        const uint32_t el = millis() - voiceStart_;
        static uint32_t lastDraw = 0;
        if (millis() - lastDraw > 250) { lastDraw = millis(); ui::setRecording(true, el); }
        if (el >= kVoiceMaxMs) {
            store::log("voice", "достигнута минута — заканчиваем");
            stopVoice();
        }
    }

    // Сеть подаём тем же циклом: приём пакетов, рассылка объявлений о себе, поддержание
    // связей. Отдельный поток здесь только отнимал бы память.
    net::pump();
    rail::pump();
    // Передачи файлов: отправка кусков и досылка.
    xfer::pump();

    // Внешний адрес спрашиваем ЗДЕСЬ, отложенно: опрос семи серверов занимает секунды, и
    // делать это в момент подключения значило бы держать экран мёртвым.
    if (needExternal_ && WiFi.isConnected()) {
        needExternal_ = false;
        net::refreshExternalAddress();
        store::log("net", net::externalAddress()[0] ? net::externalAddress()
                                                    : "внешний адрес не определён");
    }

    // Объявляемся в эфире. Без этого две платы молчат обе и не находят друг друга —
    // каждая ждёт, что позовут её.
    //
    // Период обязан быть ЗАМЕТНО больше времени передачи самого объявления. Раньше
    // объявление вставало в очередь каждые 20 секунд, а три его копии занимали эфир
    // около минуты: очередь не опустошалась, плата передавала почти непрерывно — и,
    // поскольку радио полудуплексное, почти никогда не слушала. Обе платы делали это
    // одновременно и потому не слышали друг друга вовсе.
    //
    // Пока идёт знакомство — зовём чаще, в спокойном состоянии — редко. И никогда не
    // ставим новое объявление, пока в очереди ещё лежат неотправленные кадры: догонять
    // самих себя бессмысленно.
    static uint32_t lastHello = 0;
    const uint32_t helloEvery = pairRoom_[0] ? 30000u : 120000u;
    if (radioReady && millis() - lastHello > helloEvery) {
        bool queueBusy = txBusy;
        for (size_t i = 0; !queueBusy && i < kQueueSize; ++i)
            if (queue_[i].used) queueBusy = true;
        if (!queueBusy) {
            lastHello = millis();
            announceRadio();
        }
    }

    // Значки связи в шапке — раз в секунду. Чаще незачем: состояние так быстро не
    // меняется, а перерисовка на каждом круге съедала бы время.
    static uint32_t lastIcons = 0;
    if (millis() - lastIcons > 1000) {
        lastIcons = millis();
        ui::setLinkState(WiFi.isConnected(), radioReady,
                         rail::connected() || nostr::connected() || tracker::connected());
        // Присутствие тухнет само — по молчанию, без событий. Проверка дешёвая и
        // перерисовывает список только при действительной смене.
        refreshPeersIfChanged();

        // Ход перекачки. Приём голосового по радио занимает минуту-другую, и без
        // полоски получатель уверен, что не происходит ничего.
        static bool hadXfer = false;
        const xfer::Progress* p = xfer::activeAt(0);
        if (p && p->total) {
            const int pct = int(uint64_t(p->done) * 100 / p->total);
            ui::setTransfer(pct > 100 ? 100 : pct, !p->outgoing, p->kind == xfer::K_VOICE);
            hadXfer = true;
        } else if (hadXfer) {
            ui::setTransfer(-1, false, false);
            hadXfer = false;
        }
    }

    // Опрашиваем все три источника ввода: клавиатуру, трекбол и сенсор. Каждый хорош в
    // своём, и работают они одновременно.
    input::Event e = input::keyboard::poll();
    if (e.type == input::EV_NONE) e = input::trackball::poll();
    if (e.type == input::EV_NONE) e = input::touch::poll();
    if (e.type != input::EV_NONE) handleEvent(e);

    delay(5);
}
