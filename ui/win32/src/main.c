// main.c — application graphique vibesync (VS-014, passe 2).
//
// Une fenêtre Win32, une UI immediate-mode dessinée en GDI sur back-buffer
// (ui.c), le moteur de sync (engine.c), le WebSocket (net.c) et VLC (vlc.c).
//
// Répartition des threads :
//   - thread UI : fenêtre, dessin, MOTEUR. Seul à toucher VsEngine.
//   - thread réseau (net.c) : WinHTTP ; pousse ses événements dans sa file et
//     réveille l'UI par PostMessage(WM_APP_NET).
//   - thread VLC : HTTP local bloquant (statut, commandes, lancement) ; publie
//     le dernier statut sous verrou et réveille l'UI par PostMessage.
// Aucune structure du moteur n'est touchée hors du thread UI.

#include "base.h"
#include "engine.h"
#include "ini.h"
#include "json.h"
#include "net.h"
#include "protocol.h"
#include "ui.h"
#include "vlc.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shobjidl.h>

#include <stdio.h>
#include <string.h>

#define WM_APP_NET (WM_APP + 1)
#define WM_APP_VLC (WM_APP + 2)
#define WM_APP_VLC_OPEN (WM_APP + 3)

#define TIMER_ENGINE 1
#define TIMER_UI 2
#define TIMER_SHOT 3

// --------------------------------------------------------- travailleur VLC ---

typedef struct {
    VlcClient client;
    HWND hwnd;
    HANDLE thread;
    HANDLE wake;
    volatile long stop;
    SRWLOCK lock;

    b32 running;  // VLC lancé et piloté (sous lock)
    VsStatus status;
    b32 status_ok;

    VsCmd cmds[32];
    isize cmd_count;

    b32 open_pending;
    StrBuf open_path;
    b32 open_ok;
    b32 open_failed;
    char open_error[192];
    StrBuf open_name;
    i64 open_size;
} VlcWorker;

static DWORD WINAPI vlc_thread(LPVOID param) {
    VlcWorker *w = (VlcWorker *)param;
    Arena *scratch = arena_create(VS_MB(4));
    if (!scratch) return 1;
    while (!InterlockedCompareExchange(&w->stop, 0, 0)) {
        WaitForSingleObject(w->wake, 200);
        if (InterlockedCompareExchange(&w->stop, 0, 0)) break;

        // 1. Ouverture d'un fichier (bloquant : jusqu'à 20 s).
        StrBuf path;
        b32 want_open = 0;
        AcquireSRWLockExclusive(&w->lock);
        if (w->open_pending) {
            path = w->open_path;
            w->open_pending = 0;
            want_open = 1;
        }
        ReleaseSRWLockExclusive(&w->lock);
        if (want_open) {
            VlcClient fresh;
            memset(&fresh, 0, sizeof(fresh));
            Str8 binary;
            VlcError err = VLC_ERR_NOT_FOUND;
            TempArena t = temp_begin(scratch);
            if (vlc_locate(scratch, &binary)) {
                err = vlc_launch(scratch, &fresh, binary, strbuf_str(&path), 20000);
            }
            temp_end(t);
            AcquireSRWLockExclusive(&w->lock);
            if (err == VLC_OK) {
                if (w->running) vlc_close(&w->client);
                w->client = fresh;
                w->running = 1;
                w->open_ok = 1;
                w->open_failed = 0;
                w->open_error[0] = 0;
                Str8 p = strbuf_str(&path);
                isize slash = p.len;
                while (slash > 0 && p.data[slash - 1] != '\\' && p.data[slash - 1] != '/') slash--;
                strbuf_set(&w->open_name, str8_sub(p, slash, -1));
                w->open_size = 0;
                WIN32_FILE_ATTRIBUTE_DATA fad;
                TempArena t2 = temp_begin(scratch);
                u16 *wp = utf8_to_utf16(scratch, p, NULL);
                if (GetFileAttributesExW((LPCWSTR)wp, GetFileExInfoStandard, &fad)) {
                    w->open_size = ((i64)fad.nFileSizeHigh << 32) | (i64)fad.nFileSizeLow;
                }
                temp_end(t2);
            } else {
                w->open_ok = 0;
                w->open_failed = 1;
                snprintf(w->open_error, sizeof(w->open_error), "%s", vlc_error_text(err));
            }
            ReleaseSRWLockExclusive(&w->lock);
            PostMessageW(w->hwnd, WM_APP_VLC_OPEN, 0, 0);
        }

        // 2. Commandes décidées par le moteur.
        VsCmd cmds[32];
        isize n = 0;
        AcquireSRWLockExclusive(&w->lock);
        n = w->cmd_count;
        if (n > 0) memcpy(cmds, w->cmds, sizeof(VsCmd) * (size_t)n);
        w->cmd_count = 0;
        b32 running = w->running;
        ReleaseSRWLockExclusive(&w->lock);
        for (isize i = 0; i < n && running; i++) {
            TempArena t = temp_begin(scratch);
            vlc_apply(&w->client, scratch, cmds[i]);
            temp_end(t);
        }

        // 3. Statut courant, publié pour le thread UI.
        if (running) {
            VsStatus st;
            TempArena t = temp_begin(scratch);
            VlcError err = vlc_status(&w->client, scratch, &st);
            temp_end(t);
            AcquireSRWLockExclusive(&w->lock);
            w->status_ok = (err == VLC_OK);
            if (err == VLC_OK) w->status = st;
            ReleaseSRWLockExclusive(&w->lock);
            PostMessageW(w->hwnd, WM_APP_VLC, 0, 0);
        }
    }
    arena_destroy(scratch);
    return 0;
}

