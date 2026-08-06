// test_win32.c — moitié PLATEFORME de la suite de tests (ADR-010, VS-030).
//
// Tout ce qui exige Windows : conversions UTF-16, analyse d'URL de net.c,
// WebSocket réel contre un mini serveur local, fichier ini sur disque, GDI/UI,
// recherche de médias sur une VRAIE arborescence, DPAPI, faux VLC HTTP sur
// socket. La logique portable — vecteurs de conformité compris — est dans
// test_core.c.

#include "conn.h"
#include "engine.h"
#include "health.h"
#include "ini.h"
#include "json.h"
#include "media.h"
#include "net.h"
#include "protocol.h"
#include "secret.h"
#include "test_util.h"
#include "ui.h"
#include "vlc.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ------------------------------------------------------------ base ---

// Conversions UTF-8 ↔ UTF-16 : elles n'existent que du côté système, la
// couche commune ne manipule que de l'UTF-8 (ADR-010).
static void test_base_platform(Arena *a) {
    section("base");
    TempArena t = temp_begin(a);
    Str8 src = S("caf\xc3\xa9 \xf0\x9f\x98\x80 fin");  // « café 😀 fin »
    isize wlen = 0;
    u16 *w = utf8_to_utf16(a, src, &wlen);
    // « café 😀 fin » = 11 unités UTF-16 (l'émoji en occupe deux).
    CHECK(wlen == 11, "longueur UTF-16 = %lld", (long long)wlen);
    CHECK(w[5] == 0xd83d && w[6] == 0xde00, "paire de substitution mal encodée");
    Str8 back = utf16_to_utf8(a, w);
    CHECK(str8_eq(back, src), "aller-retour UTF-8/16 cassé");
    u16 lone[] = {0xd800, 'a', 0};
    Str8 repl = utf16_to_utf8(a, lone);
    CHECK(repl.len == 4 && repl.data[0] == 0xef, "substitut orphelin non remplacé");
    temp_end(t);
}

// ------------------------------------------------------------ net : URL ---

static void test_net_url(void) {
    section("net");
    NetUrl u;
    CHECK(net_parse_url(S("ws://localhost:8080/ws"), &u) && !u.secure && u.port == 8080 &&
              strcmp(u.host, "localhost") == 0 && strcmp(u.path, "/ws") == 0,
          "ws:// avec port");
    CHECK(net_parse_url(S("wss://vibesync.example.com/ws"), &u) && u.secure && u.port == 443, "wss:// par défaut 443");
    CHECK(net_parse_url(S("ws://host"), &u) && u.port == 80 && strcmp(u.path, "/") == 0, "chemin par défaut");
    CHECK(net_parse_url(S("wss://[::1]:9000/ws"), &u) && u.port == 9000 && strcmp(u.host, "::1") == 0, "IPv6");
    CHECK(net_parse_url(S("https://x/y?z=1"), &u) && u.secure && strcmp(u.path, "/y?z=1") == 0, "query conservée");
    CHECK(!net_parse_url(S("ftp://x/"), &u), "schéma inconnu accepté");
    CHECK(!net_parse_url(S("ws://"), &u), "hôte vide accepté");
    CHECK(!net_parse_url(S("ws://h:0/"), &u), "port 0 accepté");
    CHECK(!net_parse_url(S("ws://h:99999/"), &u), "port hors bornes accepté");
    CHECK(!net_parse_url(S("ws://user:pass@h/"), &u), "identifiants dans l'URL acceptés");
    CHECK(!net_parse_url(S(""), &u), "URL vide acceptée");
}

// ------------------------------------------- mini serveur WebSocket local ---
//
// Serveur RFC 6455 minimal (handshake + trames texte) pour exercer net.c pour
// de vrai : cycle connexion/envoi/réception/fermeture, saturation de la file,
// et surtout arrêt CONCURRENT d'un envoi — le scénario des deux bloquants de
// la revue. Aucune dépendance : Winsock + SHA-1 maison.

typedef struct {
    u32 h[5];
    u64 bits;
    u8 buf[64];
    isize buf_len;
} Sha1;

static u32 rol32(u32 v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_block(Sha1 *s, const u8 *p) {
    u32 w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((u32)p[i * 4] << 24) | ((u32)p[i * 4 + 1] << 16) | ((u32)p[i * 4 + 2] << 8) | p[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a1 = s->h[0], b1 = s->h[1], c1 = s->h[2], d1 = s->h[3], e1 = s->h[4];
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20) {
            f = (b1 & c1) | (~b1 & d1);
            k = 0x5a827999u;
        } else if (i < 40) {
            f = b1 ^ c1 ^ d1;
            k = 0x6ed9eba1u;
        } else if (i < 60) {
            f = (b1 & c1) | (b1 & d1) | (c1 & d1);
            k = 0x8f1bbcdcu;
        } else {
            f = b1 ^ c1 ^ d1;
            k = 0xca62c1d6u;
        }
        u32 tmp = rol32(a1, 5) + f + e1 + k + w[i];
        e1 = d1;
        d1 = c1;
        c1 = rol32(b1, 30);
        b1 = a1;
        a1 = tmp;
    }
    s->h[0] += a1;
    s->h[1] += b1;
    s->h[2] += c1;
    s->h[3] += d1;
    s->h[4] += e1;
}

static void sha1_init(Sha1 *s) {
    s->h[0] = 0x67452301u;
    s->h[1] = 0xefcdab89u;
    s->h[2] = 0x98badcfeu;
    s->h[3] = 0x10325476u;
    s->h[4] = 0xc3d2e1f0u;
    s->bits = 0;
    s->buf_len = 0;
}

static void sha1_update(Sha1 *s, const u8 *data, isize len) {
    s->bits += (u64)len * 8;
    while (len > 0) {
        isize take = VS_MIN(64 - s->buf_len, len);
        memcpy(s->buf + s->buf_len, data, (size_t)take);
        s->buf_len += take;
        data += take;
        len -= take;
        if (s->buf_len == 64) {
            sha1_block(s, s->buf);
            s->buf_len = 0;
        }
    }
}

static void sha1_final(Sha1 *s, u8 out[20]) {
    u64 bits = s->bits;
    u8 pad = 0x80;
    sha1_update(s, &pad, 1);
    u8 zero = 0;
    while (s->buf_len != 56) sha1_update(s, &zero, 1);
    u8 len_be[8];
    for (int i = 0; i < 8; i++) len_be[i] = (u8)(bits >> (56 - 8 * i));
    s->bits = bits;  // sha1_update ci-dessous ne doit pas fausser le compte
    sha1_update(s, len_be, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (u8)(s->h[i] >> 24);
        out[i * 4 + 1] = (u8)(s->h[i] >> 16);
        out[i * 4 + 2] = (u8)(s->h[i] >> 8);
        out[i * 4 + 3] = (u8)s->h[i];
    }
}

typedef struct {
    SOCKET listener;
    int port;
    HANDLE thread;
    volatile long stop;
    volatile long accepted;
    volatile long received;
    volatile long echo;        // renvoyer chaque message reçu
    volatile long flood;       // nombre de messages à pousser dès la connexion
    volatile long flood_size;  // taille de chaque message poussé
    volatile long drop_after;  // fermer brutalement après N messages reçus
    // Départ volontaire (VS-028) : ce que le serveur a VU arriver. C'est la
    // seule preuve automatisable que la trame de fermeture est bien partie
    // avant que le client ne démonte sa socket.
    volatile long saw_close;
    volatile long close_status;
} MiniWs;

static b32 sock_send_all(SOCKET s, const u8 *data, isize len) {
    isize sent = 0;
    while (sent < len) {
        int n = send(s, (const char *)data + sent, (int)(len - sent), 0);
        if (n <= 0) return 0;
        sent += n;
    }
    return 1;
}

static b32 sock_recv_exact(SOCKET s, u8 *out, isize len) {
    isize got = 0;
    while (got < len) {
        int n = recv(s, (char *)out + got, (int)(len - got), 0);
        if (n <= 0) return 0;
        got += n;
    }
    return 1;
}

