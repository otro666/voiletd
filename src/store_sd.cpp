#include "store_sd.h"

#include <Arduino.h>
#include <SD.h>
#include <string.h>
#include <time.h>

#include "contacts.h"

namespace store {

namespace {

bool g_ready = false;
uint32_t g_logSize = 0;

constexpr const char* kRoot     = "/vual";
constexpr const char* kIdent    = "/vual/identity.bin";
constexpr const char* kWifi     = "/vual/wifi.txt";
constexpr const char* kLog      = "/vual/vual.log";
constexpr const char* kChats    = "/vual/chats";
constexpr const char* kMedia    = "/vual/media";

/** Журнал не должен съесть карту. При превышении начинаем заново: старое к тому времени
 *  уже неинтересно, а разбирать гигабайт всё равно никто не станет. */
// Журнал — инструмент разбора, а не архив. Двухмегабайтный файл давал заметную задержку
// при открытии экрана даже после ускорения чтения, а пользы от такой глубины нет:
// смотрят всегда последние сотни строк. Четверть мегабайта — это тысячи строк.
constexpr uint32_t kLogMax = 256UL * 1024;

/** Имя собеседника в имени файла: чистим от того, что файловая система не примет. */
void safeName(const char* in, char* out, size_t cap) {
    size_t n = 0;
    for (const char* p = in; *p && n + 1 < cap; ++p) {
        const char c = *p;
        out[n++] = (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                    c == '"' || c == '<' || c == '>' || c == '|') ? '_' : c;
    }
    out[n] = 0;
}

}  // namespace

bool begin() {
    if (!SD.exists(kRoot) && !SD.mkdir(kRoot)) return false;
    SD.mkdir(kChats);
    SD.mkdir(kMedia);

    File f = SD.open(kLog, FILE_READ);
    if (f) { g_logSize = uint32_t(f.size()); f.close(); }

    g_ready = true;
    log("store", "хранилище на карте готово");
    return true;
}

bool ready() { return g_ready; }

// ── личность ───────────────────────────────────────────────────────────────────────────

bool saveIdentity(const char* name, const uint8_t* privKey, size_t privLen,
                  const uint8_t* pubKey, size_t pubLen) {
    if (!g_ready) return false;

    // Пишем во временный файл и переименовываем. Прямая запись поверх при сбое питания
    // оставила бы половину ключа — и личность была бы потеряна безвозвратно.
    const char* tmp = "/vual/identity.tmp";
    SD.remove(tmp);
    File f = SD.open(tmp, FILE_WRITE);
    if (!f) return false;

    const uint8_t ver = 1;
    f.write(&ver, 1);
    const uint8_t nlen = uint8_t(strlen(name));
    f.write(&nlen, 1);
    f.write(reinterpret_cast<const uint8_t*>(name), nlen);
    f.write(privKey, privLen);
    f.write(pubKey, pubLen);
    f.close();

    SD.remove(kIdent);
    const bool ok = SD.rename(tmp, kIdent);
    log("store", ok ? "личность сохранена" : "личность НЕ сохранена");
    return ok;
}

bool loadIdentity(char* name, size_t nameCap,
                  uint8_t* privKey, size_t privLen,
                  uint8_t* pubKey, size_t pubLen) {
    if (!g_ready) return false;
    File f = SD.open(kIdent, FILE_READ);
    if (!f) return false;

    uint8_t ver = 0, nlen = 0;
    bool ok = f.read(&ver, 1) == 1 && ver == 1 && f.read(&nlen, 1) == 1;
    if (ok && nlen < nameCap) {
        ok = f.read(reinterpret_cast<uint8_t*>(name), nlen) == nlen;
        name[nlen] = 0;
        ok = ok && f.read(privKey, privLen) == int(privLen);
        ok = ok && f.read(pubKey, pubLen) == int(pubLen);
    } else ok = false;
    f.close();
    return ok;
}

// ── сети ───────────────────────────────────────────────────────────────────────────────

bool saveWifi(const char* ssid, const char* pass) {
    if (!g_ready || !ssid || !*ssid) return false;

    // Переписываем файл целиком, ставя эту сеть первой: при запуске подключаемся к
    // первой строке, а последняя удачная — самая вероятная и в следующий раз.
    char keep[8][160] = {};
    size_t kept = 0;
    File r = SD.open(kWifi, FILE_READ);
    if (r) {
        while (r.available() && kept < 7) {
            const String line = r.readStringUntil('\n');
            if (line.length() < 2) continue;
            // Свою же запись выбрасываем — она уйдёт наверх.
            if (line.startsWith(String(ssid) + "\t")) continue;
            snprintf(keep[kept++], sizeof(keep[0]), "%s", line.c_str());
        }
        r.close();
    }

    SD.remove(kWifi);
    File f = SD.open(kWifi, FILE_WRITE);
    if (!f) return false;
    f.printf("%s\t%s\n", ssid, pass ? pass : "");
    for (size_t i = 0; i < kept; ++i) f.printf("%s\n", keep[i]);
    f.close();
    log("store", "сеть запомнена");
    return true;
}

bool findWifi(const char* ssid, char* pass, size_t passCap) {
    if (!g_ready) return false;
    File f = SD.open(kWifi, FILE_READ);
    if (!f) return false;
    bool found = false;
    while (f.available()) {
        const String line = f.readStringUntil('\n');
        const int tab = line.indexOf('\t');
        if (tab <= 0) continue;
        if (line.substring(0, tab) != ssid) continue;
        snprintf(pass, passCap, "%s", line.substring(tab + 1).c_str());
        found = true;
        break;
    }
    f.close();
    return found;
}

bool lastWifi(char* ssid, size_t ssidCap, char* pass, size_t passCap) {
    if (!g_ready) return false;
    File f = SD.open(kWifi, FILE_READ);
    if (!f) return false;
    bool ok = false;
    if (f.available()) {
        const String line = f.readStringUntil('\n');
        const int tab = line.indexOf('\t');
        if (tab > 0) {
            snprintf(ssid, ssidCap, "%s", line.substring(0, tab).c_str());
            snprintf(pass, passCap, "%s", line.substring(tab + 1).c_str());
            ok = true;
        }
    }
    f.close();
    return ok;
}

// ── переписка ──────────────────────────────────────────────────────────────────────────

bool appendMessage(const char* peer, bool mine, uint32_t ts, const char* text) {
    if (!g_ready || !peer || !text) return false;
    char nm[40], path[80];
    safeName(peer, nm, sizeof(nm));
    snprintf(path, sizeof(path), "%s/%s.txt", kChats, nm);

    File f = SD.open(path, FILE_APPEND);
    if (!f) return false;
    // Строка на сообщение: разбирается тривиально и переживает обрыв записи — потеряется
    // не больше одной последней строки.
    f.printf("%c\t%lu\t%s\n", mine ? 'M' : 'T', (unsigned long)ts, text);
    f.close();
    return true;
}

size_t loadMessages(const char* peer, size_t maxCount,
                    void (*cb)(bool mine, uint32_t ts, const char* text)) {
    if (!g_ready || !peer || !cb) return 0;
    char nm[40], path[80];
    safeName(peer, nm, sizeof(nm));
    snprintf(path, sizeof(path), "%s/%s.txt", kChats, nm);

    File f = SD.open(path, FILE_READ);
    if (!f) return 0;

    // Читаем всё, но отдаём последние: история может быть длинной, а на экран помещается
    // десяток строк. Держать в памяти всю переписку нельзя — её там просто нет.
    struct Item { bool mine; uint32_t ts; char text[128]; };
    static Item ring[32];
    size_t head = 0, total = 0;
    const size_t cap = maxCount < 32 ? maxCount : 32;

    while (f.available()) {
        const String line = f.readStringUntil('\n');
        if (line.length() < 4) continue;
        const int t1 = line.indexOf('\t');
        const int t2 = line.indexOf('\t', t1 + 1);
        if (t1 < 0 || t2 < 0) continue;
        Item& it = ring[head % cap];
        it.mine = line[0] == 'M';
        it.ts = uint32_t(line.substring(t1 + 1, t2).toInt());
        snprintf(it.text, sizeof(it.text), "%s", line.substring(t2 + 1).c_str());
        ++head; ++total;
    }
    f.close();

    const size_t n = total < cap ? total : cap;
    const size_t start = total < cap ? 0 : head % cap;
    for (size_t i = 0; i < n; ++i) {
        const Item& it = ring[(start + i) % cap];
        cb(it.mine, it.ts, it.text);
    }
    return n;
}

const char* mediaPath(const char* peer, const char* fileName) {
    static char path[120];
    char nm[40];
    safeName(peer, nm, sizeof(nm));
    char dir[80];
    snprintf(dir, sizeof(dir), "%s/%s", kMedia, nm);
    SD.mkdir(dir);
    snprintf(path, sizeof(path), "%s/%s", dir, fileName);
    return path;
}

// ── журнал ─────────────────────────────────────────────────────────────────────────────

void log(const char* tag, const char* text) {
    if (!g_ready) return;

    // Подряд идущие одинаковые строки не пишем, а считаем.
    //
    // Повторяющаяся раз в несколько секунд неудача (например, «визитку собрать не
    // удалось») за час превращает журнал в десятки тысяч одинаковых строк: и файл
    // распухает, и найти в нём что-то другое невозможно. Одна строка со счётчиком
    // несёт ровно ту же весть.
    static char lastTag[16] = {};
    static char lastText[96] = {};
    static uint32_t repeats = 0;
    const bool same = tag && text && strncmp(lastTag, tag, sizeof(lastTag) - 1) == 0 &&
                      strncmp(lastText, text, sizeof(lastText) - 1) == 0;
    if (same) {
        ++repeats;
        // Пишем не каждый повтор, а по круглым числам — чтобы след остался, а файл нет.
        if (repeats != 10 && repeats != 100 && repeats % 1000 != 0) return;
    } else {
        repeats = 0;
        snprintf(lastTag, sizeof(lastTag), "%s", tag ? tag : "");
        snprintf(lastText, sizeof(lastText), "%s", text ? text : "");
    }

    char withCount[160];
    if (repeats) {
        snprintf(withCount, sizeof(withCount), "%s (повторов: %lu)",
                 text ? text : "", (unsigned long)repeats);
        text = withCount;
    }

    if (g_logSize > kLogMax) {
        SD.remove(kLog);
        g_logSize = 0;
    }

    File f = SD.open(kLog, FILE_APPEND);
    if (!f) return;

    // Время от запуска, а не календарное: часы выставляются только после подключения к
    // сети, а разбирать надо в том числе и то, что случилось до него.
    const uint32_t ms = millis();
    const int n = f.printf("[%lu.%03lu] %s: %s\n",
                           (unsigned long)(ms / 1000), (unsigned long)(ms % 1000),
                           tag ? tag : "", text ? text : "");
    if (n > 0) g_logSize += uint32_t(n);
    f.close();
}

uint32_t logSize() { return g_logSize; }

bool logClear() {
    if (!g_ready) return false;
    SD.remove(kLog);
    g_logSize = 0;
    log("store", "журнал очищен");
    return true;
}

size_t logPage(size_t page, size_t perPage, char* buf, size_t bufCap,
               const char** lines, bool* hasMore) {
    if (!g_ready || !buf || !lines || perPage == 0) return 0;
    File f = SD.open(kLog, FILE_READ);
    if (!f) return 0;

    // Считаем строки БЛОКАМИ, а не по байту.
    //
    // Раньше здесь стояло чтение по одному байту за раз, и на каждый байт приходилось
    // обращение к карте. Пока журнал был крошечным, это сходило с рук; на разросшемся
    // файле экран «Журнал» стал открываться по несколько секунд, а то и дольше. Блок в
    // полкилобайта уменьшает число обращений в пятьсот раз при том же результате.
    static uint8_t blk[512];
    size_t total = 0;
    int got = 0;
    while ((got = f.read(blk, sizeof(blk))) > 0)
        for (int i = 0; i < got; ++i) if (blk[i] == '\n') ++total;

    // Страница 0 — конец файла. Отступаем от него на нужное число строк.
    const size_t skipFromStart = (total > (page + 1) * perPage)
                               ? total - (page + 1) * perPage : 0;
    if (hasMore) *hasMore = skipFromStart > 0;

    // Пропуск — тем же блочным проходом: ищем смещение нужной строки и прыгаем на него,
    // вместо того чтобы вычитывать строку за строкой.
    uint32_t offset = 0;
    if (skipFromStart) {
        f.seek(0);
        size_t seen = 0;
        uint32_t base = 0;
        bool found = false;
        while (!found && (got = f.read(blk, sizeof(blk))) > 0) {
            for (int i = 0; i < got; ++i) {
                if (blk[i] != '\n') continue;
                if (++seen == skipFromStart) { offset = base + uint32_t(i) + 1; found = true; break; }
            }
            base += uint32_t(got);
        }
        if (!found) offset = base;
    }
    f.seek(offset);

    size_t used = 0, count = 0;
    while (f.available() && count < perPage && used + 2 < bufCap) {
        const String line = f.readStringUntil('\n');
        if (line.length() == 0) continue;
        const size_t n = size_t(line.length());
        if (used + n + 1 >= bufCap) break;
        memcpy(buf + used, line.c_str(), n);
        buf[used + n] = 0;
        lines[count++] = buf + used;
        used += n + 1;
    }
    f.close();
    return count;
}

size_t logTail(char* buf, size_t bufCap, const char** lines, size_t maxLines) {
    if (!g_ready || !buf || !lines || maxLines == 0) return 0;
    File f = SD.open(kLog, FILE_READ);
    if (!f) return 0;

    // Читаем ХВОСТ файла, а не весь: журнал может быть в мегабайты, и держать его в
    // памяти нечем. Берём с запасом — примерно по сотне байт на строку.
    const uint32_t size = uint32_t(f.size());
    const uint32_t want = uint32_t(maxLines) * 100;
    const uint32_t from = size > want ? size - want : 0;
    f.seek(from);

    size_t used = 0;
    if (from > 0) f.readStringUntil('\n');       // первая строка обрезана — выбрасываем

    size_t count = 0;
    while (f.available() && used + 2 < bufCap) {
        String line = f.readStringUntil('\n');
        if (line.length() == 0) continue;
        const size_t n = size_t(line.length());
        if (used + n + 1 >= bufCap) break;
        memcpy(buf + used, line.c_str(), n);
        buf[used + n] = 0;

        // Кольцом: когда строк больше, чем помещается, старые вытесняются.
        if (count < maxLines) lines[count++] = buf + used;
        else {
            for (size_t i = 0; i + 1 < maxLines; ++i) lines[i] = lines[i + 1];
            lines[maxLines - 1] = buf + used;
        }
        used += n + 1;
    }
    f.close();
    return count;
}

}  // namespace store

