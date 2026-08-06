#include "net.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <string.h>

_Static_assert(sizeof(SRWLOCK) == sizeof(void *), "SRWLOCK tient dans un pointeur");

#define SRW(field) ((PSRWLOCK)(void *)&(field))

struct NetEvent {
    NetEventKind kind;
    u32 code;
    isize len;
    NetEvent *next;
    u8 *data;
};

// ------------------------------------------------------------------- URL ---

b32 net_parse_url(Str8 url, NetUrl *out) {
    memset(out, 0, sizeof(*out));
    Str8 rest;
    if (str8_starts_with(url, str8_lit("wss://"))) {
        out->secure = 1;
        out->port = 443;
        rest = str8_sub(url, 6, -1);
    } else if (str8_starts_with(url, str8_lit("ws://"))) {
        out->secure = 0;
        out->port = 80;
        rest = str8_sub(url, 5, -1);
    } else if (str8_starts_with(url, str8_lit("https://"))) {
        out->secure = 1;
        out->port = 443;
        rest = str8_sub(url, 8, -1);
    } else if (str8_starts_with(url, str8_lit("http://"))) {
        out->secure = 0;
        out->port = 80;
        rest = str8_sub(url, 7, -1);
    } else {
        return 0;
    }
    if (rest.len == 0) return 0;

    isize slash = str8_find_char(rest, '/', 0);
    Str8 authority = slash < 0 ? rest : str8_sub(rest, 0, slash);
    Str8 path = slash < 0 ? str8_lit("/") : str8_sub(rest, slash, -1);
    if (authority.len == 0) return 0;
    // Les identifiants dans l'URL ne sont pas acceptés (surface inutile).
    if (str8_find_char(authority, '@', 0) >= 0) return 0;

    Str8 host = authority;
    if (authority.data[0] == '[') {  // IPv6 littéral
        isize close_br = str8_find_char(authority, ']', 0);
        if (close_br < 0) return 0;
        host = str8_sub(authority, 1, close_br - 1);
        Str8 after = str8_sub(authority, close_br + 1, -1);
        if (after.len > 0) {
            if (after.data[0] != ':') return 0;
            i64 p = 0;
            if (!str_to_i64(str8_sub(after, 1, -1), &p) || p <= 0 || p > 65535) return 0;
            out->port = (int)p;
        }
    } else {
        isize colon = str8_find_char(authority, ':', 0);
        if (colon >= 0) {
            host = str8_sub(authority, 0, colon);
            i64 p = 0;
            if (!str_to_i64(str8_sub(authority, colon + 1, -1), &p) || p <= 0 || p > 65535) return 0;
            out->port = (int)p;
        }
    }
    if (host.len == 0 || host.len >= (isize)sizeof(out->host)) return 0;
    if (path.len >= (isize)sizeof(out->path)) return 0;
    memcpy(out->host, host.data, (size_t)host.len);
    out->host[host.len] = 0;
    memcpy(out->path, path.data, (size_t)path.len);
    out->path[path.len] = 0;
    return 1;
}

// ---------------------------------------------------- file d'événements ---
//
// Nœuds de taille exacte dans une arène dédiée. L'arène n'est remise à zéro
// que lorsque la file est vide (allocation/libération strictement FIFO) : rien
// n'est jamais écrasé sous le consommateur, et la profondeur n'est plus
// plafonnée à un nombre fixe de créneaux.

// queue_push_locked suppose queue_lock tenu. Renvoie 0 si l'arène sature.
static b32 queue_push_locked(Net *n, NetEventKind kind, u32 code, const u8 *data, isize len) {
    isize need = (isize)sizeof(NetEvent) + len + 64;
    if (arena_pos(n->queue_arena) + need > NET_QUEUE_ARENA - VS_KB(64)) return 0;
    NetEvent *ev = arena_push_struct(n->queue_arena, NetEvent);
    ev->kind = kind;
    ev->code = code;
    ev->len = len;
    if (len > 0) {
        ev->data = arena_push_array(n->queue_arena, u8, len);
        if (data) memcpy(ev->data, data, (size_t)len);
    }
    if (n->tail) {
        n->tail->next = ev;
    } else {
        n->head = ev;
    }
    n->tail = ev;
    n->queued++;
    return 1;
}