static b32 ws_send_text_frame(SOCKET s, const u8 *data, isize len) {
    u8 hdr[10];
    isize n = 0;
    hdr[0] = 0x81;  // FIN + texte
    if (len < 126) {
        hdr[1] = (u8)len;
        n = 2;
    } else if (len <= 0xffff) {
        hdr[1] = 126;
        hdr[2] = (u8)(len >> 8);
        hdr[3] = (u8)len;
        n = 4;
    } else {
        hdr[1] = 127;
        for (int i = 0; i < 8; i++) hdr[2 + i] = (u8)((u64)len >> (56 - 8 * i));
        n = 10;
    }
    if (!sock_send_all(s, hdr, n)) return 0;
    return sock_send_all(s, data, len);
}

// ws_recv_frame lit une trame masquée du client. opcode reçoit l'opcode.
static b32 ws_recv_frame(SOCKET s, u8 *out, isize cap, isize *out_len, int *opcode) {
    u8 h[2];
    if (!sock_recv_exact(s, h, 2)) return 0;
    *opcode = h[0] & 0x0f;
    b32 masked = (h[1] & 0x80) != 0;
    u64 len = h[1] & 0x7f;
    if (len == 126) {
        u8 e[2];
        if (!sock_recv_exact(s, e, 2)) return 0;
        len = ((u64)e[0] << 8) | e[1];
    } else if (len == 127) {
        u8 e[8];
        if (!sock_recv_exact(s, e, 8)) return 0;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | e[i];
    }
    u8 mask[4] = {0, 0, 0, 0};
    if (masked && !sock_recv_exact(s, mask, 4)) return 0;
    if (len > (u64)cap) return 0;
    if (len > 0 && !sock_recv_exact(s, out, (isize)len)) return 0;
    if (masked) {
        for (u64 i = 0; i < len; i++) out[i] ^= mask[i & 3];
    }
    *out_len = (isize)len;
    return 1;
}