// ── контакты ───────────────────────────────────────────────────────────────────────────
//
// Объявлены в contacts.h, а живут здесь: сам модуль контактов проверяется на компьютере,
// где ни карты, ни Arduino нет, и работа с картой в нём сломала бы проверку.
//
// Без сохранения каждая перезагрузка стирала всех собеседников, и знакомиться
// приходилось заново — а знакомство это обмен ключами, его «просто повторить» нельзя.

namespace contacts {

namespace {
constexpr const char* kContactsPath = "/vual/contacts.bin";
constexpr const char* kContactsTmp  = "/vual/contacts.tmp";
constexpr uint8_t kContactsVer = 1;
}  // namespace

bool save() {
    if (!store::ready()) return false;

    // Во временный файл и переименованием — как личность: сбой питания посреди записи
    // не должен уносить ВСЕХ собеседников разом.
    SD.remove(kContactsTmp);
    File f = SD.open(kContactsTmp, FILE_WRITE);
    if (!f) return false;

    f.write(&kContactsVer, 1);
    const uint8_t n = uint8_t(count());
    f.write(&n, 1);
    for (size_t i = 0; i < count(); ++i) {
        Contact* c = at(i);
        if (!c) continue;
        f.write(reinterpret_cast<const uint8_t*>(c->name), kNameLen);
        f.write(c->pubComp, voile::kPubComp);
    }
    f.close();

    SD.remove(kContactsPath);
    const bool ok = SD.rename(kContactsTmp, kContactsPath);
    if (!ok) store::log("contacts", "список НЕ сохранён");
    return ok;
}

bool load() {
    if (!store::ready()) return false;
    File f = SD.open(kContactsPath, FILE_READ);
    if (!f) return false;

    uint8_t ver = 0, n = 0;
    bool ok = f.read(&ver, 1) == 1 && ver == kContactsVer && f.read(&n, 1) == 1;
    size_t loaded = 0;
    for (size_t i = 0; ok && i < n && i < kMaxContacts; ++i) {
        char name[kNameLen];
        uint8_t pub[voile::kPubComp];
        ok = f.read(reinterpret_cast<uint8_t*>(name), kNameLen) == int(kNameLen) &&
             f.read(pub, voile::kPubComp) == int(voile::kPubComp);
        if (!ok) break;
        name[kNameLen - 1] = 0;
        // Через upsert, а не напрямую: он же выводит адрес из ключа, и битая запись
        // с чужим адресом появиться не может.
        if (upsert(name, pub)) ++loaded;
    }
    f.close();

    if (loaded) {
        char msg[48];
        snprintf(msg, sizeof(msg), "восстановлено собеседников: %u", unsigned(loaded));
        store::log("contacts", msg);
    }
    return ok;
}

}  // namespace contacts