// queue_push est appelée par le thread réseau. En cas de saturation, elle
// remonte une erreur explicite et demande l'arrêt : jamais de perte muette
// d'un état autoritatif.
static void queue_push(Net *n, NetEventKind kind, u32 code, const u8 *data, isize len) {
    b32 saturated = 0;
    AcquireSRWLockExclusive(SRW(n->queue_lock));
    if (!queue_push_locked(n, kind, code, data, len)) {
        // Dernière tentative sans charge utile : signaler la saturation.
        if (!queue_push_locked(n, NET_EV_ERROR, NET_ERR_QUEUE_FULL, NULL, 0)) {
            n->dropped++;  // file totalement bloquée : le consommateur est mort
        }
        saturated = 1;
    }
    ReleaseSRWLockExclusive(SRW(n->queue_lock));
    if (saturated) InterlockedExchange(&n->stop, 1);
    if (n->wakeup) SetEvent((HANDLE)n->wakeup);
}

b32 net_poll(Net *n, NetSlot *out) {
    b32 got = 0;
    AcquireSRWLockExclusive(SRW(n->queue_lock));
    NetEvent *ev = n->head;
    if (ev) {
        out->kind = ev->kind;
        out->code = ev->code;
        out->len = VS_MIN(ev->len, (isize)NET_MSG_MAX);
        if (out->len > 0) memcpy(out->data, ev->data, (size_t)out->len);
        n->head = ev->next;
        if (!n->head) {
            n->tail = NULL;
            arena_reset(n->queue_arena);  // file vide : mémoire récupérable
        }
        n->queued--;
        got = 1;
    }
    ReleaseSRWLockExclusive(SRW(n->queue_lock));
    return got;
}

void *net_wakeup_handle(Net *n) { return n->wakeup; }

NetState net_state(Net *n) {
    AcquireSRWLockShared(SRW(n->lock));
    NetState s = n->state;
    ReleaseSRWLockShared(SRW(n->lock));
    return s;
}

// --------------------------------------------------- handles sous verrou ---

// close_handles_locked ferme et annule les handles WinHTTP. Fermer un handle
// débloque immédiatement l'appel synchrone en cours du thread réseau. Les
// pointeurs sont annulés dans la foulée : chaque handle est fermé une fois et
// une seule, quel que soit le thread qui arrive le premier.
static void close_handles_locked(Net *n) {
    if (n->websock) {
        WinHttpCloseHandle((HINTERNET)n->websock);
        n->websock = NULL;
    }
    if (n->request) {
        WinHttpCloseHandle((HINTERNET)n->request);
        n->request = NULL;
    }
    if (n->connect) {
        WinHttpCloseHandle((HINTERNET)n->connect);
        n->connect = NULL;
    }
    if (n->session) {
        WinHttpCloseHandle((HINTERNET)n->session);
        n->session = NULL;
    }
}

// publish tente de publier un handle fraîchement créé. Si l'arrêt a été
// demandé entre-temps, le handle est fermé sur place et la fonction renvoie 0 :
// aucun handle n'échappe au cycle de vie.
static b32 publish(Net *n, void **slot, HINTERNET h) {
    b32 ok = 0;
    AcquireSRWLockExclusive(SRW(n->lock));
    if (n->state == NET_STATE_CONNECTING && !InterlockedCompareExchange(&n->stop, 0, 0)) {
        *slot = (void *)h;
        ok = 1;
    }
    ReleaseSRWLockExclusive(SRW(n->lock));
    if (!ok) WinHttpCloseHandle(h);
    return ok;
}

// ---------------------------------------------------------- thread réseau ---

static void thread_finish(Net *n) {
    AcquireSRWLockExclusive(SRW(n->lock));
    close_handles_locked(n);
    n->state = NET_STATE_DEAD;
    ReleaseSRWLockExclusive(SRW(n->lock));
    if (n->wakeup) SetEvent((HANDLE)n->wakeup);
}

