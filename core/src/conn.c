#include "conn.h"

#include "engine.h"  // engine_next_backoff : une seule courbe de réessai

#include <string.h>

// ------------------------------------------------ normalisation d'adresse ---

static u8 lower(u8 c) { return (c >= 'A' && c <= 'Z') ? (u8)(c + 32) : c; }

static b32 eq_ci(Str8 s, const char *lit) {
    isize n = 0;
    while (lit[n]) n++;
    if (s.len != n) return 0;
    for (isize i = 0; i < n; i++) {
        if (lower(s.data[i]) != (u8)lit[i]) return 0;
    }
    return 1;
}

// host_only garde la partie hôte d'une saisie : « exemple.fr:8443/ws?x » →
// « exemple.fr:8443 ».
static Str8 host_only(Str8 s) {
    if (s.len >= 2 && s.data[0] == '/' && s.data[1] == '/') s = str8_sub(s, 2, -1);
    for (isize i = 0; i < s.len; i++) {
        u8 c = s.data[i];
        if (c == '/' || c == '?' || c == '#') return str8_sub(s, 0, i);
    }
    return s;
}

// strip_port retire « :port » comme net.SplitHostPort : une adresse IPv6 nue
// (plusieurs deux-points, sans crochets) est laissée intacte.
static Str8 strip_port(Str8 host) {
    if (host.len > 0 && host.data[0] == '[') {
        isize close = 0;
        while (close < host.len && host.data[close] != ']') close++;
        return str8_sub(host, 1, close - 1);  // contenu des crochets
    }
    isize colons = 0, last = -1;
    for (isize i = 0; i < host.len; i++) {
        if (host.data[i] == ':') {
            colons++;
            last = i;
        }
    }
    if (colons != 1 || last < 0) return host;  // 0 = pas de port, ≥2 = IPv6 nue
    for (isize i = last + 1; i < host.len; i++) {
        if (host.data[i] < '0' || host.data[i] > '9') return host;
    }
    return str8_sub(host, 0, last);
}

b32 conn_is_local_host(Str8 host) {
    Str8 h = strip_port(host);
    return eq_ci(h, "localhost") || eq_ci(h, "127.0.0.1") || eq_ci(h, "::1");
}

