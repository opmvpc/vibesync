// vlc_win32.c — moitié PLATEFORME du pilotage VLC (ADR-010, VS-030).
//
// Socket Winsock vers l'interface HTTP locale, localisation de l'exécutable
// (variables d'environnement + chemins Windows, en UTF-16) et lancement du
// process. Toute la logique testable sans réseau ni process est dans
// vlc_core.c.
#include "vlc.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <string.h>

// ---------------------------------------------------------- socket Winsock ---

static b32 g_wsa_ready = 0;

static b32 winsock_init(void) {
    if (g_wsa_ready) return 1;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
    g_wsa_ready = 1;
    return 1;
}

// http_get exécute une requête complète vers l'interface locale de VLC.
static VlcError http_get(VlcClient *c, Arena *scratch, Str8 path, Str8 *body_out) {
    if (!winsock_init()) return VLC_ERR_SOCKET;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return VLC_ERR_SOCKET;

    DWORD timeout = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u16)c->port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(s);
        return VLC_ERR_CONNECT;
    }

    Str8 req = vlc_build_request(scratch, path, str8_from_cstr(c->auth_b64), c->port);
    isize sent = 0;
    while (sent < req.len) {
        int n = send(s, (const char *)req.data + sent, (int)(req.len - sent), 0);
        if (n <= 0) {
            closesocket(s);
            return VLC_ERR_SEND;
        }
        sent += n;
    }
    shutdown(s, SD_SEND);

    Builder resp;
    builder_init(&resp, scratch, VS_KB(16));
    char chunk[4096];
    for (;;) {
        int n = recv(s, chunk, (int)sizeof(chunk), 0);
        if (n == 0) break;
        if (n < 0) {
            closesocket(s);
            return VLC_ERR_RECV;
        }
        builder_bytes(&resp, chunk, n);
        if (resp.len > VS_MB(4)) break;  // borne dure : VLC ne renvoie que du JSON court
    }
    closesocket(s);

    int code = 0;
    Str8 body;
    if (!http_parse_response(scratch, builder_result(&resp), &code, &body)) return VLC_ERR_RECV;
    if (code == 401) return VLC_ERR_AUTH;
    if (code != 200) return VLC_ERR_HTTP;
    *body_out = body;
    return VLC_OK;
}

// ------------------------------------------------------------ localisation ---

static b32 file_exists(const u16 *path) {
    DWORD attrs = GetFileAttributesW((LPCWSTR)path);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// env_var lit une variable d'environnement en UTF-8 (chaîne vide si absente).
// Le résultat vit dans l'arène de l'appelant.
static Str8 env_var(Arena *a, const char *name) {
    u16 *wname = utf8_to_utf16(a, str8_from_cstr(name), NULL);
    DWORD n = GetEnvironmentVariableW((LPCWSTR)wname, NULL, 0);
    if (n == 0) return str8_lit("");
    u16 *buf = arena_push_array(a, u16, (isize)n + 1);
    DWORD got = GetEnvironmentVariableW((LPCWSTR)wname, (LPWSTR)buf, n);
    if (got == 0 || got > n) return str8_lit("");
    return utf16_to_utf8(a, buf);
}

static b32 try_candidate(Arena *a, Str8 path, Str8 *out) {
    if (path.len == 0) return 0;
    TempArena t = temp_begin(a);
    u16 *w = utf8_to_utf16(a, path, NULL);
    b32 ok = file_exists(w);
    temp_end(t);
    if (!ok) return 0;
    *out = str8_copy(a, path);
    return 1;
}

b32 vlc_locate(Arena *a, Str8 *out_path) {
    Str8 forced = env_var(a, "VIBESYNC_VLC");
    if (forced.len > 0) return try_candidate(a, forced, out_path);

    if (try_candidate(a, str8_lit("C:\\Program Files\\VideoLAN\\VLC\\vlc.exe"), out_path)) return 1;
    if (try_candidate(a, str8_lit("C:\\Program Files (x86)\\VideoLAN\\VLC\\vlc.exe"), out_path)) return 1;

    static const char *vars[] = {"ProgramFiles", "ProgramFiles(x86)", "ProgramW6432"};
    for (isize i = 0; i < (isize)(sizeof(vars) / sizeof(vars[0])); i++) {
        Str8 base = env_var(a, vars[i]);
        if (base.len == 0) continue;
        Str8 cand = str8_cat(a, base, str8_lit("\\VideoLAN\\VLC\\vlc.exe"));
        if (try_candidate(a, cand, out_path)) return 1;
    }
    Str8 local = env_var(a, "LOCALAPPDATA");
    if (local.len > 0) {
        if (try_candidate(a, str8_cat(a, local, str8_lit("\\Programs\\VideoLAN\\VLC\\vlc.exe")), out_path)) return 1;
        if (try_candidate(a, str8_cat(a, local, str8_lit("\\Programs\\VLC\\vlc.exe")), out_path)) return 1;
    }
    return 0;
}

// ----------------------------------------------------------------- lancement ---
// free_port réserve puis relâche un port libre sur la loopback.
static int free_port(void) {
    if (!winsock_init()) return 0;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(s);
        return 0;
    }
    int len = (int)sizeof(addr);
    if (getsockname(s, (struct sockaddr *)&addr, &len) != 0) {
        closesocket(s);
        return 0;
    }
    int port = ntohs(addr.sin_port);
    closesocket(s);
    return port;
}