static b32 ws_handshake(SOCKET s, Arena *a) {
    u8 req[4096];
    isize len = 0;
    for (;;) {
        if (len >= (isize)sizeof(req)) return 0;
        int n = recv(s, (char *)req + len, (int)(sizeof(req) - (size_t)len), 0);
        if (n <= 0) return 0;
        len += n;
        if (len >= 4 && memcmp(req + len - 4, "\r\n\r\n", 4) == 0) break;
    }
    Str8 head = str8(req, len);
    Str8 tag = str8_lit("Sec-WebSocket-Key:");
    isize at = -1;
    for (isize i = 0; i + tag.len <= head.len; i++) {
        if (memcmp(head.data + i, tag.data, (size_t)tag.len) == 0) {
            at = i + tag.len;
            break;
        }
    }
    if (at < 0) return 0;
    isize end = at;
    while (end < head.len && head.data[end] != '\r' && head.data[end] != '\n') end++;
    Str8 key = str8_trim(str8_sub(head, at, end - at));

    Str8 magic = str8_lit("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    Sha1 sh;
    sha1_init(&sh);
    sha1_update(&sh, key.data, key.len);
    sha1_update(&sh, magic.data, magic.len);
    u8 digest[20];
    sha1_final(&sh, digest);
    char accept[64];
    base64_encode(digest, 20, accept, (isize)sizeof(accept));

    Builder resp;
    builder_init(&resp, a, 256);
    builder_cstr(&resp, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n");
    builder_cstr(&resp, "Sec-WebSocket-Accept: ");
    builder_cstr(&resp, accept);
    builder_cstr(&resp, "\r\n\r\n");
    Str8 out = builder_result(&resp);
    return sock_send_all(s, out.data, out.len);
}

static DWORD WINAPI mini_ws_thread(LPVOID param) {
    MiniWs *srv = (MiniWs *)param;
    Arena *a = arena_create(VS_MB(2));
    if (!a) return 1;
    while (!InterlockedCompareExchange(&srv->stop, 0, 0)) {
        SOCKET c = accept(srv->listener, NULL, NULL);
        if (c == INVALID_SOCKET) break;
        InterlockedIncrement(&srv->accepted);
        isize mark = arena_pos(a);
        if (!ws_handshake(c, a)) {
            closesocket(c);
            arena_pop_to(a, mark);
            continue;
        }
        long flood = InterlockedCompareExchange(&srv->flood, 0, 0);
        if (flood > 0) {
            long size = InterlockedCompareExchange(&srv->flood_size, 0, 0);
            if (size < 16) size = 16;
            u8 *payload = arena_push_array(a, u8, size);
            memset(payload, 'x', (size_t)size);
            payload[0] = '{';
            payload[size - 1] = '}';
            for (long i = 0; i < flood; i++) {
                if (!ws_send_text_frame(c, payload, size)) break;
            }
        }
        u8 *buf = arena_push_array(a, u8, VS_KB(128));
        for (;;) {
            if (InterlockedCompareExchange(&srv->stop, 0, 0)) break;
            isize n = 0;
            int opcode = 0;
            if (!ws_recv_frame(c, buf, VS_KB(128), &n, &opcode)) break;
            if (opcode == 0x8) {  // close : on relève le code (2 octets, gros-boutiste)
                if (n >= 2) InterlockedExchange(&srv->close_status, ((long)buf[0] << 8) | (long)buf[1]);
                InterlockedIncrement(&srv->saw_close);
                break;
            }
            if (opcode != 0x1 && opcode != 0x2 && opcode != 0x0) continue;
            long count = InterlockedIncrement(&srv->received);
            long drop = InterlockedCompareExchange(&srv->drop_after, 0, 0);
            if (drop > 0 && count >= drop) break;  // fermeture brutale
            if (InterlockedCompareExchange(&srv->echo, 0, 0)) {
                if (!ws_send_text_frame(c, buf, n)) break;
            }
        }
        closesocket(c);
        arena_pop_to(a, mark);
    }
    arena_destroy(a);
    return 0;
}

static b32 mini_ws_start(MiniWs *srv) {
    memset(srv, 0, sizeof(*srv));
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    srv->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv->listener == INVALID_SOCKET) return 0;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(srv->listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) return 0;
    int alen = (int)sizeof(addr);
    if (getsockname(srv->listener, (struct sockaddr *)&addr, &alen) != 0) return 0;
    srv->port = ntohs(addr.sin_port);
    if (listen(srv->listener, 8) != 0) return 0;
    srv->thread = CreateThread(NULL, 0, mini_ws_thread, srv, 0, NULL);
    return srv->thread != NULL;
}

static void mini_ws_stop(MiniWs *srv) {
    InterlockedExchange(&srv->stop, 1);
    if (srv->listener != INVALID_SOCKET) {
        closesocket(srv->listener);
        srv->listener = INVALID_SOCKET;
    }
    if (srv->thread) {
        WaitForSingleObject(srv->thread, 5000);
        CloseHandle(srv->thread);
        srv->thread = NULL;
    }
}

static Str8 mini_ws_url(Arena *a, const MiniWs *srv) {
    Builder b;
    builder_init(&b, a, 64);
    builder_cstr(&b, "ws://127.0.0.1:");
    builder_i64(&b, srv->port);
    builder_cstr(&b, "/ws");
    return builder_result(&b);
}

// wait_event attend un événement précis (ms max) en vidant la file.
static b32 wait_event(Net *net, NetSlot *slot, NetEventKind want, int timeout_ms) {
    i64 deadline = vs_now_ns() + (i64)timeout_ms * 1000000LL;
    for (;;) {
        while (net_poll(net, slot)) {
            if (slot->kind == want) return 1;
            if (slot->kind == NET_EV_ERROR || slot->kind == NET_EV_CLOSED) {
                if (want != NET_EV_ERROR && want != NET_EV_CLOSED) return 0;
            }
        }
        if (vs_now_ns() > deadline) return 0;
        HANDLE h = (HANDLE)net_wakeup_handle(net);
        if (h) WaitForSingleObject(h, 20);
        else Sleep(5);
    }
}

typedef struct {
    Net *net;
    volatile long stop;
    volatile long sent;
} Spammer;

// spam_thread martèle net_send_text pendant que le thread principal ferme :
// c'est le scénario de course « fermeture serveur pendant un envoi ».
static DWORD WINAPI spam_thread(LPVOID param) {
    Spammer *sp = (Spammer *)param;
    Str8 msg = str8_lit("{\"type\":\"ping\",\"data\":{\"t\":1785960000000}}");
    while (!InterlockedCompareExchange(&sp->stop, 0, 0)) {
        if (net_send_text(sp->net, msg)) InterlockedIncrement(&sp->sent);
    }
    return 0;
}

static void test_net_live(Arena *a) {
    section("net (cycle réel)");
    TempArena top = temp_begin(a);
    MiniWs srv;
    if (!mini_ws_start(&srv)) {
        failf("mini serveur WebSocket indisponible");
        temp_end(top);
        return;
    }
    InterlockedExchange(&srv.echo, 1);
    Str8 url = mini_ws_url(a, &srv);
    Net *net = arena_push_struct(a, Net);
    NetSlot *slot = arena_push_struct(a, NetSlot);

    CHECK(net_init(net), "net_init");

    // 1. Trois cycles connexion → envoi → écho → fermeture : pas de second
    //    thread réseau, pas de handle qui fuit, reconnexion propre.
    for (int cycle = 0; cycle < 3; cycle++) {
        CHECK(net_connect(net, url), "cycle %d : net_connect", cycle);
        CHECK(wait_event(net, slot, NET_EV_CONNECTED, 5000), "cycle %d : pas de NET_EV_CONNECTED", cycle);
        CHECK(net_state(net) == NET_STATE_OPEN, "cycle %d : état != OPEN", cycle);
        Str8 msg = str8_lit("{\"type\":\"hello\",\"data\":{\"version\":1}}");
        CHECK(net_send_text(net, msg), "cycle %d : envoi", cycle);
        CHECK(wait_event(net, slot, NET_EV_MESSAGE, 5000), "cycle %d : pas d'écho", cycle);
        CHECK(slot->len == msg.len && memcmp(slot->data, msg.data, (size_t)msg.len) == 0,
              "cycle %d : écho altéré (%lld octets)", cycle, (long long)slot->len);
        net_close(net);
        CHECK(net_state(net) == NET_STATE_DEAD, "cycle %d : état != DEAD après close", cycle);
        CHECK(!net_send_text(net, msg), "cycle %d : envoi accepté après fermeture", cycle);
    }
    CHECK(srv.accepted == 3, "%ld connexions acceptées, attendu 3", (long)srv.accepted);

    // 2. Fermeture CONCURRENTE d'un envoi, 100 itérations : c'est le scénario
    //    du bloquant nº2 (data race / double close sur les handles WinHTTP).
    {
        int stress = 100;
        int connected = 0;
        for (int i = 0; i < stress; i++) {
            if (!net_connect(net, url)) continue;
            Spammer sp;
            memset(&sp, 0, sizeof(sp));
            sp.net = net;
            HANDLE th = CreateThread(NULL, 0, spam_thread, &sp, 0, NULL);
            if (wait_event(net, slot, NET_EV_CONNECTED, 3000)) connected++;
            Sleep(i % 3);  // fenêtres de course variées
            net_close(net);
            InterlockedExchange(&sp.stop, 1);
            if (th) {
                WaitForSingleObject(th, 5000);
                CloseHandle(th);
            }
            CHECK(net_state(net) == NET_STATE_DEAD, "stress %d : état != DEAD", i);
            while (net_poll(net, slot)) { /* vidange */ }
        }
        CHECK(connected > stress / 2, "seulement %d/%d connexions établies sous stress", connected, stress);
        printf("  stress fermeture/envoi : %d itérations, %d connectées\n", stress, connected);
    }

    // 3. Fermeture brutale par le serveur : la perte est signalée, jamais tue.
    {
        InterlockedExchange(&srv.drop_after, 1);
        CHECK(net_connect(net, url), "connexion pour fermeture brutale");
        if (wait_event(net, slot, NET_EV_CONNECTED, 5000)) {
            net_send_text(net, str8_lit("{\"type\":\"ping\",\"data\":{\"t\":1}}"));
            b32 signaled = 0;
            i64 deadline = vs_now_ns() + 5000LL * 1000000LL;
            while (vs_now_ns() < deadline && !signaled) {
                while (net_poll(net, slot)) {
                    if (slot->kind == NET_EV_ERROR || slot->kind == NET_EV_CLOSED) signaled = 1;
                }
                Sleep(10);
            }
            CHECK(signaled, "coupure serveur non remontée");
        }
        net_close(net);
        InterlockedExchange(&srv.drop_after, 0);
    }

    // 4. DÉPART VOLONTAIRE (VS-028) : net_close_graceful doit faire partir une
    //    trame de fermeture 1000 que le serveur voit arriver, puis laisser le
    //    thread réseau se retirer de lui-même — c'est lui, et lui seul, qui
    //    ferme ses handles WinHTTP (revue terra).
    //    Serveur DÉDIÉ : les sections précédentes ferment aussi des sockets, et
    //    une trame en retard y fausserait le compte.
    MiniWs bye;
    if (!mini_ws_start(&bye)) {
        failf("mini serveur WebSocket (départ volontaire) indisponible");
    } else {
        Str8 bye_url = mini_ws_url(a, &bye);
        CHECK(net_connect(net, bye_url), "connexion pour départ volontaire");
        if (wait_event(net, slot, NET_EV_CONNECTED, 5000)) {
            i64 t0 = vs_now_ns();
            net_close_graceful(net, NET_CLOSE_GRACE_MS);
            i64 elapsed_ms = (vs_now_ns() - t0) / 1000000LL;
            CHECK(net_state(net) == NET_STATE_DEAD, "état != DEAD après départ volontaire");
            // Le repli dur ne doit pas être le chemin nominal : la sortie du
            // thread se compte en millisecondes, pas en délai de grâce écoulé.
            CHECK(elapsed_ms < NET_CLOSE_GRACE_MS + 2000, "fermeture volontaire trop lente (%lld ms)",
                  (long long)elapsed_ms);
            // Le serveur peut mettre un instant à lire la trame.
            i64 deadline = vs_now_ns() + 3000LL * 1000000LL;
            while (vs_now_ns() < deadline && !InterlockedCompareExchange(&bye.saw_close, 0, 0)) Sleep(10);
            long seen = InterlockedCompareExchange(&bye.saw_close, 0, 0);
            long status = InterlockedCompareExchange(&bye.close_status, 0, 0);
            CHECK(seen == 1, "%ld trame(s) de fermeture vue(s) par le serveur, attendu 1", seen);
            CHECK(status == 1000, "code de fermeture = %ld, attendu 1000", status);
        }
        // Idempotence : un second appel sur une connexion déjà morte ne doit ni
        // bloquer ni rouvrir quoi que ce soit.
        net_close_graceful(net, NET_CLOSE_GRACE_MS);
        CHECK(net_state(net) == NET_STATE_DEAD, "second départ volontaire : état != DEAD");
        while (net_poll(net, slot)) { /* vidange */ }
        mini_ws_stop(&bye);
    }

    net_destroy(net);
    mini_ws_stop(&srv);
    temp_end(top);
}

static void test_net_queue_saturation(Arena *a) {
    section("net (saturation de file)");
    TempArena top = temp_begin(a);
    MiniWs srv;
    if (!mini_ws_start(&srv)) {
        failf("mini serveur WebSocket indisponible");
        temp_end(top);
        return;
    }
    // Beaucoup plus que ce que l'arène de file peut contenir : le consommateur
    // ne vide rien pendant ce temps.
    InterlockedExchange(&srv.flood_size, 32768);
    InterlockedExchange(&srv.flood, 400);
    Str8 url = mini_ws_url(a, &srv);
    Net *net = arena_push_struct(a, Net);
    NetSlot *slot = arena_push_struct(a, NetSlot);
    CHECK(net_init(net), "net_init");
    CHECK(net_connect(net, url), "net_connect");

    Sleep(1500);  // le serveur inonde, personne ne consomme

    isize messages = 0, errors = 0, queue_full = 0;
    while (net_poll(net, slot)) {
        if (slot->kind == NET_EV_MESSAGE) {
            messages++;
            // Aucune troncature silencieuse : les messages restent entiers.
            CHECK(slot->len == 32768, "message tronqué (%lld octets)", (long long)slot->len);
            if (slot->len == 32768) {
                CHECK(slot->data[0] == '{' && slot->data[slot->len - 1] == '}', "message corrompu");
            }
        } else if (slot->kind == NET_EV_ERROR) {
            errors++;
            if (slot->code == NET_ERR_QUEUE_FULL) queue_full++;
        }
    }
    printf("  %lld messages reçus, %lld erreurs (dont %lld saturation)\n", (long long)messages,
           (long long)errors, (long long)queue_full);
    CHECK(messages > 50, "file trop petite : seulement %lld messages avant saturation", (long long)messages);
    CHECK(queue_full > 0, "saturation non signalée explicitement");
    CHECK(net->dropped == 0, "%lld événement(s) perdus silencieusement", (long long)net->dropped);

    net_destroy(net);
    mini_ws_stop(&srv);
    temp_end(top);
}

// ------------------------------------------------------------ ini : fichier ---

static void test_ini_file(Arena *a) {
    section("ini");
    TempArena top = temp_begin(a);
    // Le format et les règles d'édition sont vérifiés côté portable ; ici on
    // ne teste que l'aller-retour disque, sur un contenu accentué.
    Ini ini;
    ini_parse(a, S("serveur = wss://vibesync.exemple.fr/ws \r\npseudo=Thibault Éloïse\r\n"), &ini);
    Str8 text = ini_write(a, &ini);

    // Fichier : écriture, relecture, BOM toléré.
    {
        u16 wtmp[MAX_PATH];
        DWORD n = GetTempPathW(MAX_PATH, (LPWSTR)wtmp);
        CHECK(n > 0, "dossier temporaire");
        Str8 dir = utf16_to_utf8(a, wtmp);
        Str8 path = str8_cat(a, dir, S("vibesync-test.ini"));
        CHECK(ini_save_file(a, path, text), "écriture du fichier");
        Ini loaded;
        CHECK(ini_load_file(a, path, &loaded), "lecture du fichier");
        CHECK(str8_eq(ini_get(&loaded, "pseudo", S("")), S("Thibault Éloïse")), "accents sur disque");
        Str8 bom = str8_cat(a, S("\xef\xbb\xbf"), text);
        CHECK(ini_save_file(a, path, bom), "écriture avec BOM");
        CHECK(ini_load_file(a, path, &loaded), "lecture avec BOM");
        CHECK(str8_eq(ini_get(&loaded, "serveur", S("")), S("wss://vibesync.exemple.fr/ws")), "BOM ignoré");
        u16 *wpath = utf8_to_utf16(a, path, NULL);
        DeleteFileW((LPCWSTR)wpath);
        Ini none;
        CHECK(!ini_load_file(a, path, &none) && none.count == 0, "fichier absent");
    }

    temp_end(top);
}

// ------------------------------------------------------------ ui ---

static void test_ui(void) {
    section("ui");
    struct {
        f64 sec;
        const char *want;
    } times[] = {
        {0, "0:00"},        {5, "0:05"},       {59.9, "0:59"},     {60, "1:00"},
        {83, "1:23"},       {599, "9:59"},     {3600, "1:00:00"},  {5025, "1:23:45"},
        {-5, "0:00"},       {36000, "10:00:00"},
    };
    for (isize i = 0; i < VS_ARRAY_COUNT(times); i++) {
        char buf[32];
        ui_format_time(times[i].sec, buf, sizeof(buf));
        CHECK(strcmp(buf, times[i].want) == 0, "temps %.1f = %s, attendu %s", times[i].sec, buf,
              times[i].want);
    }
    {
        char buf[32];
        // Valeur non finie : repli sur 0:00 plutôt qu'un affichage absurde.
        ui_format_time(1e308 * 10, buf, sizeof(buf));
        CHECK(strcmp(buf, "0:00") == 0, "durée infinie : %s", buf);
        // Valeur finie démesurée : bornée à 99:59:59.
        ui_format_time(9999999, buf, sizeof(buf));
        CHECK(strcmp(buf, "99:59:59") == 0, "durée démesurée bornée : %s", buf);
    }
    // Champ de saisie : troncature sur frontière UTF-8, jamais au milieu.
    {
        UiText t;
        memset(&t, 0, sizeof(t));
        u8 big_text[UI_TEXT_CAP * 2];
        for (isize i = 0; i < (isize)sizeof(big_text); i += 2) {
            big_text[i] = 0xc3;  // « é » en UTF-8 : deux octets
            big_text[i + 1] = 0xa9;
        }
        ui_text_set(&t, str8(big_text, (isize)sizeof(big_text)));
        CHECK(t.len < UI_TEXT_CAP, "champ tronqué (%lld)", (long long)t.len);
        CHECK(utf8_validate(ui_text_str(&t)), "troncature au milieu d'un caractère UTF-8");
        ui_text_set(&t, S("bonjour"));
        CHECK(str8_eq(ui_text_str(&t), S("bonjour")) && t.caret == 7, "contenu et caret");
    }
}

// ------------------------------------------- édition de texte (VS-018) ---
//
// Le modèle caret/sélection est découplé du rendu : ces tests l'exercent sans
// fenêtre ni GDI, en branchant une police fictive sur UiTextMetrics.

// fake_width : 10 px par caractère, 4 px pour les caractères fins. Largeurs
// inégales exprès, pour que le hit-test ne puisse pas tricher avec une division.
static i32 fake_width(void *ctx, const UiText *t, isize byte_off) {
    VS_UNUSED(ctx);
    i32 w = 0;
    for (isize i = 0; i < byte_off && i < t->len; i++) {
        u8 c = t->data[i];
        if ((c & 0xc0) == 0x80) continue;  // octet de continuation : même glyphe
        w += (c == 'i' || c == 'l') ? 4 : 10;
    }
    return w;
}

static void test_text_edit(void) {
    section("édition de texte");
    UiTextMetrics mx = {fake_width, NULL};
    UiText t;
    memset(&t, 0, sizeof(t));

    // --- hit-test : « salon », frontières à 0, 10, 20, 24, 34, 44 px ---
    ui_text_set(&t, S("salon"));
    struct {
        i32 x;
        isize want;
    } hits[] = {
        {-20, 0}, {0, 0}, {4, 0},  {6, 1},  {15, 1},
        {16, 2},  {23, 3}, {40, 5}, {1000, 5},
    };
    for (isize i = 0; i < VS_ARRAY_COUNT(hits); i++) {
        isize got = ui_text_hit(&t, &mx, hits[i].x);
        CHECK(got == hits[i].want, "hit(%d) = %lld, attendu %lld", (int)hits[i].x, (long long)got,
              (long long)hits[i].want);
    }
    {
        UiText empty;
        memset(&empty, 0, sizeof(empty));
        CHECK(ui_text_hit(&empty, &mx, 50) == 0, "hit sur champ vide");
    }

    // --- hit-test : jamais au milieu d'un caractère multi-octets ---
    ui_text_set(&t, S("café"));  // « é » = 2 octets, len 5
    CHECK(t.len == 5, "longueur de « café » = %lld", (long long)t.len);
    CHECK(ui_text_hit(&t, &mx, 33) == 3, "hit avant « é »");
    CHECK(ui_text_hit(&t, &mx, 36) == 5, "hit après « é » (pas 4)");
    ui_text_move(&t, 4, 0);  // offset interdit : ramené sur la frontière
    CHECK(t.caret == 3, "caret recalé sur frontière UTF-8 (%lld)", (long long)t.caret);

    // --- clic puis glissé : sélection continue ---
    ui_text_set(&t, S("salon"));
    ui_text_move(&t, ui_text_hit(&t, &mx, 16), 0);  // appui à 16 px → offset 2
    CHECK(t.caret == 2 && !ui_text_has_sel(&t), "appui : caret posé, pas de sélection");
    ui_text_move(&t, ui_text_hit(&t, &mx, 30), 1);  // glissé
    CHECK(ui_text_sel_lo(&t) == 2 && ui_text_sel_hi(&t) == 4, "glissé : sélection [2,4)");
    ui_text_move(&t, ui_text_hit(&t, &mx, 1000), 1);
    CHECK(str8_eq(ui_text_selection(&t), S("lon")), "glissé jusqu'au bout");
    // Retour en arrière : l'ancre ne bouge pas, la sélection s'inverse.
    ui_text_move(&t, ui_text_hit(&t, &mx, 0), 1);
    CHECK(ui_text_sel_lo(&t) == 0 && ui_text_sel_hi(&t) == 2 && str8_eq(ui_text_selection(&t), S("sa")),
          "sélection inversée");

    // --- double-clic : mot sous le curseur ---
    struct {
        const char *text;
        isize pos;
        const char *want;
    } words[] = {
        {"salut le monde", 6, "le"},
        {"salut le monde", 0, "salut"},
        {"salut le monde", 14, "monde"},   // clic après le dernier caractère
        {"salut le monde", 5, " "},        // sur un espace : la suite d'espaces
        {"ws://127.0.0.1:8080/ws", 5, "127"},
        {"ws://127.0.0.1:8080/ws", 2, "://"},  // ponctuation : le groupe entier
        {"Thibault Éloïse", 9, "Éloïse"},      // accents = lettres
    };
    for (isize i = 0; i < VS_ARRAY_COUNT(words); i++) {
        ui_text_set(&t, S(words[i].text));
        isize lo, hi;
        ui_text_word_bounds(&t, words[i].pos, &lo, &hi);
        ui_text_select_range(&t, lo, hi);
        CHECK(str8_eq(ui_text_selection(&t), S(words[i].want)), "mot en %lld de « %s » = « %.*s »",
              (long long)words[i].pos, words[i].text, (int)ui_text_selection(&t).len,
              ui_text_selection(&t).data);
    }
    {
        UiText empty;
        memset(&empty, 0, sizeof(empty));
        isize lo = 9, hi = 9;
        ui_text_word_bounds(&empty, 0, &lo, &hi);
        CHECK(lo == 0 && hi == 0, "mot dans un champ vide");
    }

    // --- sauts de mot (Ctrl+flèches) ---
    ui_text_set(&t, S("salut le monde"));
    CHECK(ui_text_word_right(&t, 0) == 6, "mot à droite depuis 0");
    CHECK(ui_text_word_right(&t, 6) == 9, "mot à droite depuis 6");
    CHECK(ui_text_word_right(&t, 14) == 14, "mot à droite en fin de champ");
    CHECK(ui_text_word_left(&t, 14) == 9, "mot à gauche depuis la fin");
    CHECK(ui_text_word_left(&t, 9) == 6, "mot à gauche depuis 9");
    CHECK(ui_text_word_left(&t, 0) == 0, "mot à gauche en début de champ");

    // --- Ctrl+A puis frappe : la sélection est remplacée ---
    ui_text_set(&t, S("ancien"));
    ui_text_select_all(&t);
    CHECK(ui_text_has_sel(&t) && ui_text_sel_lo(&t) == 0 && ui_text_sel_hi(&t) == 6, "Ctrl+A");
    ui_text_insert_cp(&t, 'X');
    CHECK(str8_eq(ui_text_str(&t), S("X")) && t.caret == 1 && !ui_text_has_sel(&t),
          "frappe sur sélection : remplacement");

    // --- suppressions ---
    ui_text_set(&t, S("salon"));
    ui_text_select_range(&t, 1, 3);
    ui_text_backspace(&t, 0);
    CHECK(str8_eq(ui_text_str(&t), S("son")) && t.caret == 1, "Retour arrière sur sélection");
    ui_text_set(&t, S("salon"));
    ui_text_select_range(&t, 1, 3);
    ui_text_delete_fwd(&t, 0);
    CHECK(str8_eq(ui_text_str(&t), S("son")), "Suppr sur sélection");
    ui_text_set(&t, S("café"));
    ui_text_backspace(&t, 0);
    CHECK(str8_eq(ui_text_str(&t), S("caf")) && utf8_validate(ui_text_str(&t)),
          "Retour arrière supprime le caractère entier");
    ui_text_set(&t, S("salut le monde"));
    ui_text_backspace(&t, 1);
    CHECK(str8_eq(ui_text_str(&t), S("salut le ")), "Ctrl+Retour arrière : un mot");
    ui_text_set(&t, S("salut le monde"));
    ui_text_move(&t, 0, 0);
    ui_text_delete_fwd(&t, 1);
    CHECK(str8_eq(ui_text_str(&t), S("le monde")), "Ctrl+Suppr : un mot");

    // --- collage : mono-ligne, sélection remplacée ---
    ui_text_set(&t, S("abc"));
    ui_text_select_all(&t);
    ui_text_insert_str(&t, S("un\r\ndeux\ttrois"));
    CHECK(str8_eq(ui_text_str(&t), S("undeuxtrois")), "collage filtré : « %.*s »", (int)t.len, t.data);
    CHECK(t.caret == t.len && !ui_text_has_sel(&t), "caret après collage");

    // --- Maj+Origine / Maj+Fin ---
    ui_text_set(&t, S("salon"));
    ui_text_move(&t, 2, 0);
    ui_text_move(&t, t.len, 1);
    CHECK(str8_eq(ui_text_selection(&t), S("lon")), "Maj+Fin");
    ui_text_move(&t, 0, 1);
    CHECK(str8_eq(ui_text_selection(&t), S("sa")), "Maj+Origine depuis l'ancre");

    // --- limites : capacité et frontières UTF-8 ---
    {
        u8 big[UI_TEXT_CAP * 2];
        for (isize i = 0; i < (isize)sizeof(big); i += 2) {
            big[i] = 0xc3;  // « é »
            big[i + 1] = 0xa9;
        }
        ui_text_set(&t, str8(big, (isize)sizeof(big)));
        CHECK(t.len == UI_TEXT_CAP - 2 && utf8_validate(ui_text_str(&t)), "troncature (%lld octets)",
              (long long)t.len);
        isize before = t.len;
        ui_text_insert_cp(&t, 0xe9);  // « é » : 2 octets, il n'en reste qu'un
        CHECK(t.len == before, "insertion refusée quand le champ est plein");
        ui_text_insert_cp(&t, 'x');  // 1 octet : passe tout juste
        CHECK(t.len == before + 1 && t.data[t.len] == 0, "dernier octet utilisable");
        CHECK(utf8_validate(ui_text_str(&t)), "champ plein toujours valide en UTF-8");
        ui_text_select_all(&t);
        ui_text_insert_str(&t, S("court"));
        CHECK(str8_eq(ui_text_str(&t), S("court")), "remplacement total d'un champ plein");
    }
}

// ------------------------------------- dossiers médias, recherche (VS-026) ---

// mk_tree fabrique une arborescence temporaire et renvoie sa racine.
static Str8 mk_tree(Arena *a, const char *leaf) {
    u16 wtmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, (LPWSTR)wtmp);
    if (n == 0) return str8_lit("");
    Str8 root = str8_cat(a, utf16_to_utf8(a, wtmp), str8_from_cstr(leaf));
    u16 *w = utf8_to_utf16(a, root, NULL);
    CreateDirectoryW((LPCWSTR)w, NULL);
    return root;
}