static void worker_start(VlcWorker *w, HWND hwnd) {
    memset(w, 0, sizeof(*w));
    InitializeSRWLock(&w->lock);
    w->hwnd = hwnd;
    w->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    w->thread = CreateThread(NULL, 0, vlc_thread, w, 0, NULL);
}

static void worker_stop(VlcWorker *w) {
    InterlockedExchange(&w->stop, 1);
    if (w->wake) SetEvent(w->wake);
    if (w->thread) {
        WaitForSingleObject(w->thread, 25000);
        CloseHandle(w->thread);
        w->thread = NULL;
    }
    if (w->wake) {
        CloseHandle(w->wake);
        w->wake = NULL;
    }
    if (w->running) {
        vlc_close(&w->client);
        w->running = 0;
    }
}

static void worker_open(VlcWorker *w, Str8 path) {
    AcquireSRWLockExclusive(&w->lock);
    strbuf_set(&w->open_path, path);
    w->open_pending = 1;
    ReleaseSRWLockExclusive(&w->lock);
    SetEvent(w->wake);
}

static void worker_push_cmds(VlcWorker *w, const VsCmd *cmds, isize count) {
    if (count <= 0) return;
    AcquireSRWLockExclusive(&w->lock);
    for (isize i = 0; i < count && w->cmd_count < VS_ARRAY_COUNT(w->cmds); i++) {
        w->cmds[w->cmd_count++] = cmds[i];
    }
    ReleaseSRWLockExclusive(&w->lock);
    SetEvent(w->wake);
}

// -------------------------------------------------------------- application ---

typedef struct {
    Arena *perm;
    Arena *scratch;
    VsEngine engine;
    Net *net;
    UiApp ui;
    VlcWorker vlc;
    Ini ini;

    HWND hwnd;
    HDC mem_dc;
    HBITMAP bmp;
    void *bits;
    i32 bw, bh;

    char session[VS_SESSION_TOKEN_LEN + 1];
    Str8 url, name, room, password;
    b32 ws_open;
    i64 backoff_ns;
    i64 next_attempt;
    b32 engine_timer_on;
    b32 ui_timer_on;
    i64 start_ticks;
    b32 first_paint;
    // Mode « smoke » : capture de l'écran réel après connexion, puis sortie.
    Str8 shot_path;
    Str8 auto_chat;  // message envoyé dès le welcome (diagnostic)
} App;

static App *g_app;

static i64 now_ms(void) { return (i64)GetTickCount64(); }

// --- réglages ---

static void settings_load(App *app) {
    Str8 path = ini_path(app->perm);
    ini_load_file(app->perm, path, &app->ini);
    Str8 v;
    v = ini_get(&app->ini, "serveur", str8_lit(""));
    if (v.len) ui_text_set(&app->ui.f_server, v);
    v = ini_get(&app->ini, "pseudo", str8_lit(""));
    if (v.len) ui_text_set(&app->ui.f_name, v);
    v = ini_get(&app->ini, "salle", str8_lit(""));
    if (v.len) ui_text_set(&app->ui.f_room, v);
    // Le mot de passe n'est volontairement pas mémorisé.
}

static void settings_save(App *app) {
    TempArena t = temp_begin(app->scratch);
    Ini ini;
    ini_clear(&ini);
    ini_set(app->scratch, &ini, "serveur", ui_text_str(&app->ui.f_server));
    ini_set(app->scratch, &ini, "pseudo", ui_text_str(&app->ui.f_name));
    ini_set(app->scratch, &ini, "salle", ui_text_str(&app->ui.f_room));
    ini_save_file(app->scratch, ini_path(app->scratch), ini_write(app->scratch, &ini));
    temp_end(t);
}

// --- sorties du moteur ---

static void dispatch_output(App *app, VsOutput *out) {
    for (isize i = 0; i < out->msg_count; i++) {
        TempArena t = temp_begin(app->scratch);
        Str8 raw = proto_encode_msg(app->scratch, &out->msgs[i]);
        b32 ok = app->ws_open ? net_send_text(app->net, raw) : 1;
        temp_end(t);
        if (!ok) {
            // Une erreur d'écriture ferme la session : pas de perte silencieuse.
            net_close(app->net);
            app->ws_open = 0;
            engine_session_lost(&app->engine);
            break;
        }
    }
    out->msg_count = 0;
    worker_push_cmds(&app->vlc, out->cmds, out->cmd_count);
    out->cmd_count = 0;
}

// --- vue : recopie de l'état moteur vers l'UI (thread UI uniquement) ---

static void refresh_view(App *app) {
    UiApp *ui = &app->ui;
    ui->phase = app->engine.phase;
    ui->connecting = app->engine.phase == VS_PHASE_CONNECTING;
    ui->retrying = ui->connecting && app->backoff_ns > 0;
    ui->ready = app->engine.ready;
    ui->paused = app->engine.room_state.paused || !app->engine.have_state;
    ui->latency_ms = app->engine.latency_ms;
    ui->drift_sec = app->engine.drift;
    ui->correcting = app->engine.correcting != VS_CORRECT_NONE;
    ui->buffering = app->engine.buffering;
    ui->position_sec = app->engine.have_status ? app->engine.status.position_sec : 0;
    ui->duration_sec = app->engine.have_status ? app->engine.status.length_sec : 0;
    AcquireSRWLockExclusive(&app->vlc.lock);
    ui->vlc_running = app->vlc.running;
    ReleaseSRWLockExclusive(&app->vlc.lock);
    if (app->engine.have_file) {
        snprintf(ui->file_name, sizeof(ui->file_name), "%s", (const char *)app->engine.file_name.data);
    }
}

static void redraw(App *app) {
    if (app->hwnd) InvalidateRect(app->hwnd, NULL, FALSE);
}