static DWORD WINAPI net_thread(LPVOID param) {
    Net *n = (Net *)param;
    Arena *a = arena_create(VS_KB(512));
    if (!a) {
        queue_push(n, NET_EV_ERROR, NET_ERR_NO_MEMORY, NULL, 0);
        thread_finish(n);
        return 1;
    }

    u16 *whost = utf8_to_utf16(a, str8_from_cstr(n->url.host), NULL);
    u16 *wpath = utf8_to_utf16(a, str8_from_cstr(n->url.path), NULL);
    HINTERNET ws_local = NULL;
    u8 *msg = NULL;
    isize msg_len = 0;
    b32 msg_overflow = 0;

    HINTERNET h = WinHttpOpen(L"vibesync/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h) {
        queue_push(n, NET_EV_ERROR, (u32)GetLastError(), NULL, 0);
        goto done;
    }
    // Réception sans limite : c'est la fermeture des handles qui débloque.
    WinHttpSetTimeouts(h, 10000, 10000, 5000, 0);
    if (!publish(n, &n->session, h)) goto done;

    h = WinHttpConnect((HINTERNET)n->session, (LPCWSTR)whost, (INTERNET_PORT)n->url.port, 0);
    if (!h) {
        queue_push(n, NET_EV_ERROR, (u32)GetLastError(), NULL, 0);
        goto done;
    }
    if (!publish(n, &n->connect, h)) goto done;

    h = WinHttpOpenRequest((HINTERNET)n->connect, L"GET", (LPCWSTR)wpath, NULL, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, n->url.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!h) {
        queue_push(n, NET_EV_ERROR, (u32)GetLastError(), NULL, 0);
        goto done;
    }
    if (!publish(n, &n->request, h)) goto done;

    if (!WinHttpSetOption(h, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0) ||
        !WinHttpSendRequest(h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) ||
        !WinHttpReceiveResponse(h, NULL)) {
        queue_push(n, NET_EV_ERROR, (u32)GetLastError(), NULL, 0);
        goto done;
    }
    h = WinHttpWebSocketCompleteUpgrade(h, 0);
    if (!h) {
        queue_push(n, NET_EV_ERROR, (u32)GetLastError(), NULL, 0);
        goto done;
    }
    if (!publish(n, &n->websock, h)) goto done;

    // Copie locale du handle WebSocket : la réception se fait hors verrou.
    // WinHTTP ne libère pas un handle tant qu'une opération est en cours, même
    // si WinHttpCloseHandle a été appelé par le thread principal ; l'appel
    // rend alors la main avec ERROR_WINHTTP_OPERATION_CANCELLED.
    AcquireSRWLockExclusive(SRW(n->lock));
    ws_local = (HINTERNET)n->websock;
    if (ws_local) n->state = NET_STATE_OPEN;
    ReleaseSRWLockExclusive(SRW(n->lock));
    if (!ws_local) goto done;

    queue_push(n, NET_EV_CONNECTED, 0, NULL, 0);

    // Boucle de réception : réassemblage des fragments jusqu'au message complet.
    msg = arena_push_array(a, u8, NET_MSG_MAX);
    for (;;) {
        if (InterlockedCompareExchange(&n->stop, 0, 0)) break;
        DWORD got = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
        u8 chunk[8192];
        DWORD rc = WinHttpWebSocketReceive(ws_local, chunk, (DWORD)sizeof(chunk), &got, &type);
        if (rc != NO_ERROR) {
            if (!InterlockedCompareExchange(&n->stop, 0, 0)) queue_push(n, NET_EV_ERROR, (u32)rc, NULL, 0);
            break;
        }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            USHORT status = 0;
            u8 reason[WINHTTP_WEB_SOCKET_MAX_CLOSE_REASON_LENGTH];
            DWORD reason_len = 0;
            AcquireSRWLockExclusive(SRW(n->lock));
            if (n->websock) {
                WinHttpWebSocketQueryCloseStatus((HINTERNET)n->websock, &status, reason, (DWORD)sizeof(reason),
                                                 &reason_len);
            }
            ReleaseSRWLockExclusive(SRW(n->lock));
            queue_push(n, NET_EV_CLOSED, status, reason, (isize)reason_len);
            break;
        }
        b32 binary = (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
                      type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE);
        b32 final = (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
                     type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE);
        if (msg_len + (isize)got > NET_MSG_MAX) {
            msg_overflow = 1;  // message hors gabarit : refus explicite
        } else if (got > 0) {
            memcpy(msg + msg_len, chunk, (size_t)got);
            msg_len += (isize)got;
        }
        if (!final) continue;
        if (msg_overflow) {
            queue_push(n, NET_EV_ERROR, NET_ERR_MSG_TOO_BIG, NULL, 0);
            break;  // le flux n'est plus fiable : on ferme
        }
        if (!binary) queue_push(n, NET_EV_MESSAGE, 0, msg, msg_len);  // protocole = texte JSON
        msg_len = 0;
        msg_overflow = 0;
    }

