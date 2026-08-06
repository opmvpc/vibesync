#include "net.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <string.h>

_Static_assert(sizeof(SRWLOCK) == sizeof(void *), "SRWLOCK tient dans un pointeur");

#define SRW(field) ((PSRWLOCK)(void *)&(field))

// ----------------------------------------------------------------- URL ---

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
        isize close = str8_find_char(authority, ']', 0);
        if (close < 0) return 0;
        host = str8_sub(authority, 1, close - 1);
        Str8 after = str8_sub(authority, close + 1, -1);
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

// -------------------------------------------------------------- file SPSC ---

static void queue_push(Net *n, NetEventKind kind, u32 code, const u8 *data, isize len) {
    if (len > NET_MSG_MAX) len = NET_MSG_MAX;
    AcquireSRWLockExclusive(SRW(n->lock));
    isize next = (n->tail + 1) % NET_QUEUE_SLOTS;
    if (next == n->head) {
        n->dropped++;  // file pleine : la boucle principale est en retard
        ReleaseSRWLockExclusive(SRW(n->lock));
        return;
    }
    NetSlot *s = &n->slots[n->tail];
    s->kind = kind;
    s->code = code;
    s->len = len;
    if (len > 0 && data) memcpy(s->data, data, (size_t)len);
    n->tail = next;
    ReleaseSRWLockExclusive(SRW(n->lock));
    if (n->wakeup) SetEvent((HANDLE)n->wakeup);
}

b32 net_poll(Net *n, NetSlot *out) {
    b32 got = 0;
    AcquireSRWLockExclusive(SRW(n->lock));
    if (n->head != n->tail) {
        NetSlot *s = &n->slots[n->head];
        out->kind = s->kind;
        out->code = s->code;
        out->len = s->len;
        if (s->len > 0) memcpy(out->data, s->data, (size_t)s->len);
        n->head = (n->head + 1) % NET_QUEUE_SLOTS;
        got = 1;
    }
    ReleaseSRWLockExclusive(SRW(n->lock));
    return got;
}

void *net_wakeup_handle(Net *n) { return n->wakeup; }

// ----------------------------------------------------------- thread réseau ---

static void net_teardown_handles(Net *n) {
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

static DWORD WINAPI net_thread(LPVOID param) {
    Net *n = (Net *)param;
    Arena *a = arena_create(VS_KB(256));
    if (!a) {
        queue_push(n, NET_EV_ERROR, 0, NULL, 0);
        InterlockedExchange(&n->running, 0);
        return 1;
    }

    u16 *whost = utf8_to_utf16(a, str8_from_cstr(n->url.host), NULL);
    u16 *wpath = utf8_to_utf16(a, str8_from_cstr(n->url.path), NULL);

    HINTERNET session = WinHttpOpen(L"vibesync/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        queue_push(n, NET_EV_ERROR, (u32)GetLastError(), NULL, 0);
        goto done;
    }
    n->session = session;
    {
        DWORD tmo = 10000;
        WinHttpSetTimeouts(session, (int)tmo, (int)tmo, (int)tmo, 0 /* réception : sans limite */);
    }

    HINTERNET connect = WinHttpConnect(session, (LPCWSTR)whost, (INTERNET_PORT)n->url.port, 0);
    if (!connect) {
        queue_push(n, NET_EV_ERROR, (u32)GetLastError(), NULL, 0);
        goto done;
    }
    n->connect = connect;

    DWORD flags = n->url.secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", (LPCWSTR)wpath, NULL, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        queue_push(n, NET_EV_ERROR, (u32)GetLastError(), NULL, 0);
        goto done;
    }
    n->request = request;

    if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0)) {
        queue_push(n, NET_EV_ERROR, (u32)GetLastError(), NULL, 0);
        goto done;
    }
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL)) {
        queue_push(n, NET_EV_ERROR, (u32)GetLastError(), NULL, 0);
        goto done;
    }
    HINTERNET ws = WinHttpWebSocketCompleteUpgrade(request, 0);
    if (!ws) {
        queue_push(n, NET_EV_ERROR, (u32)GetLastError(), NULL, 0);
        goto done;
    }
    // Le handle de requête n'est plus utile une fois l'upgrade complété.
    WinHttpCloseHandle(request);
    n->request = NULL;
    n->websock = ws;
    queue_push(n, NET_EV_CONNECTED, 0, NULL, 0);

    // Boucle de réception : réassemblage des fragments jusqu'au message complet.
    u8 *msg = arena_push_array(a, u8, NET_MSG_MAX);
    isize msg_len = 0;
    b32 msg_overflow = 0;
    u8 chunk[4096];
    for (;;) {
        if (InterlockedCompareExchange(&n->stop, 0, 0)) break;
        DWORD got = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
        DWORD rc = WinHttpWebSocketReceive(ws, chunk, (DWORD)sizeof(chunk), &got, &type);
        if (rc != NO_ERROR) {
            if (!InterlockedCompareExchange(&n->stop, 0, 0)) {
                queue_push(n, NET_EV_ERROR, (u32)rc, NULL, 0);
            }
            break;
        }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            USHORT status = 0;
            u8 reason[WINHTTP_WEB_SOCKET_MAX_CLOSE_REASON_LENGTH];
            DWORD reason_len = 0;
            WinHttpWebSocketQueryCloseStatus(ws, &status, reason, (DWORD)sizeof(reason), &reason_len);
            queue_push(n, NET_EV_CLOSED, status, reason, (isize)reason_len);
            break;
        }
        b32 binary = (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
                      type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE);
        b32 final = (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
                     type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE);
        if (msg_len + (isize)got > NET_MSG_MAX) {
            msg_overflow = 1;  // message hors gabarit : on le jette proprement
        } else if (got > 0) {
            memcpy(msg + msg_len, chunk, (size_t)got);
            msg_len += (isize)got;
        }
        if (!final) continue;
        if (msg_overflow) {
            queue_push(n, NET_EV_ERROR, (u32)ERROR_INSUFFICIENT_BUFFER, NULL, 0);
        } else if (!binary) {  // le protocole vibesync est en texte JSON
            queue_push(n, NET_EV_MESSAGE, 0, msg, msg_len);
        }
        msg_len = 0;
        msg_overflow = 0;
    }