static void set_timers(App *app) {
    b32 want_engine = (app->engine.phase != VS_PHASE_IDLE) || app->ws_open;
    AcquireSRWLockExclusive(&app->vlc.lock);
    if (app->vlc.running) want_engine = 1;
    ReleaseSRWLockExclusive(&app->vlc.lock);
    if (want_engine != app->engine_timer_on) {
        if (want_engine) SetTimer(app->hwnd, TIMER_ENGINE, 200, NULL);
        else KillTimer(app->hwnd, TIMER_ENGINE);
        app->engine_timer_on = want_engine;
    }
    b32 want_ui = app->ui.need_timer;
    if (want_ui != app->ui_timer_on) {
        if (want_ui) SetTimer(app->hwnd, TIMER_UI, 100, NULL);
        else KillTimer(app->hwnd, TIMER_UI);
        app->ui_timer_on = want_ui;
    }
}

// --- moteur (thread UI) ---

static void engine_step(App *app) {
    VsOutput out;
    vs_output_reset(&out);
    i64 now = vs_now_ns();

    VsStatus st;
    b32 have = 0, ok = 0;
    AcquireSRWLockExclusive(&app->vlc.lock);
    if (app->vlc.running) {
        have = 1;
        ok = app->vlc.status_ok;
        st = app->vlc.status;
    }
    ReleaseSRWLockExclusive(&app->vlc.lock);
    if (have) {
        if (ok) engine_on_vlc_status(&app->engine, now, &st, &out);
        else engine_on_vlc_error(&app->engine);
    }
    engine_on_tick(&app->engine, now, &out);
    dispatch_output(app, &out);

    // Reconnexion : backoff 1 s → 10 s.
    if (!app->ws_open && app->engine.phase != VS_PHASE_IDLE && now >= app->next_attempt &&
        net_state(app->net) == NET_STATE_DEAD) {
        if (!net_connect(app->net, app->url)) {
            app->backoff_ns = engine_next_backoff(app->backoff_ns);
            app->next_attempt = now + app->backoff_ns;
            ui_set_status(&app->ui, "URL de serveur invalide.", 1);
        }
    }
    refresh_view(app);
}

// --- messages serveur ---

static void fill_users(App *app, VsInMsg *m) {
    UiApp *ui = &app->ui;
    ui->user_count = VS_MIN(m->user_count, (isize)UI_MAX_USERS);
    for (isize i = 0; i < ui->user_count; i++) {
        VsUser *u = &m->users[i];
        UiUser *d = &ui->users[i];
        memset(d, 0, sizeof(*d));
        snprintf(d->name, sizeof(d->name), "%.*s", (int)u->name.len, u->name.data);
        d->ready = u->ready;
        d->latency_ms = u->latency_ms;
        d->is_self = strbuf_eq(&app->engine.self_id, u->id);
        if (u->has_file) {
            d->has_file = 1;
            snprintf(d->file, sizeof(d->file), "%.*s", (int)u->file_name.len, u->file_name.data);
        }
    }
}

static void on_server_message(App *app, Str8 raw) {
    TempArena t = temp_begin(app->scratch);
    VsInMsg *m = proto_decode(app->scratch, raw);
    if (!m || m->invalid) {
        temp_end(t);
        return;
    }
    VsOutput out;
    vs_output_reset(&out);
    i64 now = vs_now_ns();
    switch (m->kind) {
        case VS_IN_WELCOME:
            engine_on_welcome(&app->engine, now, m->self_id, &m->state,
                              m->have_self_ready ? &m->self_ready : NULL, &out);
            app->backoff_ns = 0;
            fill_users(app, m);
            app->ui.screen = UI_SCREEN_ROOM;
            app->ui.connecting = 0;
            if (m->room.len > 0) {
                snprintf(app->ui.room, sizeof(app->ui.room), "%.*s", (int)m->room.len, m->room.data);
            } else {
                snprintf(app->ui.room, sizeof(app->ui.room), "%.*s", (int)app->room.len, app->room.data);
            }
            ui_set_status(&app->ui, "", 0);
            ui_chat_add(&app->ui, str8_lit(""), str8_lit("Connecté à la salle."), 1);
            if (app->auto_chat.len > 0) engine_chat(&app->engine, app->auto_chat, &out);  // --chat
            break;
        case VS_IN_PONG: engine_on_pong(&app->engine, now, m->pong); break;
        case VS_IN_ROOMSTATE: engine_on_roomstate(&app->engine, now, &m->state); break;
        case VS_IN_USERS:
            fill_users(app, m);
            for (isize i = 0; i < m->user_count; i++) {
                if (strbuf_eq(&app->engine.self_id, m->users[i].id)) {
                    engine_on_self_ready(&app->engine, m->users[i].ready);
                }
            }
            break;
        case VS_IN_TOAST: {
            char text[224];
            snprintf(text, sizeof(text), "%.*s", (int)m->text.len, m->text.data);
            int level = str8_eq_cstr(m->level, "error") ? 2 : (str8_eq_cstr(m->level, "warn") ? 1 : 0);
            ui_toast(&app->ui, text, level, now_ms());
            break;
        }
        case VS_IN_CHATEVENT: ui_chat_add(&app->ui, m->from, m->text, 0); break;
        case VS_IN_ERROR: {
            char text[224];
            if (m->text.len > 0) {
                snprintf(text, sizeof(text), "%.*s", (int)m->text.len, m->text.data);
            } else if (str8_eq_cstr(m->code, "name_taken")) {
                snprintf(text, sizeof(text), "Ce pseudo est déjà pris dans la salle.");
            } else if (str8_eq_cstr(m->code, "bad_password")) {
                snprintf(text, sizeof(text), "Mot de passe du serveur incorrect.");
            } else if (str8_eq_cstr(m->code, "version_mismatch")) {
                snprintf(text, sizeof(text), "Version de protocole incompatible avec le serveur.");
            } else {
                snprintf(text, sizeof(text), "Erreur serveur : %.*s", (int)m->code.len, m->code.data);
            }
            if (proto_error_is_fatal(m->code)) {
                net_close(app->net);
                app->ws_open = 0;
                engine_disconnected(&app->engine);
                app->ui.screen = UI_SCREEN_CONNECT;
                app->ui.connecting = 0;
                ui_set_status(&app->ui, text, 1);
            } else {
                ui_toast(&app->ui, text, 1, now_ms());
            }
            break;
        }
        case VS_IN_UNKNOWN: break;
    }
    dispatch_output(app, &out);
    temp_end(t);
}

