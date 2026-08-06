// main.c — cœur du client vibesync, sans interface graphique (VS-014, passe 1).
//
// Assemble net.c (WebSocket), vlc.c (lecteur local) et engine.c (moteur de
// sync) dans une boucle de 200 ms. L'UI immediate-mode GDI viendra dans une
// seconde passe et remplacera ce main : la boucle et l'état sont déjà là.
//
//   vibesync.exe --server wss://exemple/ws --room salon --nom thib \
//                [--mdp X] [--fichier "C:\films\ep1.mkv"]

#include "base.h"
#include "engine.h"
#include "json.h"
#include "net.h"
#include "protocol.h"
#include "vlc.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

typedef struct {
    Arena *perm;
    Arena *scratch;
    VsEngine engine;
    Net *net;
    VlcClient vlc;
    b32 vlc_running;

    char session[VS_SESSION_TOKEN_LEN + 1];
    Str8 url, name, room, password, file;

    b32 ws_open;      // handshake WebSocket établi
    i64 backoff_ns;
    i64 next_attempt;
} App;

static volatile long g_quit = 0;

static BOOL WINAPI console_ctrl(DWORD type) {
    VS_UNUSED(type);
    InterlockedExchange(&g_quit, 1);
    return TRUE;
}

static void logf_line(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

// send_msgs pousse les décisions du moteur vers le serveur. Une erreur
// d'écriture ferme la connexion (pas de perte silencieuse de `control`).
static void send_msgs(App *app, VsOutput *out) {
    for (isize i = 0; i < out->msg_count; i++) {
        TempArena t = temp_begin(app->scratch);
        Str8 raw = proto_encode_msg(app->scratch, &out->msgs[i]);
        b32 ok = app->ws_open ? net_send_text(app->net, raw) : 1;
        temp_end(t);
        if (!ok) {
            logf_line("écriture serveur impossible : fermeture et reconnexion");
            net_close(app->net);
            app->ws_open = 0;
            engine_session_lost(&app->engine);
            return;
        }
    }
    out->msg_count = 0;
}

static void apply_cmds(App *app, VsOutput *out) {
    if (!app->vlc_running) {
        out->cmd_count = 0;
        return;
    }
    for (isize i = 0; i < out->cmd_count; i++) {
        TempArena t = temp_begin(app->scratch);
        VlcError err = vlc_apply(&app->vlc, app->scratch, out->cmds[i]);
        temp_end(t);
        if (err != VLC_OK) {
            logf_line("commande VLC %s en échec : %s", vs_cmd_name(out->cmds[i].kind), vlc_error_text(err));
        }
    }
    out->cmd_count = 0;
}

static void start_connection(App *app) {
    engine_connecting(&app->engine);
    if (!net_connect(app->net, app->url)) {
        logf_line("URL invalide ou connexion impossible : %.*s", (int)app->url.len, app->url.data);
        app->backoff_ns = engine_next_backoff(app->backoff_ns);
        app->next_attempt = vs_now_ns() + app->backoff_ns;
        return;
    }
    logf_line("connexion à %.*s…", (int)app->url.len, app->url.data);
}

static void on_server_message(App *app, Str8 raw, VsOutput *out) {
    TempArena t = temp_begin(app->scratch);
    VsInMsg *m = proto_decode(app->scratch, raw);
    i64 now = vs_now_ns();
    if (!m) {
        logf_line("message serveur illisible (ignoré)");
        temp_end(t);
        return;
    }
    switch (m->kind) {
        case VS_IN_WELCOME:
            logf_line("bienvenue : salle %.*s, id %.*s", (int)m->room.len, m->room.data, (int)m->self_id.len,
                      m->self_id.data);
            engine_on_welcome(&app->engine, now, m->self_id, &m->state,
                              m->have_self_ready ? &m->self_ready : NULL, out);
            app->backoff_ns = 0;
            break;
        case VS_IN_PONG: engine_on_pong(&app->engine, now, m->pong); break;
        case VS_IN_ROOMSTATE: engine_on_roomstate(&app->engine, now, &m->state); break;
        case VS_IN_USERS:
            for (isize i = 0; i < m->user_count; i++) {
                if (str8_eq(m->users[i].id, strbuf_str(&app->engine.self_id))) {
                    engine_on_self_ready(&app->engine, m->users[i].ready);
                }
            }
            break;
        case VS_IN_TOAST: logf_line("[%.*s] %.*s", (int)m->level.len, m->level.data, (int)m->text.len, m->text.data); break;
        case VS_IN_CHATEVENT: logf_line("<%.*s> %.*s", (int)m->from.len, m->from.data, (int)m->text.len, m->text.data); break;
        case VS_IN_ERROR:
            logf_line("erreur serveur %.*s : %.*s", (int)m->code.len, m->code.data, (int)m->text.len, m->text.data);
            if (proto_error_is_fatal(m->code)) InterlockedExchange(&g_quit, 1);
            break;
        case VS_IN_UNKNOWN: break;
    }
    temp_end(t);
    send_msgs(app, out);
}

static Str8 arg_value(int argc, char **argv, int *i) {
    if (*i + 1 >= argc) return str8_lit("");
    (*i)++;
    return str8_from_cstr(argv[*i]);
}

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(console_ctrl, TRUE);

    Arena *perm = arena_create(VS_MB(8));
    Arena *scratch = arena_create(VS_MB(8));
    if (!perm || !scratch) {
        printf("mémoire insuffisante\n");
        return 2;
    }
    App *app = arena_push_struct(perm, App);
    app->perm = perm;
    app->scratch = scratch;
    app->url = str8_lit("ws://127.0.0.1:8080/ws");
    app->name = str8_lit("thib");
    app->room = str8_lit("salon");
    app->password = str8_lit("");
    app->file = str8_lit("");

    for (int i = 1; i < argc; i++) {
        Str8 arg = str8_from_cstr(argv[i]);
        if (str8_eq_cstr(arg, "--server")) app->url = arg_value(argc, argv, &i);
        else if (str8_eq_cstr(arg, "--room")) app->room = arg_value(argc, argv, &i);
        else if (str8_eq_cstr(arg, "--nom") || str8_eq_cstr(arg, "--name")) app->name = arg_value(argc, argv, &i);
        else if (str8_eq_cstr(arg, "--mdp")) app->password = arg_value(argc, argv, &i);
        else if (str8_eq_cstr(arg, "--fichier") || str8_eq_cstr(arg, "--file")) app->file = arg_value(argc, argv, &i);
        else {
            printf("usage: vibesync --server ws://hote/ws --room salon --nom thib [--mdp X] [--fichier film.mkv]\n");
            return 2;
        }
    }

    engine_init(&app->engine);
    if (!proto_session_token(app->session, (isize)sizeof(app->session))) {
        logf_line("génération du jeton de session impossible");
        return 2;
    }
    app->net = arena_push_struct(perm, Net);
    net_init(app->net);

    VsOutput out;
    vs_output_reset(&out);

    // Lancement de VLC sur le fichier demandé (facultatif à ce stade).
    if (app->file.len > 0) {
        Str8 binary;
        if (!vlc_locate(perm, &binary)) {
            logf_line("%s", vlc_error_text(VLC_ERR_NOT_FOUND));
        } else {
            logf_line("VLC : %.*s", (int)binary.len, binary.data);
            VlcError err = vlc_launch(scratch, &app->vlc, binary, app->file, 20000);
            if (err != VLC_OK) {
                logf_line("lancement de VLC : %s", vlc_error_text(err));
            } else {
                app->vlc_running = 1;
                isize slash = app->file.len;
                while (slash > 0 && app->file.data[slash - 1] != '\\' && app->file.data[slash - 1] != '/') slash--;
                engine_open_file(&app->engine, str8_sub(app->file, slash, -1), 0, &out);
            }
        }
    }

    app->next_attempt = vs_now_ns();
    i64 next_tick = vs_now_ns();

    while (!InterlockedCompareExchange(&g_quit, 0, 0)) {
        i64 now = vs_now_ns();

        if (!app->ws_open && now >= app->next_attempt && !app->net->thread) start_connection(app);

        // Événements réseau.
        NetSlot *slot = arena_push_struct(scratch, NetSlot);
        isize scratch_mark = arena_pos(scratch);
        while (net_poll(app->net, slot)) {
            switch (slot->kind) {
                case NET_EV_CONNECTED: {
                    app->ws_open = 1;
                    TempArena t = temp_begin(scratch);
                    Str8 hello = proto_encode_hello(scratch, app->name, app->room, app->password,
                                                    str8_from_cstr(app->session));
                    if (!net_send_text(app->net, hello)) {
                        logf_line("envoi du hello impossible");
                        net_close(app->net);
                        app->ws_open = 0;
                        engine_session_lost(&app->engine);
                    }
                    temp_end(t);
                    break;
                }
                case NET_EV_MESSAGE:
                    on_server_message(app, str8(slot->data, slot->len), &out);
                    break;
                case NET_EV_CLOSED:
                case NET_EV_ERROR:
                    logf_line("connexion perdue (code %lu)", (unsigned long)slot->code);
                    net_close(app->net);
                    app->ws_open = 0;
                    engine_session_lost(&app->engine);
                    app->backoff_ns = engine_next_backoff(app->backoff_ns);
                    app->next_attempt = vs_now_ns() + app->backoff_ns;
                    break;
                case NET_EV_NONE: break;
            }
            arena_pop_to(scratch, scratch_mark);
        }

        // Tic du moteur toutes les 200 ms.
        now = vs_now_ns();
        if (now >= next_tick) {
            next_tick = now + VS_POLL_INTERVAL_NS;
            if (app->vlc_running) {
                TempArena t = temp_begin(scratch);
                VsStatus st;
                VlcError err = vlc_status(&app->vlc, scratch, &st);
                if (err == VLC_OK) {
                    engine_on_vlc_status(&app->engine, now, &st, &out);
                } else {
                    engine_on_vlc_error(&app->engine);
                }
                temp_end(t);
            }
            engine_on_tick(&app->engine, now, &out);
            send_msgs(app, &out);
            apply_cmds(app, &out);
        }

        // Attente : réveil par le réseau ou échéance du prochain tic.
        i64 wait_ms = (next_tick - vs_now_ns()) / 1000000LL;
        if (wait_ms < 1) wait_ms = 1;
        if (wait_ms > 200) wait_ms = 200;
        HANDLE h = (HANDLE)net_wakeup_handle(app->net);
        if (h) {
            WaitForSingleObject(h, (DWORD)wait_ms);
        } else {
            Sleep((DWORD)wait_ms);
        }
        arena_reset(scratch);
    }

    logf_line("arrêt");
    net_destroy(app->net);
    vlc_close(&app->vlc);
    arena_destroy(scratch);
    arena_destroy(perm);
    return 0;
}