static void mk_dir(Arena *a, Str8 parent, const char *name) {
    Str8 p = str8_cat(a, str8_cat(a, parent, S("\\")), S(name));
    u16 *w = utf8_to_utf16(a, p, NULL);
    CreateDirectoryW((LPCWSTR)w, NULL);
}

// mk_file crée un fichier de `size` octets (le contenu n'a aucune importance,
// seule la taille sert à départager les homonymes).
static void mk_file(Arena *a, Str8 dir, const char *name, isize size) {
    Str8 p = str8_cat(a, str8_cat(a, dir, S("\\")), S(name));
    u16 *w = utf8_to_utf16(a, p, NULL);
    HANDLE h = CreateFileW((LPCWSTR)w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    if (size > 0) {
        u8 *buf = arena_push_array(a, u8, size);
        DWORD written = 0;
        WriteFile(h, buf, (DWORD)size, &written, NULL);
    }
    CloseHandle(h);
}

// rm_tree efface récursivement (nettoyage de fin de test).
static void rm_tree(Arena *a, Str8 dir) {
    TempArena t = temp_begin(a);
    Str8 pattern = str8_cat(a, dir, S("\\*"));
    u16 *wp = utf8_to_utf16(a, pattern, NULL);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((LPCWSTR)wp, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            Str8 name = utf16_to_utf8(a, (const u16 *)fd.cFileName);
            if (str8_eq(name, S(".")) || str8_eq(name, S(".."))) continue;
            Str8 child = str8_cat(a, str8_cat(a, dir, S("\\")), name);
            u16 *wc = utf8_to_utf16(a, child, NULL);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rm_tree(a, child);
            else DeleteFileW((LPCWSTR)wc);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    u16 *wd = utf8_to_utf16(a, dir, NULL);
    RemoveDirectoryW((LPCWSTR)wd);
    temp_end(t);
}

static void test_media(Arena *a) {
    section("dossiers médias");
    TempArena top = temp_begin(a);

    // --- recherche sur une arborescence réelle ---
    Str8 root = mk_tree(a, "vibesync-media-test");
    CHECK(root.len > 0, "arborescence temporaire créée");
    if (root.len == 0) {
        temp_end(top);
        return;
    }
    mk_dir(a, root, "films");
    Str8 films = str8_cat(a, root, S("\\films"));
    mk_dir(a, films, "vo");
    Str8 vo = str8_cat(a, films, S("\\vo"));
    mk_file(a, root, "autre.mkv", 10);
    mk_file(a, films, "EP1-VOSTFR.mkv", 100);   // casse différente
    mk_file(a, vo, "ep1-vostfr.mkv", 5000);     // homonyme, plus gros
    mk_file(a, vo, "bande-annonce.mp4", 20);

    StrBuf dirs[MEDIA_MAX_DIRS];
    memset(dirs, 0, sizeof(dirs));
    strbuf_set(&dirs[0], root);
    MediaFind r;

    // Trouvé malgré la casse, et le plus gros homonyme l'emporte.
    CHECK(media_find(a, dirs, 1, S("ep1-vostfr.mkv"), &r), "fichier trouvé");
    CHECK(r.matches == 2, "deux homonymes vus (%lld)", (long long)r.matches);
    CHECK(r.size_bytes == 5000, "le plus gros gagne (%lld octets)", (long long)r.size_bytes);
    CHECK(r.found && strbuf_str(&r.path).len > 0, "chemin complet rendu");
    {
        Str8 p = strbuf_str(&r.path);
        b32 in_vo = 0;
        for (isize i = 0; i + 3 <= p.len; i++) {
            if (memcmp(p.data + i, "\\vo\\", 4) == 0) in_vo = 1;
        }
        CHECK(in_vo, "chemin du gros fichier : %.*s", (int)p.len, p.data);
    }
    // Recherche insensible à la casse dans l'autre sens.
    CHECK(media_find(a, dirs, 1, S("EP1-VOSTFR.MKV"), &r) && r.matches == 2, "recherche insensible à la casse");
    // Nom exact : pas de correspondance partielle.
    CHECK(!media_find(a, dirs, 1, S("ep1"), &r), "pas de correspondance partielle");
    CHECK(!media_find(a, dirs, 1, S("absent.mkv"), &r), "fichier absent");
    CHECK(r.visited > 0, "entrées parcourues comptées (%lld)", (long long)r.visited);
    // Cas dégénérés.
    CHECK(!media_find(a, dirs, 1, S(""), &r), "nom vide refusé");
    CHECK(!media_find(a, dirs, 0, S("ep1-vostfr.mkv"), &r), "aucun dossier : rien à chercher");
    {
        StrBuf missing[1];
        memset(missing, 0, sizeof(missing));
        strbuf_set(&missing[0], str8_cat(a, root, S("\\nexiste-pas")));
        CHECK(!media_find(a, missing, 1, S("ep1-vostfr.mkv"), &r), "dossier inexistant : échec propre");
    }
    // Une barre finale ne casse pas la construction du chemin.
    {
        StrBuf slash[1];
        memset(slash, 0, sizeof(slash));
        strbuf_set(&slash[0], str8_cat(a, root, S("\\")));
        CHECK(media_find(a, slash, 1, S("autre.mkv"), &r), "dossier avec barre finale");
    }

    // --- borne de profondeur : au-delà de 6 niveaux, on ne descend plus ---
    {
        Str8 deep = root;
        for (int i = 0; i < 8; i++) {
            char name[8];
            snprintf(name, sizeof(name), "n%d", i);
            mk_dir(a, deep, name);
            deep = str8_cat(a, str8_cat(a, deep, S("\\")), S(name));
        }
        mk_file(a, deep, "trop-loin.mkv", 10);
        CHECK(!media_find(a, dirs, 1, S("trop-loin.mkv"), &r),
              "profondeur > %d : non atteint (%lld entrées)", MEDIA_MAX_DEPTH, (long long)r.visited);
        // À la profondeur maximale, en revanche, on trouve.
        Str8 shallow = str8_cat(a, root, S("\\n0\\n1\\n2\\n3\\n4"));
        mk_file(a, shallow, "juste-assez.mkv", 10);
        CHECK(media_find(a, dirs, 1, S("juste-assez.mkv"), &r), "profondeur atteignable : trouvé");
    }

    // --- borne d'entrées : beaucoup de fichiers, la recherche s'écourte ---
    {
        mk_dir(a, root, "beaucoup");
        Str8 many = str8_cat(a, root, S("\\beaucoup"));
        for (int i = 0; i < 300; i++) {
            char name[32];
            snprintf(name, sizeof(name), "f%04d.bin", i);
            mk_file(a, many, name, 0);
        }
        CHECK(media_find(a, dirs, 1, S("f0299.bin"), &r), "trouvé parmi 300 fichiers");
        CHECK(r.visited <= MEDIA_MAX_ENTRIES, "borne d'entrées respectée (%lld)", (long long)r.visited);
        CHECK(!r.truncated, "300 fichiers : pas de troncature");
    }

    rm_tree(a, root);
    temp_end(top);
}

// --------------------------------- mot de passe mémorisé (VS-025, DPAPI) ---

static void test_secret(Arena *a) {
    section("secret (DPAPI)");
    TempArena top = temp_begin(a);

    // Aller-retour : accents et octets non ASCII compris.
    const char *plains[] = {"s3cret", "mot de passe très long avec des accents éàü et des espaces", "x",
                            "!@#$%^&*()_+-={}[]|\\:;\"'<>,.?/"};
    for (isize i = 0; i < VS_ARRAY_COUNT(plains); i++) {
        Str8 hex, back;
        if (!secret_protect(a, S(plains[i]), &hex)) {
            failf("chiffrement refusé pour « %s »", plains[i]);
            g_checks++;
            continue;
        }
        // Le blob ne doit jamais contenir le clair en toutes lettres.
        CHECK(hex.len > 0 && (hex.len & 1) == 0, "blob hexadécimal de longueur paire (%lld)",
              (long long)hex.len);
        b32 hexish = 1;
        for (isize k = 0; k < hex.len; k++) {
            u8 c = hex.data[k];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) hexish = 0;
        }
        CHECK(hexish, "blob strictement hexadécimal minuscule");
        u8 *raw = NULL;
        isize raw_len = 0;
        CHECK(secret_hex_decode(a, hex, &raw, &raw_len), "blob redécodable");
        // Le clair ne doit pas se retrouver tel quel dans le blob. Sous 4
        // octets, la recherche n'a aucune valeur : une aiguille si courte se
        // retrouve par hasard dans quelques centaines d'octets chiffrés.
        Str8 needle = S(plains[i]);
        if (needle.len >= 4) {
            b32 found = 0;
            for (isize k = 0; raw && k + needle.len <= raw_len; k++) {
                if (memcmp(raw + k, needle.data, (size_t)needle.len) == 0) found = 1;
            }
            CHECK(!found, "le clair « %s » n'apparaît pas dans le blob chiffré", plains[i]);
        }

        CHECK(secret_unprotect(a, hex, &back), "déchiffrement de « %s »", plains[i]);
        CHECK(str8_eq(back, S(plains[i])), "aller-retour exact : « %.*s »", (int)back.len, back.data);
    }

    // Deux chiffrements du même clair diffèrent (DPAPI sale son entrée).
    {
        Str8 h1, h2;
        if (secret_protect(a, S("pareil"), &h1) && secret_protect(a, S("pareil"), &h2)) {
            CHECK(!str8_eq(h1, h2), "deux blobs du même clair ne sont pas identiques");
        }
    }

    // Clair vide : refusé (rien à mémoriser).
    {
        Str8 hex;
        CHECK(!secret_protect(a, S(""), &hex), "clair vide refusé");
    }

    // Blobs invalides : échec propre, jamais de plantage ni de sortie remplie.
    {
        Str8 ref;
        CHECK(secret_protect(a, S("s3cret"), &ref), "blob de référence");
        struct {
            const char *hex;
            const char *why;
        } bad[] = {
            {"", "blob vide"},
            {"abc", "longueur impaire"},
            {"zz", "caractère non hexadécimal"},
            {"00112233445566778899aabbccddeeff", "octets aléatoires"},
            {"deadbeef", "blob trop court"},
        };
        for (isize i = 0; i < VS_ARRAY_COUNT(bad); i++) {
            Str8 out = S("sentinelle");
            CHECK(!secret_unprotect(a, S(bad[i].hex), &out), "%s rejeté", bad[i].why);
            CHECK(str8_eq(out, S("sentinelle")), "%s : la sortie n'est pas touchée", bad[i].why);
        }
        // Blob authentique mais corrompu au milieu : DPAPI doit le refuser.
        u8 *copy = arena_push_array(a, u8, ref.len);
        memcpy(copy, ref.data, (size_t)ref.len);
        copy[ref.len / 2] ^= 0x0f;  // reste hexadécimal, mais le contenu change
        if (copy[ref.len / 2] > 'f') copy[ref.len / 2] = '0';
        Str8 out = S("sentinelle");
        CHECK(!secret_unprotect(a, str8(copy, ref.len), &out), "blob corrompu rejeté");
        // Mauvaise entropie applicative : un blob DPAPI d'une autre appli ne
        // doit pas être lisible par nous. On le simule en tronquant.
        CHECK(!secret_unprotect(a, str8_sub(ref, 0, ref.len - 2), &out), "blob tronqué rejeté");
    }

    // secret_wipe efface réellement.
    {
        u8 buf[32];
        memset(buf, 0xab, sizeof(buf));
        secret_wipe(buf, (isize)sizeof(buf));
        b32 clean = 1;
        for (isize i = 0; i < (isize)sizeof(buf); i++) {
            if (buf[i] != 0) clean = 0;
        }
        CHECK(clean, "secret_wipe met le tampon à zéro");
        secret_wipe(NULL, 16);  // ne doit pas planter
        secret_wipe(buf, 0);
    }

    // --- règles du fichier ini ---
    section("ini : secret");
    {
        Ini ini;
        ini_clear(&ini);
        ini_set(a, &ini, "serveur", S("wss://x/ws"));
        ini_set(a, &ini, "password_enc", S("00ff"));
        ini_set(a, &ini, "salle", S("salon"));
        CHECK(ini.count == 3, "trois entrées");
        CHECK(ini_remove(&ini, "password_enc"), "suppression de password_enc");
        CHECK(ini.count == 2, "l'entrée a disparu (%lld)", (long long)ini.count);
        CHECK(ini_get(&ini, "password_enc", S("absent")).len == 6, "clé introuvable après suppression");
        // Les autres clés gardent leur valeur et leur ordre.
        CHECK(str8_eq(ini_get(&ini, "serveur", S("")), S("wss://x/ws")) &&
                  str8_eq(ini_get(&ini, "salle", S("")), S("salon")),
              "les autres réglages sont intacts");
        CHECK(!ini_remove(&ini, "password_enc"), "seconde suppression sans effet");
        CHECK(!ini_remove(&ini, "jamais_vu"), "clé inconnue : rien à supprimer");
        // Le fichier écrit ne doit plus mentionner la clé.
        Str8 text = ini_write(a, &ini);
        b32 mentions = 0;
        for (isize i = 0; i + 12 <= text.len; i++) {
            if (memcmp(text.data + i, "password_enc", 12) == 0) mentions = 1;
        }
        CHECK(!mentions, "vibesync.ini n'écrit plus password_enc");
    }
    // Un ini existant sans entrée de mot de passe reste lisible tel quel.
    {
        Ini ini;
        CHECK(ini_parse(a, S("serveur=wss://x/ws\npseudo=thibault\n"), &ini), "ini d'une version antérieure");
        CHECK(ini_get(&ini, "password_enc", S("")).len == 0, "aucun secret mémorisé");
        CHECK(ini_get(&ini, "retenir_mdp", S("1")).len == 1, "case cochée par défaut");
    }

    temp_end(top);
}

// --------------------------------------------- faux VLC HTTP (sur socket) ---
//
// Sert /requests/status.json comme le vrai VLC, pour exercer le chemin réseau
// de vlc.c (Basic auth, commandes, réponses chunked) et la préparation
// pause+seek 0 exigée par docs/protocol.md §Chargement de fichier.

typedef struct {
    SOCKET listener;
    int port;
    HANDLE thread;
    volatile long stop;
    volatile long requests;
    volatile long chunked;  // répondre en Transfer-Encoding: chunked
    char password[64];
    // état simulé, protégé par le verrou
    SRWLOCK lock;
    const char *state;
    f64 pos;
    f64 length;
    f64 rate;
} FakeVlcHttp;

static void fake_http_body(FakeVlcHttp *srv, Arena *a, Str8 *out) {
    JsonWriter w;
    jw_init(&w, a);
    jw_obj_begin(&w);
    jw_key(&w, "state");
    jw_cstr(&w, srv->state);
    jw_kv_num(&w, "length", srv->length);
    jw_kv_num(&w, "time", (f64)(i64)srv->pos);
    jw_kv_num(&w, "rate", srv->rate);
    jw_kv_num(&w, "position", srv->length > 0 ? srv->pos / srv->length : 0);
    jw_key(&w, "information");
    jw_obj_begin(&w);
    jw_key(&w, "category");
    jw_obj_begin(&w);
    jw_key(&w, "meta");
    jw_obj_begin(&w);
    jw_kv_str(&w, "filename", str8_lit("ep1.mkv"));
    jw_obj_end(&w);
    jw_obj_end(&w);
    jw_obj_end(&w);
    jw_obj_end(&w);
    *out = jw_result(&w);
}

// fake_http_apply exécute la commande portée par la query.
static void fake_http_apply(FakeVlcHttp *srv, Str8 query) {
    Str8 cmd = str8_lit("");
    Str8 val = str8_lit("");
    isize i = 0;
    while (i < query.len) {
        isize amp = str8_find_char(query, '&', i);
        Str8 pair = str8_sub(query, i, (amp < 0 ? query.len : amp) - i);
        isize eq = str8_find_char(pair, '=', 0);
        if (eq > 0) {
            Str8 k = str8_sub(pair, 0, eq);
            Str8 v = str8_sub(pair, eq + 1, -1);
            if (str8_eq_cstr(k, "command")) cmd = v;
            else if (str8_eq_cstr(k, "val")) val = v;
        }
        if (amp < 0) break;
        i = amp + 1;
    }
    if (cmd.len == 0) return;
    if (str8_eq_cstr(cmd, "pl_forcepause")) {
        if (strcmp(srv->state, "playing") == 0) srv->state = "paused";
    } else if (str8_eq_cstr(cmd, "pl_forceresume")) {
        if (strcmp(srv->state, "paused") == 0) srv->state = "playing";
    } else if (str8_eq_cstr(cmd, "seek")) {
        f64 v = 0;
        if (str_to_f64(val, &v) && v >= 0) srv->pos = v;
    } else if (str8_eq_cstr(cmd, "rate")) {
        f64 v = 0;
        if (str_to_f64(val, &v) && v > 0) srv->rate = v;
    }
}

static DWORD WINAPI fake_vlc_thread(LPVOID param) {
    FakeVlcHttp *srv = (FakeVlcHttp *)param;
    Arena *a = arena_create(VS_MB(1));
    if (!a) return 1;
    char expected[128];
    {
        u8 raw[80];
        isize m = 0;
        raw[m++] = ':';
        for (isize i = 0; srv->password[i] && m < (isize)sizeof(raw); i++) raw[m++] = (u8)srv->password[i];
        base64_encode(raw, m, expected, (isize)sizeof(expected));
    }
    while (!InterlockedCompareExchange(&srv->stop, 0, 0)) {
        SOCKET c = accept(srv->listener, NULL, NULL);
        if (c == INVALID_SOCKET) break;
        isize mark = arena_pos(a);
        u8 req[8192];
        isize len = 0;
        b32 complete = 0;
        while (len < (isize)sizeof(req)) {
            int n = recv(c, (char *)req + len, (int)(sizeof(req) - (size_t)len), 0);
            if (n <= 0) break;
            len += n;
            if (len >= 4 && memcmp(req + len - 4, "\r\n\r\n", 4) == 0) {
                complete = 1;
                break;
            }
        }
        if (!complete) {
            closesocket(c);
            arena_pop_to(a, mark);
            continue;
        }
        InterlockedIncrement(&srv->requests);
        Str8 head = str8(req, len);
        // Authentification : « Authorization: Basic <b64> ».
        b32 authorized = 0;
        Str8 tag = str8_lit("Authorization: Basic ");
        for (isize i = 0; i + tag.len <= head.len; i++) {
            if (memcmp(head.data + i, tag.data, (size_t)tag.len) != 0) continue;
            isize s = i + tag.len, e = s;
            while (e < head.len && head.data[e] != '\r' && head.data[e] != '\n') e++;
            authorized = str8_eq(str8_trim(str8_sub(head, s, e - s)), str8_from_cstr(expected));
            break;
        }
        Builder resp;
        builder_init(&resp, a, VS_KB(8));
        if (!authorized) {
            builder_cstr(&resp, "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"VLC\"\r\n"
                                "Content-Length: 0\r\nConnection: close\r\n\r\n");
        } else {
            isize qs = str8_find_char(head, '?', 0);
            isize sp = str8_find_char(head, ' ', 4);
            Str8 query = str8_lit("");
            if (qs > 0 && sp > qs) query = str8_sub(head, qs + 1, sp - qs - 1);
            Str8 body;
            AcquireSRWLockExclusive(&srv->lock);
            fake_http_apply(srv, query);
            fake_http_body(srv, a, &body);
            ReleaseSRWLockExclusive(&srv->lock);
            if (InterlockedCompareExchange(&srv->chunked, 0, 0)) {
                builder_cstr(&resp, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                    "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
                // Deux blocs pour exercer réellement le dé-chunking.
                isize half = body.len / 2;
                char hex[24];
                snprintf(hex, sizeof(hex), "%llx\r\n", (unsigned long long)half);
                builder_cstr(&resp, hex);
                builder_bytes(&resp, body.data, half);
                builder_cstr(&resp, "\r\n");
                snprintf(hex, sizeof(hex), "%llx\r\n", (unsigned long long)(body.len - half));
                builder_cstr(&resp, hex);
                builder_bytes(&resp, body.data + half, body.len - half);
                builder_cstr(&resp, "\r\n0\r\n\r\n");
            } else {
                char hdr[160];
                snprintf(hdr, sizeof(hdr),
                         "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %llu\r\n"
                         "Connection: close\r\n\r\n",
                         (unsigned long long)body.len);
                builder_cstr(&resp, hdr);
                builder_bytes(&resp, body.data, body.len);
            }
        }
        Str8 out = builder_result(&resp);
        sock_send_all(c, out.data, out.len);
        shutdown(c, SD_SEND);
        closesocket(c);
        arena_pop_to(a, mark);
    }
    arena_destroy(a);
    return 0;
}

static b32 fake_vlc_start(FakeVlcHttp *srv, const char *password) {
    memset(srv, 0, sizeof(*srv));
    InitializeSRWLock(&srv->lock);
    srv->state = "playing";  // VLC démarre la lecture tout seul à l'ouverture
    srv->pos = 5.0;
    srv->length = 1200;
    srv->rate = 1;
    isize n = (isize)strlen(password);
    if (n >= (isize)sizeof(srv->password)) return 0;
    memcpy(srv->password, password, (size_t)n + 1);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    srv->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv->listener == INVALID_SOCKET) return 0;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(srv->listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) return 0;
    int alen = (int)sizeof(addr);
    if (getsockname(srv->listener, (struct sockaddr *)&addr, &alen) != 0) return 0;
    srv->port = ntohs(addr.sin_port);
    if (listen(srv->listener, 8) != 0) return 0;
    srv->thread = CreateThread(NULL, 0, fake_vlc_thread, srv, 0, NULL);
    return srv->thread != NULL;
}

static void fake_vlc_stop(FakeVlcHttp *srv) {
    InterlockedExchange(&srv->stop, 1);
    if (srv->listener != INVALID_SOCKET) {
        closesocket(srv->listener);
        srv->listener = INVALID_SOCKET;
    }
    if (srv->thread) {
        WaitForSingleObject(srv->thread, 5000);
        CloseHandle(srv->thread);
        srv->thread = NULL;
    }
}

static void test_vlc_live(Arena *a) {
    section("vlc (HTTP réel)");
    TempArena top = temp_begin(a);
    FakeVlcHttp srv;
    if (!fake_vlc_start(&srv, "mdp-test")) {
        failf("faux VLC HTTP indisponible");
        temp_end(top);
        return;
    }
    VlcClient c;
    vlc_client_init(&c, srv.port, S("mdp-test"));

    VsStatus st;
    CHECK(vlc_status(&c, a, &st) == VLC_OK, "status sur socket");
    CHECK(st.state == VS_PLAY_PLAYING, "VLC démarre en lecture");
    CHECK(approx(st.position_sec, 5, 1e-6), "position initiale = %f", st.position_sec);
    CHECK(strbuf_eq(&st.file_name, S("ep1.mkv")), "nom de fichier");

    // Réponses chunked : même résultat.
    InterlockedExchange(&srv.chunked, 1);
    CHECK(vlc_status(&c, a, &st) == VLC_OK, "status en Transfer-Encoding: chunked");
    CHECK(st.length_sec == 1200, "durée en chunked");
    InterlockedExchange(&srv.chunked, 0);

    // Préparation : pause + position 0 constatés (§Chargement de fichier).
    CHECK(vlc_prepare_paused(&c, a, 5000) == VLC_OK, "préparation pause+0");
    CHECK(vlc_status(&c, a, &st) == VLC_OK, "status après préparation");
    CHECK(st.state == VS_PLAY_PAUSED, "média non mis en pause");
    CHECK(st.position_sec < VLC_START_TOLERANCE, "média non ramené au début (%f)", st.position_sec);
    // Idempotence : un second appel ne fait rien de plus.
    long before = srv.requests;
    CHECK(vlc_prepare_paused(&c, a, 5000) == VLC_OK, "préparation idempotente");
    CHECK(srv.requests - before <= 2, "préparation non idempotente (%ld requêtes)", srv.requests - before);

    // Commandes.
    CHECK(vlc_seek(&c, a, 42.4) == VLC_OK, "seek");
    CHECK(vlc_status(&c, a, &st) == VLC_OK, "status après seek");
    CHECK(approx(st.position_sec, 42, 1e-6), "seek arrondi à la seconde : %f", st.position_sec);
    CHECK(vlc_set_rate(&c, a, 1.05) == VLC_OK, "rate");
    CHECK(vlc_resume(&c, a) == VLC_OK, "resume");
    CHECK(vlc_status(&c, a, &st) == VLC_OK, "status final");
    CHECK(st.state == VS_PLAY_PLAYING && approx(st.rate, 1.05, 1e-9), "reprise et rate");
    VsCmd cmd = {VS_CMD_PAUSE, 0};
    CHECK(vlc_apply(&c, a, cmd) == VLC_OK, "vlc_apply pause");

    // Mauvais mot de passe : 401 remonté distinctement.
    VlcClient bad;
    vlc_client_init(&bad, srv.port, S("mauvais"));
    CHECK(vlc_status(&bad, a, &st) == VLC_ERR_AUTH, "mot de passe erroné accepté");

    // Port fermé : erreur de connexion, pas de blocage.
    VlcClient dead;
    vlc_client_init(&dead, 1, S("x"));
    VlcError err = vlc_status(&dead, a, &st);
    CHECK(err == VLC_ERR_CONNECT || err == VLC_ERR_RECV, "port fermé : %s", vlc_error_text(err));

    fake_vlc_stop(&srv);
    temp_end(top);
}

// ------------------------------------------------------------ ordonnancement ---

void test_win32_run(Arena *a) {
    test_base_platform(a);
    test_net_url();
    test_ini_file(a);
    test_ui();
    test_text_edit();
    test_media(a);
    test_secret(a);
    test_vlc_live(a);
    test_net_live(a);
    test_net_queue_saturation(a);
}