static void pump_net(App *app) {
    isize mark = arena_pos(app->scratch);
    NetSlot *slot = arena_push_struct(app->scratch, NetSlot);
    isize inner = arena_pos(app->scratch);
    while (net_poll(app->net, slot)) {
        switch (slot->kind) {
            case NET_EV_CONNECTED: {
                app->ws_open = 1;
                TempArena t = temp_begin(app->scratch);
                Str8 hello = proto_encode_hello(app->scratch, app->name, app->room, app->password,
                                                str8_from_cstr(app->session));
                b32 ok = net_send_text(app->net, hello);
                temp_end(t);
                if (!ok) {
                    net_close(app->net);
                    app->ws_open = 0;
                }
                break;
            }
            case NET_EV_MESSAGE: on_server_message(app, str8(slot->data, slot->len)); break;
            case NET_EV_CLOSED:
            case NET_EV_ERROR: {
                net_close(app->net);
                app->ws_open = 0;
                engine_session_lost(&app->engine);
                app->backoff_ns = engine_next_backoff(app->backoff_ns);
                app->next_attempt = vs_now_ns() + app->backoff_ns;
                if (app->ui.screen == UI_SCREEN_CONNECT) {
                    ui_set_status(&app->ui, "Serveur injoignable. Nouvelle tentative…", 1);
                } else {
                    ui_toast(&app->ui, "Connexion perdue, reconnexion en cours…", 1, now_ms());
                }
                break;
            }
            case NET_EV_NONE: break;
        }
        arena_pop_to(app->scratch, inner);
    }
    arena_pop_to(app->scratch, mark);
    refresh_view(app);
}

// --- actions de l'UI ---

static void do_connect(App *app) {
    Str8 server = str8_trim(ui_text_str(&app->ui.f_server));
    Str8 name = str8_trim(ui_text_str(&app->ui.f_name));
    Str8 room = str8_trim(ui_text_str(&app->ui.f_room));
    if (server.len == 0 || name.len == 0 || room.len == 0) {
        ui_set_status(&app->ui, "Serveur, pseudo et salle sont obligatoires.", 1);
        return;
    }
    NetUrl probe;
    if (!net_parse_url(server, &probe)) {
        ui_set_status(&app->ui, "Adresse invalide : attendu ws://hôte/ws ou wss://hôte/ws.", 1);
        return;
    }
    app->url = str8_copy(app->perm, server);
    app->name = str8_copy(app->perm, name);
    app->room = str8_copy(app->perm, room);
    app->password = str8_copy(app->perm, ui_text_str(&app->ui.f_password));
    settings_save(app);

    app->backoff_ns = 0;
    app->next_attempt = vs_now_ns();
    engine_connecting(&app->engine);
    app->ui.connecting = 1;
    ui_set_status(&app->ui, "Connexion au serveur…", 0);
    if (!net_connect(app->net, app->url)) {
        ui_set_status(&app->ui, "Connexion impossible.", 1);
        engine_disconnected(&app->engine);
        app->ui.connecting = 0;
    }
}

static void do_disconnect(App *app) {
    net_close(app->net);
    app->ws_open = 0;
    engine_disconnected(&app->engine);
    app->ui.screen = UI_SCREEN_CONNECT;
    app->ui.connecting = 0;
    app->ui.user_count = 0;
    app->ui.chat_count = 0;
    ui_set_status(&app->ui, "Déconnecté.", 0);
}

// open_file_dialog ouvre le sélecteur natif (IFileOpenDialog).
static b32 open_file_dialog(App *app, Str8 *out) {
    b32 got = 0;
    IFileOpenDialog *dlg = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog,
                                  (void **)&dlg);
    if (FAILED(hr) || !dlg) return 0;
    COMDLG_FILTERSPEC filters[] = {
        {L"Vidéos", L"*.mkv;*.mp4;*.avi;*.mov;*.m4v;*.webm;*.ts;*.mpg;*.mpeg;*.wmv;*.flv"},
        {L"Audio", L"*.mp3;*.flac;*.wav;*.m4a;*.ogg;*.opus"},
        {L"Tous les fichiers", L"*.*"},
    };
    dlg->lpVtbl->SetFileTypes(dlg, (UINT)VS_ARRAY_COUNT(filters), filters);
    dlg->lpVtbl->SetTitle(dlg, L"Choisir le média à regarder");
    if (SUCCEEDED(dlg->lpVtbl->Show(dlg, app->hwnd))) {
        IShellItem *item = NULL;
        if (SUCCEEDED(dlg->lpVtbl->GetResult(dlg, &item)) && item) {
            PWSTR wpath = NULL;
            if (SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &wpath)) && wpath) {
                *out = str8_copy(app->perm, utf16_to_utf8(app->scratch, (const u16 *)wpath));
                CoTaskMemFree(wpath);
                got = 1;
            }
            item->lpVtbl->Release(item);
        }
    }
    dlg->lpVtbl->Release(dlg);
    return got;
}