VlcError vlc_launch(Arena *scratch, VlcClient *c, Str8 binary, Str8 file_path, i64 timeout_ms) {
    if (binary.len == 0 || file_path.len == 0) return VLC_ERR_NOT_FOUND;
    int port = free_port();
    if (port == 0) return VLC_ERR_SOCKET;
    u8 rnd[16];
    char password[33];
    if (!vs_random_bytes(rnd, (isize)sizeof(rnd))) return VLC_ERR_SPAWN;
    vs_hex_encode(rnd, (isize)sizeof(rnd), password);
    vlc_client_init(c, port, str8_from_cstr(password));

    TempArena t = temp_begin(scratch);
    Str8 cmdline = vlc_build_command(scratch, binary, file_path, port, str8_from_cstr(password));
    u16 *wcmd = utf8_to_utf16(scratch, cmdline, NULL);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    BOOL ok = CreateProcessW(NULL, (LPWSTR)wcmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    temp_end(t);
    if (!ok) return VLC_ERR_SPAWN;
    CloseHandle(pi.hThread);
    c->process = pi.hProcess;

    // Attente de l'interface HTTP (VLC met un instant à démarrer).
    if (timeout_ms <= 0) timeout_ms = 20000;
    i64 deadline = vs_now_ns() + timeout_ms * 1000000LL;
    VlcError err = VLC_ERR_TIMEOUT;
    for (;;) {
        VsStatus st;
        TempArena tt = temp_begin(scratch);
        err = vlc_status(c, scratch, &st);
        temp_end(tt);
        if (err == VLC_OK) break;
        if (err == VLC_ERR_AUTH) break;
        if (vs_now_ns() > deadline) {
            err = VLC_ERR_TIMEOUT;
            break;
        }
        Sleep(100);
    }
    if (err == VLC_OK) err = vlc_prepare_paused(c, scratch, timeout_ms);
    if (err != VLC_OK) {
        // Trace : un échec d'attache chez un ami est indébogable sans elle. Le
        // mot de passe de l'interface n'y figure JAMAIS.
        vs_log("vlc: attache en échec (%s) — exe=\"%.*s\" port=%d fichier=\"%.*s\"", vlc_error_text(err),
               (int)binary.len, (const char *)binary.data, port, (int)file_path.len,
               (const char *)file_path.data);
        vs_log("vlc: drapeaux forcés — extraintf=http, lua-intf=http, no-one-instance, "
               "no-one-instance-when-started-from-file, no-playlist-enqueue, playlist-autostart, "
               "start-paused, no-random, no-loop, no-repeat, no-play-and-exit");
        vs_log("vlc: marche à suivre — si l'attache échoue malgré ces drapeaux, la configuration de VLC "
               "impose autre chose : fermez VLC, renommez le fichier vlcrc du dossier de configuration de "
               "VLC (sous-dossier vlc du dossier Roaming de votre profil), puis réessayez. Un VLC préparé "
               "par Syncplay est le cas connu.");
        // Ni orphelin ni handle qui fuit : le VLC qu'on vient de lancer est
        // arrêté avant de rendre l'erreur — pas de fenêtre qui joue toute seule.
        b32 keep = c->keep_alive;
        c->keep_alive = 0;
        vlc_close(c);
        c->keep_alive = keep;
    } else {
        vs_log("vlc: attaché sur le port %d — fichier \"%.*s\"", port, (int)file_path.len,
               (const char *)file_path.data);
    }
    return err;
}

// vlc_prepare_paused met un média fraîchement ouvert en pause à la position 0
// et ne rend la main qu'une fois cet état **observé** (docs/protocol.md
// §Chargement de fichier, port de internal/vlc.Prepare).
//
// VLC démarre la lecture tout seul à l'ouverture : sans cette étape, deux
// clients qui ouvrent leur média à quelques centaines de millisecondes d'écart
// démarrent déjà désynchronisés, et le rattrapage au rate (5 %/s) mettrait une
// dizaine de secondes. La boucle est idempotente : on redemande pause et seek 0
// tant que l'état visé n'est pas constaté, ce qui absorbe le délai d'ouverture
// du média comme les commandes perdues.
VlcError vlc_prepare_paused(VlcClient *c, Arena *scratch, i64 timeout_ms) {
    if (timeout_ms <= 0) timeout_ms = VLC_PREPARE_TIMEOUT_MS;
    i64 deadline = vs_now_ns() + timeout_ms * 1000000LL;
    VlcError last = VLC_ERR_TIMEOUT;
    for (;;) {
        TempArena t = temp_begin(scratch);
        VsStatus st;
        b32 ready = 0;
        last = vlc_status(c, scratch, &st);
        if (last == VLC_OK) {
            if (!vs_status_loaded(&st)) {
                last = VLC_ERR_TIMEOUT;  // média pas encore ouvert : rien à commander
            } else {
                b32 at_start = st.position_sec < VLC_START_TOLERANCE;
                if (st.state == VS_PLAY_PAUSED && at_start) {
                    ready = 1;
                } else {
                    if (st.state == VS_PLAY_PLAYING) last = vlc_pause(c, scratch);
                    if (!at_start) {
                        VlcError e2 = vlc_seek(c, scratch, 0);
                        if (last == VLC_OK) last = e2;
                    }
                    if (last == VLC_OK) last = VLC_ERR_TIMEOUT;  // pas encore constaté
                }
            }
        }
        temp_end(t);
        if (ready) return VLC_OK;
        if (last == VLC_ERR_AUTH) return last;
        if (vs_now_ns() > deadline) return last;
        Sleep(VLC_PREPARE_POLL_MS);
    }
}

void vlc_close(VlcClient *c) {
    if (c->process) {
        if (!c->keep_alive) {
            TerminateProcess((HANDLE)c->process, 0);
            WaitForSingleObject((HANDLE)c->process, 2000);
        }
        CloseHandle((HANDLE)c->process);
        c->process = NULL;
    }
}

// ----------------------------------------------------------------- commandes ---

VlcError vlc_status(VlcClient *c, Arena *scratch, VsStatus *out) {
    Str8 body;
    VlcError err = http_get(c, scratch, str8_lit("/requests/status.json"), &body);
    if (err != VLC_OK) return err;
    if (!vlc_parse_status(scratch, body, out)) return VLC_ERR_JSON;
    return VLC_OK;
}

static VlcError command(VlcClient *c, Arena *scratch, const char *name, const char *value) {
    Builder path;
    builder_init(&path, scratch, 128);
    builder_cstr(&path, "/requests/status.json?command=");
    builder_cstr(&path, name);
    if (value && value[0]) {
        builder_cstr(&path, "&val=");
        builder_cstr(&path, value);
    }
    Str8 body;
    return http_get(c, scratch, builder_result(&path), &body);
}

VlcError vlc_pause(VlcClient *c, Arena *scratch) { return command(c, scratch, "pl_forcepause", NULL); }
VlcError vlc_resume(VlcClient *c, Arena *scratch) { return command(c, scratch, "pl_forceresume", NULL); }

VlcError vlc_seek(VlcClient *c, Arena *scratch, f64 position_sec) {
    // L'API HTTP de VLC n'accepte que des secondes entières.
    if (!f64_is_finite(position_sec) || position_sec < 0) position_sec = 0;
    char val[24];
    i64_to_str((i64)f64_round(position_sec), val, (isize)sizeof(val));
    return command(c, scratch, "seek", val);
}

VlcError vlc_set_rate(VlcClient *c, Arena *scratch, f64 rate) {
    if (!f64_is_finite(rate) || rate <= 0) rate = 1;
    char val[40];
    f64_to_str(rate, val, (isize)sizeof(val));
    return command(c, scratch, "rate", val);
}

VlcError vlc_apply(VlcClient *c, Arena *scratch, VsCmd cmd) {
    switch (cmd.kind) {
        case VS_CMD_PAUSE: return vlc_pause(c, scratch);
        case VS_CMD_RESUME: return vlc_resume(c, scratch);
        case VS_CMD_SEEK: return vlc_seek(c, scratch, cmd.value);
        case VS_CMD_RATE: return vlc_set_rate(c, scratch, cmd.value);
    }
    return VLC_OK;
}
