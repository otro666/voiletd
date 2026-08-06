#include "relays.h"

namespace relays {

// ── определение внешнего адреса ────────────────────────────────────────────────────────
// Российские первыми: доступны там, где зарубежные могут быть закрыты, и отвечают быстрее.
const HostPort kStun[] = {
    {"stun.sipnet.ru",          3478},
    {"stun.sipnet.net",         3478},
    {"stun.zadarma.com",        3478},
    {"stun.l.google.com",      19302},
    {"stun.cloudflare.com",     3478},
    {"stun.nextcloud.com",       443},
    {"stun.miwifi.com",         3478},
};
const size_t kStunCount = sizeof(kStun) / sizeof(kStun[0]);

// ── рельса знакомства ──────────────────────────────────────────────────────────────────
//
// Порты и пути ТЕ ЖЕ, что в телефонной версии: веб-сокет поверх защищённого соединения.
// Сперва я взял обычный защищённый порт — проще, но неверно: телефон ходит именно через
// веб-сокет, и совпадения темы мало, надо приходить туда же тем же способом.
const HostPort kBrokers[] = {
    {"broker.emqx.io",          8084},
    {"broker-cn.emqx.io",       8084},
    {"broker.hivemq.com",       8884},
    {"test.mosquitto.org",      8081},
    {"public.mqtthq.com",       8084},
    {"mqtt.eclipseprojects.io",  443},
};
/** Путь у всех один — так принято у брокеров, работающих через веб-сокет. */
const char* const kBrokerPath = "/mqtt";
const size_t kBrokerCount = sizeof(kBrokers) / sizeof(kBrokers[0]);

// ── вторая рельса ──────────────────────────────────────────────────────────────────────
// Нужна как запасной путь: брокеры знакомства и узлы Nostr закрывают разными способами,
// и когда не работает одно, обычно работает другое.
const char* const kNostr[] = {
    // основные
    "relay.damus.io", "nos.lol", "relay.primal.net", "relay.nostr.band",
    "nostr.mom", "offchain.pub", "relay.snort.social", "relay.mostr.pub",
    "nostr.oxtr.dev", "relay.nostr.bg",
    // запасные
    "nostr-pub.wellorder.net", "relay.nostrplebs.com", "nostr.bitcoiner.social",
    "relay.nostrich.de", "nostr.fmt.wiz.biz", "relay.nostr.wirednet.jp",
    "nostr21.com", "nostr.land", "relay.exit.pub", "nostr.data.haus",
    "relay.f7z.io", "nostr.milou.lol", "relay.nostr.com.au",
    "nostr.thank.eu", "relay.plebstr.com",
};
const size_t kNostrCount = sizeof(kNostr) / sizeof(kNostr[0]);

// ── трекеры ────────────────────────────────────────────────────────────────────────────
// Порядок из телефонной версии: сперва проверенная четвёрка, дальше остальные. Мёртвые
// пропускаются сами — на них просто не удаётся подключиться, и ячейка идёт к следующему.
const Tracker kTrackers[] = {
    {"tracker.openwebtorrent.com",     443, "/"},
    {"tracker.webtorrent.dev",         443, "/"},
    {"tracker.btorrent.xyz",           443, "/"},
    {"tracker.files.fm",              7073, "/announce"},
    {"tracker.novage.com.ua",          443, "/"},
    {"tracker.ghostchu-services.top",  443, "/announce"},
    {"tracker.magnetoo.io",            443, "/"},
    {"tracker.fastcast.nz",            443, "/"},
    {"qot.abiir.top",                  443, "/announce"},
    {"tracker.tbd.wtf",                443, "/announce"},
    {"tracker.tvpirat.online",         443, "/announce"},
    {"tracker.webtorrent.io",          443, "/"},
    {"tracker.openwebtorrent.com",     443, "/announce"},
    {"tracker.files.fm",              7072, "/announce"},
    // Ниже — те, что в телефонной версии заданы обычным протоколом. Здесь они тоже через
    // веб-сокет: плата умеет только его, а эти узлы отвечают и так.
    {"tracker.gbitt.info",             443, "/announce"},
    {"tracker.opentrackr.org",         443, "/announce"},
    {"tracker.torrent.eu.org",         443, "/announce"},
    {"opentracker.i2p.rocks",          443, "/announce"},
};
const size_t kTrackerCount = sizeof(kTrackers) / sizeof(kTrackers[0]);

// ── уведомления ────────────────────────────────────────────────────────────────────────
// Оба опрашиваются сразу, побеждает первый ответивший.
const char* const kPush[] = {
    "https://functions.yandexcloud.net/d4end13c1qk57sdvqdbh",
    "https://vual-push.vual.workers.dev",
};
const size_t kPushCount = sizeof(kPush) / sizeof(kPush[0]);

}  // namespace relays