static void handle_actions(App *app) {
    UiApp *ui = &app->ui;
    VsOutput out;
    vs_output_reset(&out);
    i64 now = vs_now_ns();

    if (ui->act_connect) {
        ui->act_connect = 0;
        do_connect(app);
    }
    if (ui->act_disconnect) {
        ui->act_disconnect = 0;
        do_disconnect(app);
    }
    if (ui->act_ready) {
        ui->act_ready = 0;
        engine_set_ready(&app->engine, !app->engine.ready, &out);
    }
    if (ui->act_play) {
        ui->act_play = 0;
        engine_user_control(&app->engine, now, VS_ACT_PLAY, 0, 0, &out);
    }
    if (ui->act_pause) {
        ui->act_pause = 0;
        engine_user_control(&app->engine, now, VS_ACT_PAUSE, 0, 0, &out);
    }
    if (ui->act_seek) {
        ui->act_seek = 0;
        engine_user_control(&app->engine, now, VS_ACT_SEEK, ui->act_seek_pos, 1, &out);
    }
    if (ui->act_chat_send) {
        ui->act_chat_send = 0;
        Str8 text = str8_trim(ui_text_str(&ui->f_chat));
        // Pas d'écho local : le serveur rediffuse le message à tout le monde,
        // nous compris — l'afficher deux fois serait un doublon.
        if (text.len > 0) engine_chat(&app->engine, text, &out);
        ui_text_set(&ui->f_chat, str8_lit(""));
    }
    if (ui->act_open_file) {
        ui->act_open_file = 0;
        Str8 path;
        if (open_file_dialog(app, &path)) {
            ui_toast(ui, "Ouverture de VLC…", 0, now_ms());
            worker_open(&app->vlc, path);
        }
    }
    dispatch_output(app, &out);
    refresh_view(app);
}

// ------------------------------------------------------------ back-buffer ---

static void ensure_backbuffer(App *app, i32 w, i32 h) {
    if (app->mem_dc && app->bw == w && app->bh == h) return;
    if (app->bmp) DeleteObject(app->bmp);
    if (app->mem_dc) DeleteDC(app->mem_dc);
    HDC screen = GetDC(app->hwnd);
    app->mem_dc = CreateCompatibleDC(screen);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    app->bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &app->bits, NULL, 0);
    SelectObject(app->mem_dc, app->bmp);
    ReleaseDC(app->hwnd, screen);
    app->bw = w;
    app->bh = h;
}

static void paint(App *app) {
    RECT rc;
    GetClientRect(app->hwnd, &rc);
    i32 w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    ensure_backbuffer(app, w, h);

    ui_frame(&app->ui, app->mem_dc, w, h, now_ms());
    handle_actions(app);

    PAINTSTRUCT ps;
    HDC dc = BeginPaint(app->hwnd, &ps);
    BitBlt(dc, 0, 0, w, h, app->mem_dc, 0, 0, SRCCOPY);
    EndPaint(app->hwnd, &ps);

    if (!app->first_paint) {
        app->first_paint = 1;
        char msg[128];
        snprintf(msg, sizeof(msg), "vibesync: premier rendu en %lld ms\n",
                 (long long)((i64)GetTickCount64() - app->start_ticks));
        OutputDebugStringA(msg);
        vs_write_stderr(str8_from_cstr(msg));
    }
    set_timers(app);
}

// ------------------------------------------------------- procédure fenêtre ---