b32 conn_normalize_url(Arena *a, Str8 raw, Str8 *out, const char **err) {
    const char *dummy = NULL;
    if (!err) err = &dummy;
    *err = NULL;

    Str8 s = str8_trim(raw);
    if (s.len == 0) {
        *err = "Indiquez l'adresse du serveur (ex. vibesync.exemple.fr).";
        return 0;
    }

    // 1. Schéma : absent → wss, sauf en local où TLS n'a pas lieu d'être.
    Str8 scheme = str8_lit("");
    isize sep = -1;
    for (isize i = 0; i + 2 < s.len; i++) {
        if (s.data[i] == ':' && s.data[i + 1] == '/' && s.data[i + 2] == '/') {
            sep = i;
            break;
        }
    }
    Str8 rest;
    if (sep >= 0) {
        Str8 raw_scheme = str8_sub(s, 0, sep);
        rest = str8_sub(s, sep + 3, -1);
        if (eq_ci(raw_scheme, "ws") || eq_ci(raw_scheme, "http")) scheme = str8_lit("ws");
        else if (eq_ci(raw_scheme, "wss") || eq_ci(raw_scheme, "https")) scheme = str8_lit("wss");
        else {
            *err = "Schéma d'adresse non supporté (attendu ws, wss, http ou https).";
            return 0;
        }
    } else {
        rest = s;
        if (rest.len >= 2 && rest.data[0] == '/' && rest.data[1] == '/') rest = str8_sub(rest, 2, -1);
        scheme = conn_is_local_host(host_only(rest)) ? str8_lit("ws") : str8_lit("wss");
    }

    // 2. Autorité / chemin. Le fragment est jeté, l'userinfo aussi.
    isize cut = rest.len;
    for (isize i = 0; i < rest.len; i++) {
        u8 c = rest.data[i];
        if (c == '/' || c == '?' || c == '#') {
            cut = i;
            break;
        }
    }
    Str8 authority = str8_sub(rest, 0, cut);
    Str8 tail = str8_sub(rest, cut, -1);
    for (isize i = authority.len; i > 0; i--) {  // dernier '@' = fin de l'userinfo
        if (authority.data[i - 1] == '@') {
            authority = str8_sub(authority, i, -1);
            break;
        }
    }
    if (authority.len == 0) {
        *err = "Adresse sans nom d'hôte.";
        return 0;
    }
    for (isize i = 0; i < authority.len; i++) {
        u8 c = authority.data[i];
        if (c == ' ' || c == '\\') {
            *err = "Adresse invalide : caractère interdit dans le nom d'hôte.";
            return 0;
        }
    }

    // Fragment retiré ; chemin absent ou « / » → /ws.
    isize frag = tail.len;
    for (isize i = 0; i < tail.len; i++) {
        if (tail.data[i] == '#') {
            frag = i;
            break;
        }
    }
    tail = str8_sub(tail, 0, frag);
    Str8 path = tail, query = str8_lit("");
    for (isize i = 0; i < tail.len; i++) {
        if (tail.data[i] == '?') {
            path = str8_sub(tail, 0, i);
            query = str8_sub(tail, i, -1);
            break;
        }
    }
    if (path.len == 0 || (path.len == 1 && path.data[0] == '/')) path = str8_lit("/ws");

    Builder b;
    builder_init(&b, a, 128);
    builder_str(&b, scheme);
    builder_cstr(&b, "://");
    // L'hôte est mis en minuscules, le chemin non (il peut être sensible à la casse).
    for (isize i = 0; i < authority.len; i++) builder_byte(&b, lower(authority.data[i]));
    builder_str(&b, path);
    builder_str(&b, query);
    *out = builder_result(&b);
    return 1;
}

// --------------------------------------------------- politique de connexion ---

void conn_reset(Conn *c) { memset(c, 0, sizeof(*c)); }

void conn_start(Conn *c, i64 now_ns) {
    c->phase = CONN_TRYING;
    c->backoff_ns = 0;
    c->next_attempt_ns = now_ns;
    c->attempts = 0;
}

void conn_on_open(Conn *c) {
    c->phase = CONN_OPEN;
    c->backoff_ns = 0;
}

void conn_on_socket_down(Conn *c, i64 now_ns) {
    // Refusé ou à l'arrêt : la socket qui tombe est la CONSÉQUENCE du refus,
    // pas une panne. Ne pas reprogrammer de tentative — c'était la boucle.
    if (c->phase == CONN_REFUSED || c->phase == CONN_IDLE) return;
    c->backoff_ns = engine_next_backoff(c->backoff_ns);
    c->next_attempt_ns = now_ns + c->backoff_ns;
    c->phase = CONN_WAITING;
}

void conn_on_refused(Conn *c) {
    c->phase = CONN_REFUSED;
    c->backoff_ns = 0;
    c->next_attempt_ns = 0;
}

void conn_cancel(Conn *c) {
    c->phase = CONN_IDLE;
    c->backoff_ns = 0;
    c->next_attempt_ns = 0;
}

b32 conn_should_attempt(const Conn *c, i64 now_ns) {
    if (c->phase != CONN_TRYING && c->phase != CONN_WAITING) return 0;
    return now_ns >= c->next_attempt_ns;
}

void conn_attempt_started(Conn *c) {
    c->phase = CONN_TRYING;
    c->attempts++;
}

b32 conn_is_busy(const Conn *c) { return c->phase == CONN_TRYING || c->phase == CONN_WAITING; }

i64 conn_seconds_until_retry(const Conn *c, i64 now_ns) {
    if (c->phase != CONN_WAITING) return 0;
    i64 left = c->next_attempt_ns - now_ns;
    if (left <= 0) return 0;
    return (left + 999999999) / 1000000000;
}