done:
    net_teardown_handles(n);
    arena_destroy(a);
    InterlockedExchange(&n->running, 0);
    if (n->wakeup) SetEvent((HANDLE)n->wakeup);
    return 0;
}

// ------------------------------------------------------------------- API ---

void net_init(Net *n) {
    memset(n, 0, sizeof(*n));
    n->wakeup = (void *)CreateEventW(NULL, FALSE, FALSE, NULL);
}

b32 net_connect(Net *n, Str8 url) {
    if (n->thread) return 0;  // déjà connecté
    if (!net_parse_url(url, &n->url)) return 0;
    if (!n->wakeup) n->wakeup = (void *)CreateEventW(NULL, FALSE, FALSE, NULL);
    n->head = n->tail = 0;
    n->dropped = 0;
    InterlockedExchange(&n->stop, 0);
    InterlockedExchange(&n->running, 1);
    HANDLE th = CreateThread(NULL, 0, net_thread, n, 0, NULL);
    if (!th) {
        InterlockedExchange(&n->running, 0);
        return 0;
    }
    n->thread = (void *)th;
    return 1;
}

b32 net_send_text(Net *n, Str8 text) {
    if (text.len <= 0) return 1;
    b32 ok = 0;
    AcquireSRWLockExclusive(SRW(n->send_lock));
    HINTERNET ws = (HINTERNET)n->websock;
    if (ws) {
        DWORD rc = WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, text.data,
                                        (DWORD)text.len);
        ok = (rc == NO_ERROR);
    }
    ReleaseSRWLockExclusive(SRW(n->send_lock));
    return ok;
}

void net_close(Net *n) {
    InterlockedExchange(&n->stop, 1);
    AcquireSRWLockExclusive(SRW(n->send_lock));
    HINTERNET ws = (HINTERNET)n->websock;
    if (ws) {
        // Fermeture applicative puis fermeture du handle : le Receive bloqué
        // du thread réseau rend la main immédiatement.
        WinHttpWebSocketClose(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
    }
    ReleaseSRWLockExclusive(SRW(n->send_lock));
    if (n->thread) {
        if (WaitForSingleObject((HANDLE)n->thread, 3000) == WAIT_TIMEOUT) {
            // Dernier recours : fermer les handles réveille la réception.
            net_teardown_handles(n);
            WaitForSingleObject((HANDLE)n->thread, 2000);
        }
        CloseHandle((HANDLE)n->thread);
        n->thread = NULL;
    }
    net_teardown_handles(n);
}

void net_destroy(Net *n) {
    net_close(n);
    if (n->wakeup) {
        CloseHandle((HANDLE)n->wakeup);
        n->wakeup = NULL;
    }
}