done:
    arena_destroy(a);
    thread_finish(n);
    return 0;
}

// ------------------------------------------------------------------- API ---

b32 net_init(Net *n) {
    memset(n, 0, sizeof(*n));
    n->wakeup = (void *)CreateEventW(NULL, FALSE, FALSE, NULL);
    n->queue_arena = arena_create(NET_QUEUE_ARENA);
    if (!n->wakeup || !n->queue_arena) {
        net_destroy(n);
        return 0;
    }
    return 1;
}

b32 net_connect(Net *n, Str8 url) {
    // Idempotence : toute connexion précédente est fermée ET jointe avant
    // d'en démarrer une autre — il n'existe jamais deux threads réseau.
    net_close(n);
    if (!n->wakeup || !n->queue_arena) return 0;
    if (!net_parse_url(url, &n->url)) return 0;

    AcquireSRWLockExclusive(SRW(n->queue_lock));
    n->head = n->tail = NULL;
    n->queued = 0;
    arena_reset(n->queue_arena);
    ReleaseSRWLockExclusive(SRW(n->queue_lock));

    InterlockedExchange(&n->stop, 0);
    AcquireSRWLockExclusive(SRW(n->lock));
    n->state = NET_STATE_CONNECTING;
    ReleaseSRWLockExclusive(SRW(n->lock));

    HANDLE th = CreateThread(NULL, 0, net_thread, n, 0, NULL);
    if (!th) {
        AcquireSRWLockExclusive(SRW(n->lock));
        n->state = NET_STATE_DEAD;
        ReleaseSRWLockExclusive(SRW(n->lock));
        return 0;
    }
    n->thread = (void *)th;
    return 1;
}

b32 net_send_text(Net *n, Str8 text) {
    if (text.len <= 0) return 1;
    if (text.len > NET_MSG_MAX) return 0;
    b32 ok = 0;
    // L'envoi se fait SOUS le verrou des handles : une fermeture concurrente
    // attend la fin de l'envoi, et un envoi ne peut pas voir un handle fermé.
    AcquireSRWLockExclusive(SRW(n->lock));
    if (n->state == NET_STATE_OPEN && n->websock) {
        DWORD rc = WinHttpWebSocketSend((HINTERNET)n->websock, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                        text.data, (DWORD)text.len);
        ok = (rc == NO_ERROR);
    }
    ReleaseSRWLockExclusive(SRW(n->lock));
    return ok;
}

void net_close(Net *n) {
    InterlockedExchange(&n->stop, 1);

    AcquireSRWLockExclusive(SRW(n->lock));
    if (n->state != NET_STATE_DEAD) n->state = NET_STATE_CLOSING;
    if (n->websock) {
        // Trame de fermeture applicative (n'attend pas la réponse du pair),
        // puis fermeture des handles : la réception bloquée rend la main.
        WinHttpWebSocketShutdown((HINTERNET)n->websock, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
    }
    close_handles_locked(n);
    ReleaseSRWLockExclusive(SRW(n->lock));

    // Join SANS timeout : rien n'est libéré tant que le thread réseau vit.
    if (n->thread) {
        WaitForSingleObject((HANDLE)n->thread, INFINITE);
        CloseHandle((HANDLE)n->thread);
        n->thread = NULL;
    }

    AcquireSRWLockExclusive(SRW(n->lock));
    close_handles_locked(n);  // filet : un handle publié en course est fermé
    n->state = NET_STATE_DEAD;
    ReleaseSRWLockExclusive(SRW(n->lock));
}

void net_destroy(Net *n) {
    net_close(n);
    if (n->wakeup) {
        CloseHandle((HANDLE)n->wakeup);
        n->wakeup = NULL;
    }
    if (n->queue_arena) {
        arena_destroy(n->queue_arena);
        n->queue_arena = NULL;
    }
    n->head = n->tail = NULL;
    n->queued = 0;
}