static b32 png_write(Arena *a, Str8 path, const u8 *bgra, i32 w, i32 h);

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App *app = g_app;
    if (!app) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_DESTROY: PostQuitMessage(0); return 0;
        case WM_CLOSE:
            settings_save(app);
            DestroyWindow(hwnd);
            return 0;
        case WM_ERASEBKGND: return 1;  // tout est repeint sur le back-buffer
        case WM_PAINT: paint(app); return 0;
        case WM_SIZE: redraw(app); return 0;
        case WM_GETMINMAXINFO: {
            MINMAXINFO *mmi = (MINMAXINFO *)lp;
            i32 dpi = app->ui.dpi > 0 ? app->ui.dpi : 96;
            mmi->ptMinTrackSize.x = MulDiv(900, dpi, 96);
            mmi->ptMinTrackSize.y = MulDiv(580, dpi, 96);
            return 0;
        }
        case WM_DPICHANGED: {
            ui_set_dpi(&app->ui, (i32)LOWORD(wp));
            RECT *r = (RECT *)lp;
            SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            redraw(app);
            return 0;
        }
        case WM_MOUSEMOVE:
            ui_on_mouse_move(&app->ui, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            redraw(app);
            return 0;
        case WM_LBUTTONDOWN:
            SetCapture(hwnd);
            ui_on_mouse_down(&app->ui, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            redraw(app);
            return 0;
        case WM_LBUTTONUP:
            ReleaseCapture();
            ui_on_mouse_up(&app->ui, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            redraw(app);
            return 0;
        case WM_MOUSEWHEEL:
            ui_on_wheel(&app->ui, GET_WHEEL_DELTA_WPARAM(wp));
            redraw(app);
            return 0;
        case WM_CHAR:
            ui_on_char(&app->ui, (u32)wp);
            redraw(app);
            return 0;
        case WM_KEYDOWN:
            ui_on_key(&app->ui, (u32)wp, (GetKeyState(VK_CONTROL) & 0x8000) != 0);
            redraw(app);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) return 1;  // le curseur est géré dans la frame
            break;
        case WM_TIMER:
            if (wp == TIMER_SHOT) {
                KillTimer(hwnd, TIMER_SHOT);
                png_write(app->scratch, app->shot_path, (const u8 *)app->bits, app->bw, app->bh);
                PostQuitMessage(0);
                return 0;
            }
            if (wp == TIMER_ENGINE) engine_step(app);
            redraw(app);
            return 0;
        case WM_APP_NET:
            pump_net(app);
            set_timers(app);
            redraw(app);
            return 0;
        case WM_APP_VLC:
            engine_step(app);
            redraw(app);
            return 0;
        case WM_APP_VLC_OPEN: {
            b32 ok, failed;
            char err[192];
            StrBuf name;
            i64 size;
            AcquireSRWLockExclusive(&app->vlc.lock);
            ok = app->vlc.open_ok;
            failed = app->vlc.open_failed;
            memcpy(err, app->vlc.open_error, sizeof(err));
            name = app->vlc.open_name;
            size = app->vlc.open_size;
            app->vlc.open_ok = 0;
            app->vlc.open_failed = 0;
            ReleaseSRWLockExclusive(&app->vlc.lock);
            if (ok) {
                VsOutput out;
                vs_output_reset(&out);
                engine_open_file(&app->engine, strbuf_str(&name), size, &out);
                dispatch_output(app, &out);
                ui_toast(&app->ui, "Média chargé, en pause au début.", 0, now_ms());
            } else if (failed) {
                ui_toast(&app->ui, err, 2, now_ms());
            }
            set_timers(app);
            redraw(app);
            return 0;
        }
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ------------------------------------------------------------ capture PNG ---
//
// Écriture PNG maison (blocs deflate « stored ») : des captures d'écran pour
// la documentation sans embarquer la moindre bibliothèque.

static u32 crc32_of(const u8 *data, isize len) {
    static u32 table[256];
    static b32 ready = 0;
    if (!ready) {
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = 1;
    }
    u32 crc = 0xffffffffu;
    for (isize i = 0; i < len; i++) crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

static void put_be32(Builder *b, u32 v) {
    u8 t[4] = {(u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v};
    builder_bytes(b, t, 4);
}

static void png_chunk(Builder *out, const char *type, const u8 *data, isize len) {
    put_be32(out, (u32)len);
    isize start = out->len;
    builder_bytes(out, type, 4);
    if (len > 0) builder_bytes(out, data, len);
    put_be32(out, crc32_of(out->data + start, out->len - start));
}

// --- deflate minimal : Huffman fixe + RLE (distance 1) ---
//
// Suffisant pour des captures d'interface : combiné au filtre PNG « Up », les
// aplats deviennent de longues suites d'octets identiques que la RLE écrase.
// Une centaine de lignes contre une dépendance à zlib : le choix est vite fait.

typedef struct {
    Builder *b;
    u32 acc;
    int nbits;
} BitW;

static void bw_bits(BitW *w, u32 value, int n) {  // bits dans l'ordre LSB
    w->acc |= (value & ((1u << n) - 1u)) << w->nbits;
    w->nbits += n;
    while (w->nbits >= 8) {
        builder_byte(w->b, (u8)(w->acc & 0xff));
        w->acc >>= 8;
        w->nbits -= 8;
    }
}

static void bw_huff(BitW *w, u32 code, int n) {  // codes Huffman : MSB d'abord
    for (int i = n - 1; i >= 0; i--) bw_bits(w, (code >> i) & 1u, 1);
}

static void bw_flush(BitW *w) {
    if (w->nbits > 0) {
        builder_byte(w->b, (u8)(w->acc & 0xff));
        w->acc = 0;
        w->nbits = 0;
    }
}

// emit_symbol écrit un symbole littéral/longueur avec le code Huffman fixe.
static void emit_symbol(BitW *w, u32 sym) {
    if (sym <= 143) bw_huff(w, 0x30 + sym, 8);
    else if (sym <= 255) bw_huff(w, 0x190 + sym - 144, 9);
    else if (sym <= 279) bw_huff(w, sym - 256, 7);
    else bw_huff(w, 0xc0 + sym - 280, 8);
}

static void deflate_rle(Builder *out, const u8 *data, isize len) {
    static const u16 len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10,  11,  13,  15,  17,  19, 23, 27,
                                     31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const u8 len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                     2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    BitW w = {out, 0, 0};
    bw_bits(&w, 1, 1);  // BFINAL
    bw_bits(&w, 1, 2);  // BTYPE = Huffman fixe
    isize i = 0;
    while (i < len) {
        // Longueur de la répétition de l'octet précédent (distance 1).
        isize run = 0;
        if (i > 0) {
            while (i + run < len && run < 258 && data[i + run] == data[i - 1]) run++;
        }
        if (run >= 3) {
            isize idx = 0;
            while (idx < 28 && len_base[idx + 1] <= run) idx++;
            // La longueur codée est len_base[idx] + bits supplémentaires : on
            // consomme donc bien `run` octets, pas seulement la base.
            emit_symbol(&w, (u32)(257 + idx));
            if (len_extra[idx] > 0) bw_bits(&w, (u32)(run - len_base[idx]), len_extra[idx]);
            bw_huff(&w, 0, 5);  // distance 1
            i += run;
        } else {
            emit_symbol(&w, data[i]);
            i++;
        }
    }
    emit_symbol(&w, 256);  // fin de bloc
    bw_flush(&w);
}

static b32 png_write(Arena *a, Str8 path, const u8 *bgra, i32 w, i32 h) {
    TempArena t = temp_begin(a);
    // 1. Données brutes : filtre « Up » (2) + RGB, par ligne. Le filtre annule
    //    les aplats verticaux, la RLE fait le reste.
    isize stride = (isize)w * 3;
    isize raw_len = (isize)h * (1 + stride);
    u8 *raw = arena_push_array(a, u8, raw_len);
    u8 *prev = arena_push_array(a, u8, stride);
    u8 *cur = arena_push_array(a, u8, stride);
    isize o = 0;
    for (i32 y = 0; y < h; y++) {
        const u8 *row = bgra + (isize)y * w * 4;
        for (i32 x = 0; x < w; x++) {
            cur[x * 3 + 0] = row[x * 4 + 2];
            cur[x * 3 + 1] = row[x * 4 + 1];
            cur[x * 3 + 2] = row[x * 4 + 0];
        }
        raw[o++] = 2;  // filtre Up
        for (isize k = 0; k < stride; k++) raw[o++] = (u8)(cur[k] - prev[k]);
        memcpy(prev, cur, (size_t)stride);
    }
    // 2. Flux zlib : en-tête, deflate, adler32 (sur les données non filtrées
    //    par zlib, c'est-à-dire le flux `raw` tel quel).
    Builder z;
    builder_init(&z, a, raw_len / 2 + 1024);
    builder_byte(&z, 0x78);
    builder_byte(&z, 0x01);
    deflate_rle(&z, raw, raw_len);
    u32 s1 = 1, s2 = 0;
    for (isize i = 0; i < raw_len; i++) {
        s1 = (s1 + raw[i]) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    put_be32(&z, (s2 << 16) | s1);

    // 3. Assemblage PNG.
    Builder out;
    builder_init(&out, a, z.len + 256);
    static const u8 sig[8] = {137, 'P', 'N', 'G', '\r', '\n', 26, '\n'};
    builder_bytes(&out, sig, 8);
    u8 ihdr[13];
    ihdr[0] = (u8)(w >> 24);
    ihdr[1] = (u8)(w >> 16);
    ihdr[2] = (u8)(w >> 8);
    ihdr[3] = (u8)w;
    ihdr[4] = (u8)(h >> 24);
    ihdr[5] = (u8)(h >> 16);
    ihdr[6] = (u8)(h >> 8);
    ihdr[7] = (u8)h;
    ihdr[8] = 8;  // 8 bits par canal
    ihdr[9] = 2;  // RGB
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    png_chunk(&out, "IHDR", ihdr, 13);
    png_chunk(&out, "IDAT", z.data, z.len);
    png_chunk(&out, "IEND", NULL, 0);

    u16 *wp = utf8_to_utf16(a, path, NULL);
    HANDLE f = CreateFileW((LPCWSTR)wp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    b32 ok = 0;
    if (f != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ok = WriteFile(f, out.data, (DWORD)out.len, &written, NULL) && written == (DWORD)out.len;
        CloseHandle(f);
    }
    temp_end(t);
    return ok;
}

// capture_screens rend les deux écrans avec un état de démonstration et les
// enregistre en PNG (option --capture <dossier>).
static void capture_screens(App *app, Str8 dir) {
    i32 w = MulDiv(1000, app->ui.dpi, 96), h = MulDiv(660, app->ui.dpi, 96);
    ensure_backbuffer(app, w, h);

    ui_set_status(&app->ui, "Serveur injoignable. Nouvelle tentative…", 1);
    ui_text_set(&app->ui.f_server, str8_lit("wss://vibesync.thibault.fr/ws"));
    ui_text_set(&app->ui.f_name, str8_lit("thibault"));
    ui_text_set(&app->ui.f_room, str8_lit("soirée-film"));
    ui_text_set(&app->ui.f_password, str8_lit("secret"));
    app->ui.screen = UI_SCREEN_CONNECT;
    ui_frame(&app->ui, app->mem_dc, w, h, now_ms());
    png_write(app->scratch, str8_cat(app->scratch, dir, str8_lit("\\ui-connexion.png")),
              (const u8 *)app->bits, w, h);

    UiApp *ui = &app->ui;
    ui->screen = UI_SCREEN_ROOM;
    ui->phase = VS_PHASE_CONNECTED;
    snprintf(ui->room, sizeof(ui->room), "soirée-film");
    ui->user_count = 3;
    snprintf(ui->users[0].name, sizeof(ui->users[0].name), "thibault");
    ui->users[0].is_self = 1;
    ui->users[0].ready = 1;
    ui->users[0].has_file = 1;
    ui->users[0].latency_ms = 12;
    snprintf(ui->users[0].file, sizeof(ui->users[0].file), "ep1-vostfr.mkv");
    snprintf(ui->users[1].name, sizeof(ui->users[1].name), "camille");
    ui->users[1].ready = 1;
    ui->users[1].has_file = 1;
    ui->users[1].latency_ms = 38;
    snprintf(ui->users[1].file, sizeof(ui->users[1].file), "ep1-vostfr.mkv");
    snprintf(ui->users[2].name, sizeof(ui->users[2].name), "jean-mi");
    ui->users[2].latency_ms = 120;
    ui->ready = 1;
    ui->paused = 0;
    ui->vlc_running = 1;
    ui->position_sec = 1287;
    ui->duration_sec = 2712;
    ui->drift_sec = 0.04;
    ui->latency_ms = 12;
    snprintf(ui->file_name, sizeof(ui->file_name), "ep1-vostfr.mkv");
    ui_chat_add(ui, str8_lit(""), str8_lit("Connecté à la salle."), 1);
    ui_chat_add(ui, str8_lit("camille"), str8_lit("prête quand vous voulez !"), 0);
    ui_chat_add(ui, str8_lit("moi"), str8_lit("je lance dans 10 secondes"), 0);
    ui_chat_add(ui, str8_lit("jean-mi"), str8_lit("attendez, je cherche mon fichier…"), 0);
    ui_toast(ui, "camille a rejoint la salle", 0, now_ms());
    ui_frame(ui, app->mem_dc, w, h, now_ms());
    png_write(app->scratch, str8_cat(app->scratch, dir, str8_lit("\\ui-salle.png")), (const u8 *)app->bits, w,
              h);
}

// ------------------------------------------------------------------- main ---

// cmd_opt lit « --nom valeur » dans la ligne de commande brute (valeur entre
// guillemets acceptée). Suffisant pour les options de diagnostic.
static b32 cmd_opt(Str8 cl, const char *name, Str8 *out) {
    Str8 key = str8_from_cstr(name);
    for (isize i = 0; i + key.len + 1 < cl.len; i++) {
        if (memcmp(cl.data + i, key.data, (size_t)key.len) != 0) continue;
        if (cl.data[i + key.len] != ' ') continue;
        isize j = i + key.len + 1;
        while (j < cl.len && cl.data[j] == ' ') j++;
        if (j >= cl.len) return 0;
        if (cl.data[j] == '"') {
            isize end = str8_find_char(cl, '"', j + 1);
            if (end < 0) return 0;
            *out = str8_sub(cl, j + 1, end - j - 1);
        } else {
            isize end = str8_find_char(cl, ' ', j);
            if (end < 0) end = cl.len;
            *out = str8_sub(cl, j, end - j);
        }
        return out->len > 0;
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show);

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    VS_UNUSED(prev);
    VS_UNUSED(cmdline);
    i64 start = (i64)GetTickCount64();

    // DPI : Per-Monitor v2. Le manifeste le déclare aussi ; cet appel garantit
    // le bon comportement même si le binaire est lancé sans ses ressources.
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        typedef BOOL(WINAPI * SetCtxFn)(HANDLE);
        SetCtxFn set_ctx = (SetCtxFn)(void *)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (set_ctx) set_ctx((HANDLE)-4);  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    Arena *perm = arena_create(VS_MB(8));
    Arena *scratch = arena_create(VS_MB(8));
    if (!perm || !scratch) return 2;
    App *app = arena_push_struct(perm, App);
    g_app = app;
    app->perm = perm;
    app->scratch = scratch;
    app->start_ticks = start;
    engine_init(&app->engine);
    ui_init(&app->ui);
    proto_session_token(app->session, (isize)sizeof(app->session));
    settings_load(app);

    app->net = arena_push_struct(perm, Net);
    if (!net_init(app->net)) return 2;

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"vibesync_window";
    RegisterClassExW(&wc);

    i32 dpi = 96;
    {
        HDC screen = GetDC(NULL);
        if (screen) {
            dpi = GetDeviceCaps(screen, LOGPIXELSX);
            ReleaseDC(NULL, screen);
        }
    }
    ui_set_dpi(&app->ui, dpi);

    i32 win_w = MulDiv(1000, dpi, 96), win_h = MulDiv(660, dpi, 96);
    RECT wr = {0, 0, win_w, win_h};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"vibesync", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top, NULL, NULL, inst, NULL);
    if (!hwnd) return 2;
    app->hwnd = hwnd;
    app->ui.hwnd = hwnd;

    // Barre de titre sombre (Windows 10 20H1+), ignorée silencieusement avant.
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm) {
        typedef HRESULT(WINAPI * SetAttrFn)(HWND, DWORD, LPCVOID, DWORD);
        SetAttrFn set_attr = (SetAttrFn)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute");
        if (set_attr) {
            BOOL dark = TRUE;
            set_attr(hwnd, 20, &dark, sizeof(dark));
        }
        FreeLibrary(dwm);
    }

    net_set_notify(app->net, hwnd, WM_APP_NET);
    worker_start(&app->vlc, hwnd);

    Str8 cl = utf16_to_utf8(scratch, (const u16 *)GetCommandLineW());

    // Mode capture : rendu des deux écrans en PNG, puis sortie.
    {
        Str8 dir;
        if (cmd_opt(cl, "--capture", &dir)) {
            capture_screens(app, str8_copy(scratch, dir));
            char msg[128];
            snprintf(msg, sizeof(msg), "captures ecrites en %lld ms\n",
                     (long long)((i64)GetTickCount64() - start));
            vs_write_stderr(str8_from_cstr(msg));
            worker_stop(&app->vlc);
            net_destroy(app->net);
            return 0;
        }
    }

    // Mode « smoke » : connexion automatique puis capture de l'écran réel.
    {
        Str8 v;
        if (cmd_opt(cl, "--server", &v)) ui_text_set(&app->ui.f_server, v);
        if (cmd_opt(cl, "--nom", &v)) ui_text_set(&app->ui.f_name, v);
        if (cmd_opt(cl, "--room", &v)) ui_text_set(&app->ui.f_room, v);
        b32 with_file = 0;
        if (cmd_opt(cl, "--fichier", &v)) {
            worker_open(&app->vlc, str8_copy(perm, v));
            with_file = 1;
        }
        if (cmd_opt(cl, "--chat", &v)) app->auto_chat = str8_copy(perm, v);
        if (cmd_opt(cl, "--auto", &v)) do_connect(app);  // « --auto 1 » : connexion immédiate
        if (cmd_opt(cl, "--smoke", &v)) {
            app->shot_path = str8_copy(perm, v);
            do_connect(app);
            // Laisser à VLC le temps de démarrer quand un média est demandé.
            SetTimer(hwnd, TIMER_SHOT, with_file ? 12000 : 4000, NULL);
        }
    }

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    settings_save(app);
    worker_stop(&app->vlc);
    net_destroy(app->net);
    ui_release(&app->ui);
    CoUninitialize();
    return 0;
}
