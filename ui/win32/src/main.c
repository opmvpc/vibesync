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

#include "auto.h"
#include "base.h"
#include "conn.h"
#include "engine.h"
#include "health.h"
#include "ini.h"
#include "secret.h"
#include "json.h"
#include "net.h"
#include "protocol.h"
#include "ui.h"
#include "vlc.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
// commdlg.h : GetOpenFileNameW, la boîte « Ouvrir » du système. WIN32_LEAN_AND_MEAN
// la retire de windows.h, il faut donc l'inclure explicitement.
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <stdio.h>
#include <string.h>

#define WM_APP_NET (WM_APP + 1)
#define WM_APP_VLC (WM_APP + 2)
#define WM_APP_VLC_OPEN (WM_APP + 3)
#define WM_APP_HEALTH (WM_APP + 4)

#define TIMER_ENGINE 1
#define TIMER_UI 2
#define TIMER_SHOT 3
#define TIMER_AUTO 4

// Message d'échec d'ouverture : cause + piste actionnable (VS-029). Il est
// affiché en toast (tronqué à sa capacité) et journalisé en entier.
#define VLC_OPEN_ERROR_CAP 256

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
    char open_error[VLC_OPEN_ERROR_CAP];
    StrBuf open_name;
    i64 open_size;

    // Recherche du fichier d'un participant (VS-026). Bloquante — c'est tout
    // l'intérêt de la faire ici : la fenêtre ne gèle pas pendant qu'on
    // parcourt une arborescence, éventuellement réseau.
    b32 find_pending;
    StrBuf find_name;
    StrBuf find_dirs[MEDIA_MAX_DIRS];
    isize find_dir_count;
    b32 find_done;     // une recherche vient de se terminer
    b32 find_found;
    MediaFind find_result;
} VlcWorker;

static DWORD WINAPI vlc_thread(LPVOID param) {
    VlcWorker *w = (VlcWorker *)param;
    Arena *scratch = arena_create(VS_MB(4));
    if (!scratch) return 1;
    while (!InterlockedCompareExchange(&w->stop, 0, 0)) {
        WaitForSingleObject(w->wake, 200);
        if (InterlockedCompareExchange(&w->stop, 0, 0)) break;

        // 0. Recherche du fichier d'un participant dans les dossiers médias.
        //    Trouvé, elle enchaîne directement sur l'ouverture ci-dessous.
        StrBuf path;
        b32 want_open = 0;
        AcquireSRWLockExclusive(&w->lock);
        b32 want_find = w->find_pending;
        StrBuf find_name = w->find_name;
        StrBuf find_dirs[MEDIA_MAX_DIRS];
        isize find_dir_count = w->find_dir_count;
        if (want_find) {
            memcpy(find_dirs, w->find_dirs, sizeof(find_dirs));
            w->find_pending = 0;
        }
        ReleaseSRWLockExclusive(&w->lock);
        if (want_find) {
            MediaFind found;
            TempArena t = temp_begin(scratch);
            b32 ok = media_find(scratch, find_dirs, find_dir_count, strbuf_str(&find_name), &found);
            temp_end(t);
            AcquireSRWLockExclusive(&w->lock);
            w->find_done = 1;
            w->find_found = ok;
            w->find_result = found;
            ReleaseSRWLockExclusive(&w->lock);
            if (ok) {
                path = found.path;
                want_open = 1;
            } else {
                PostMessageW(w->hwnd, WM_APP_VLC_OPEN, 0, 0);
            }
        }

        // 1. Ouverture d'un fichier (bloquant : jusqu'à 20 s).
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
            } else {
                vs_log("vlc: exécutable introuvable (réglage VIBESYNC_VLC et emplacements standards)");
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
                // Cause ET piste : « interface HTTP de VLC muette » tout seul
                // n'apprend rien à l'utilisateur (VS-029).
                const char *hint = vlc_error_hint(err);
                snprintf(w->open_error, sizeof(w->open_error), "%s%s%s", vlc_error_text(err),
                         hint[0] ? " — " : "", hint);
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

// worker_find demande la recherche d'un nom dans les dossiers médias, puis
// l'ouverture si elle aboutit. Tout se passe sur le thread VLC.
static void worker_find(VlcWorker *w, Str8 name, const StrBuf *dirs, isize dir_count) {
    AcquireSRWLockExclusive(&w->lock);
    strbuf_set(&w->find_name, name);
    w->find_dir_count = VS_MIN(dir_count, (isize)MEDIA_MAX_DIRS);
    for (isize i = 0; i < w->find_dir_count; i++) w->find_dirs[i] = dirs[i];
    w->find_pending = 1;
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

// --------------------------------------------------- travailleur healthz ---
//
// La sonde bloque (DNS, TCP, TLS) : elle vit sur son propre thread, comme le
// pilotage de VLC. Le thread UI ne fait que poser une demande et lire un
// résultat sous verrou.

typedef struct {
    HWND hwnd;
    HANDLE thread;
    HANDLE wake;
    volatile long stop;
    SRWLOCK lock;

    b32 pending;      // une demande attend (sous lock)
    StrBuf req_host;
    int req_port;
    b32 req_secure;
    i64 req_gen;      // génération : un résultat périmé est ignoré

    b32 has_result;
    HealthResult result;
    i64 result_gen;
} HealthWorker;

static DWORD WINAPI health_thread(LPVOID param) {
    HealthWorker *w = (HealthWorker *)param;
    Arena *scratch = arena_create(VS_MB(1));
    if (!scratch) return 1;
    while (!InterlockedCompareExchange(&w->stop, 0, 0)) {
        WaitForSingleObject(w->wake, 500);
        if (InterlockedCompareExchange(&w->stop, 0, 0)) break;
        StrBuf host;
        int port = 0;
        b32 secure = 0, want = 0;
        i64 gen = 0;
        AcquireSRWLockExclusive(&w->lock);
        if (w->pending) {
            host = w->req_host;
            port = w->req_port;
            secure = w->req_secure;
            gen = w->req_gen;
            w->pending = 0;
            want = 1;
        }
        ReleaseSRWLockExclusive(&w->lock);
        if (!want) continue;

        HealthResult r;
        TempArena t = temp_begin(scratch);
        health_probe(scratch, strbuf_str(&host), port, secure, 4000, &r);
        temp_end(t);

        AcquireSRWLockExclusive(&w->lock);
        w->result = r;
        w->result_gen = gen;
        w->has_result = 1;
        ReleaseSRWLockExclusive(&w->lock);
        PostMessageW(w->hwnd, WM_APP_HEALTH, 0, 0);
    }
    arena_destroy(scratch);
    return 0;
}

static void health_start(HealthWorker *w, HWND hwnd) {
    memset(w, 0, sizeof(*w));
    InitializeSRWLock(&w->lock);
    w->hwnd = hwnd;
    w->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    w->thread = CreateThread(NULL, 0, health_thread, w, 0, NULL);
}

static void health_stop(HealthWorker *w) {
    InterlockedExchange(&w->stop, 1);
    if (w->wake) SetEvent(w->wake);
    if (w->thread) {
        WaitForSingleObject(w->thread, 15000);
        CloseHandle(w->thread);
        w->thread = NULL;
    }
    if (w->wake) {
        CloseHandle(w->wake);
        w->wake = NULL;
    }
}

// health_request remplace toute demande en attente : seule la dernière adresse
// tapée compte.
static i64 health_request(HealthWorker *w, Str8 host, int port, b32 secure) {
    AcquireSRWLockExclusive(&w->lock);
    strbuf_set(&w->req_host, host);
    w->req_port = port;
    w->req_secure = secure;
    w->req_gen++;
    w->pending = 1;
    i64 gen = w->req_gen;
    ReleaseSRWLockExclusive(&w->lock);
    SetEvent(w->wake);
    return gen;
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
    // Un échec d'écriture de vibesync.ini est journalisé À CHAQUE fois (c'est le
    // seul témoin exploitable à distance) mais n'est signalé à l'écran qu'UNE
    // fois par session : ini_flush part à chaque geste (connexion, dossier
    // ajouté, case cochée, fermeture) et un disque plein les ferait tous
    // échouer — l'utilisateur verrait le même toast en boucle.
    b32 ini_write_toasted;

    HWND hwnd;
    HDC mem_dc;
    HBITMAP bmp;
    void *bits;
    i32 bw, bh;

    // Jeton de reprise de session, persisté dans l'ini (VS-028). Dimensionné
    // sur la borne de RELECTURE, pas sur les 32 caractères qu'on génère : le
    // fichier est éditable et peut venir d'une autre version.
    char session[VS_SESSION_TOKEN_MAX + 1];
    Str8 url, name, room;
    // Le mot de passe vit dans un tampon fixe, pas dans l'arène : on peut
    // l'effacer (SecureZeroMemory) sans laisser de copie derrière soi.
    StrBuf password;
    b32 ws_open;
    Conn conn;  // politique de reconnexion (conn.c) : source unique de vérité
    HealthWorker health;
    i64 health_gen;      // génération attendue
    StrBuf download_url;  // welcome.downloadUrl
    b32 engine_timer_on;
    b32 ui_timer_on;
    i64 start_ticks;
    b32 first_paint;
    // Mode « smoke » : capture de l'écran réel après connexion, puis sortie.
    Str8 shot_path;
    Str8 auto_chat;  // message envoyé dès le welcome (diagnostic)
    StrBuf suggested_url;  // adresse proposée par le bouton « Utiliser »
    StrBuf media_dirs[MEDIA_MAX_DIRS];
    isize media_dir_count;
    StrBuf pending_find;  // nom recherché, pour le message d'échec

    // Pilote du harnais de test réel (VS-029) : inactif hors mode auto.
    AutoPilot autop;
    // Commandes envoyées à VLC depuis le lancement, par nature (index =
    // VsCmdKind). Publiées dans l'état du mode auto : `rateCmds` prouve sur une
    // vraie séance que la vitesse ne corrige plus jamais la dérive (VS-038).
    i64 cmd_counts[4];
    // Pauses automatiques annoncées par le serveur depuis le lancement : face
    // visible du symptôme de VS-039 (« Pause auto : X a N s de retard » après un
    // changement de fichier). Publié dans l'état du mode auto.
    i64 auto_pause_toasts;

    // Réglages : chemin VLC détecté au démarrage, AVANT que %VIBESYNC_VLC% ne
    // soit forcé par le réglage — sinon la « détection automatique » affichée
    // ne ferait que renvoyer le réglage à l'utilisateur.
    Str8 vlc_auto;
    // Valeur HÉRITÉE de %VIBESYNC_VLC% : celle que l'utilisateur (ou un
    // harnais) a posée dans l'environnement avant de lancer l'exe. Elle sert
    // de repli quand le réglage de l'ini est vide, cf. apply_vlc_path.
    Str8 vlc_env;
    StrBuf vlc_checked;  // dernier chemin validé (évite un accès disque par frame)
} App;

static App *g_app;

static i64 now_ms(void) { return (i64)GetTickCount64(); }

// --- réglages ---

// file_exists : un chemin doit désigner un fichier, pas un dossier.
static b32 file_exists(Arena *scratch, Str8 path) {
    if (path.len == 0) return 0;
    TempArena t = temp_begin(scratch);
    u16 *w = utf8_to_utf16(scratch, path, NULL);
    DWORD attr = GetFileAttributesW((LPCWSTR)w);
    temp_end(t);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// dir_exists : pendant de file_exists pour les répertoires (sélecteur VLC).
static b32 dir_exists(Arena *scratch, Str8 path) {
    if (path.len == 0) return 0;
    TempArena t = temp_begin(scratch);
    u16 *w = utf8_to_utf16(scratch, path, NULL);
    DWORD attr = GetFileAttributesW((LPCWSTR)w);
    temp_end(t);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// browse_dir_exists : dir_exists derrière la signature attendue par
// vlc_browse_initial_dir (`ctx` = l'arène de travail).
static b32 browse_dir_exists(void *ctx, Str8 dir) { return dir_exists((Arena *)ctx, dir); }

// env_str lit une variable d'environnement en UTF-8 (vide si absente).
static Str8 env_str(Arena *a, const wchar_t *name) {
    DWORD n = GetEnvironmentVariableW(name, NULL, 0);
    if (n == 0) return str8_lit("");
    u16 *buf = arena_push_array(a, u16, (isize)n + 1);
    DWORD got = GetEnvironmentVariableW(name, (LPWSTR)buf, n);
    if (got == 0 || got > n) return str8_lit("");
    return utf16_to_utf8(a, buf);
}

// apply_vlc_path propage le réglage à vlc.c sans toucher à vlc.c : celui-ci
// consulte déjà %VIBESYNC_VLC% en premier dans vlc_locate().
//
// Réglage VIDE ne veut pas dire « efface la variable » mais « rends la
// valeur HÉRITÉE » : %VIBESYNC_VLC% est documenté comme la façon d'indiquer un
// VLC hors des emplacements standards (c'est même la piste affichée par
// vlc_error_text), et l'écraser au démarrage la rendait inopérante — un VLC
// installé ailleurs restait « introuvable » quoi qu'on mette dans
// l'environnement. Trouvé par le harnais VS-029 dans la VM Win11, où VLC vit
// dans %USERPROFILE%\tools\vlc.
static void apply_vlc_path(App *app, Str8 path) {
    TempArena t = temp_begin(app->scratch);
    if (path.len == 0) path = app->vlc_env;  // retour à l'héritage
    if (path.len > 0) {
        u16 *w = utf8_to_utf16(app->scratch, path, NULL);
        SetEnvironmentVariableW(L"VIBESYNC_VLC", (LPCWSTR)w);
    } else {
        SetEnvironmentVariableW(L"VIBESYNC_VLC", NULL);
    }
    temp_end(t);
}

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

    // Mot de passe : cochée par défaut, décochable ; le secret n'existe sur
    // disque que sous forme de blob DPAPI.
    v = ini_get(&app->ini, "retenir_mdp", str8_lit("1"));
    app->ui.remember_password = !(v.len == 1 && v.data[0] == '0');
    v = ini_get(&app->ini, "password_enc", str8_lit(""));
    if (app->ui.remember_password && v.len > 0) {
        TempArena t = temp_begin(app->scratch);
        Str8 plain;
        if (secret_unprotect(app->scratch, v, &plain)) {
            ui_text_set(&app->ui.f_password, plain);
            secret_wipe(plain.data, plain.len);  // le champ en a sa copie
        } else {
            // Blob d'un autre compte/machine ou corrompu : champ vide, sans
            // message. L'utilisateur retape, on réenregistrera au bon format.
            OutputDebugStringA("vibesync: password_enc indéchiffrable, ignoré\n");
        }
        temp_end(t);
    }

    // Dossiers médias : ceux de l'ini, sinon le dossier Téléchargements — le
    // plus probable pour des fichiers reçus d'un ami.
    v = ini_get(&app->ini, "dossiers_medias", str8_lit(""));
    if (v.len > 0) app->media_dir_count = media_dirs_split(v, app->media_dirs, MEDIA_MAX_DIRS);
    if (app->media_dir_count == 0) {
        TempArena td = temp_begin(app->scratch);
        Str8 dl;
        if (media_default_dir(app->scratch, &dl)) strbuf_set(&app->media_dirs[app->media_dir_count++], dl);
        temp_end(td);
    }

    // Détection automatique de VLC, mesurée avant d'appliquer le réglage.
    app->vlc_auto = str8_lit("");
    TempArena t = temp_begin(app->scratch);
    // Valeur héritée de l'environnement, lue AVANT toute écriture de notre fait.
    app->vlc_env = str8_copy(app->perm, env_str(app->scratch, L"VIBESYNC_VLC"));
    Str8 found;
    if (vlc_locate(app->scratch, &found)) app->vlc_auto = str8_copy(app->perm, found);
    temp_end(t);
    apply_vlc_path(app, ini_get(&app->ini, "vlc", str8_lit("")));
}

// ini_flush est le SEUL point d'écriture de vibesync.ini. Il applique les
// règles du secret avant chaque écriture, ce qui rend structurellement
// impossible qu'un chemin de code oublie de chiffrer ou laisse un clair.
static b32 ini_flush(App *app) {
    UiApp *ui = &app->ui;
    ini_set(app->perm, &app->ini, "retenir_mdp", ui->remember_password ? str8_lit("1") : str8_lit("0"));
    if (ui->remember_password && ui->f_password.len > 0) {
        Str8 hex;
        if (secret_protect(app->perm, ui_text_str(&ui->f_password), &hex)) {
            ini_set(app->perm, &app->ini, "password_enc", hex);
        } else {
            ini_remove(&app->ini, "password_enc");  // pas de chiffrement, pas d'écriture
        }
    } else {
        // Décochée, ou aucun mot de passe : l'entrée DISPARAÎT du fichier.
        ini_remove(&app->ini, "password_enc");
    }
    ini_set(app->perm, &app->ini, "dossiers_medias",
            media_dirs_join(app->perm, app->media_dirs, app->media_dir_count));
    // Filet : un « password= » en clair hérité d'une version antérieure ou
    // ajouté à la main est retiré à la première écriture.
    ini_remove(&app->ini, "password");

    TempArena t = temp_begin(app->scratch);
    Str8 text = ini_write(app->scratch, &app->ini);
    b32 ok = ini_save_file(app->scratch, ini_path(app->scratch), text);
    temp_end(t);
    return ok;
}

// ini_flush_notify écrit et REND VISIBLE l'échec. Un disque plein, une ACL
// héritée d'un profil bricolé ou un antivirus qui verrouille le fichier
// faisaient perdre réglages et jeton de session en silence : ni l'utilisateur
// ni le journal n'en savaient rien, et le symptôme remontait du terrain sous la
// forme « il me redemande tout à chaque lancement ». `what` nomme ce qui est
// perdu ; `toast` vaut 0 quand l'appelant affiche déjà le message lui-même
// (panneau Réglages) ou quand la fenêtre est en train de disparaître (WM_CLOSE,
// sortie) — un toast n'y serait jamais lu. Jamais bloquant : l'application
// continue avec ses réglages en mémoire.
static b32 ini_flush_notify(App *app, const char *what, b32 toast) {
    if (ini_flush(app)) return 1;
    vs_log("ini: %s non enregistré — vibesync.ini n'a pas pu être écrit "
           "(disque plein, droits d'accès ou fichier verrouillé ?)",
           what);
    if (toast && !app->ini_write_toasted) {
        app->ini_write_toasted = 1;
        ui_toast(&app->ui,
                 "Réglages non enregistrés : vibesync.ini n'a pas pu être écrit. "
                 "Détail dans %APPDATA%\\vibesync.log.",
                 1, now_ms());
    }
    return 0;
}

// settings_save réécrit le fichier en CONSERVANT les clés qu'on ne gère pas
// ici (chemin VLC en particulier) : app->ini est la référence, pas un ini neuf.
// `toast` est transmis tel quel à ini_flush_notify. Renvoie 0 si rien n'a pu
// être écrit — le retour est ignorable, mais l'échec ne l'est plus.
static b32 settings_save(App *app, b32 toast) {
    ini_set(app->perm, &app->ini, "serveur", ui_text_str(&app->ui.f_server));
    ini_set(app->perm, &app->ini, "pseudo", ui_text_str(&app->ui.f_name));
    ini_set(app->perm, &app->ini, "salle", ui_text_str(&app->ui.f_room));
    return ini_flush_notify(app, "réglages", toast);
}

// Clés que l'application gère. Tout le reste de vibesync.ini appartient à
// l'utilisateur ou à une version future : ini_flush le conserve tel quel. La
// liste ne sert qu'à savoir ce qu'on a le droit d'évincer si le fichier est
// saturé — voir session_load().
static const char *const g_ini_keys[] = {
    "serveur", "pseudo", "salle", "retenir_mdp", "password_enc", "dossiers_medias", "vlc", "session",
};

// session_load récupère le jeton de reprise de session persisté (VS-028).
//
// Il DOIT survivre au redémarrage de l'exe : un jeton neuf à chaque lancement,
// c'est `name_taken` tant que la connexion zombie n'a pas expiré côté serveur
// (timeout de lecture 60 s) — c'est très exactement le « on peut pas se
// reconnecter juste après avec le même pseudo » remonté du terrain. Avec un
// jeton stable, la reprise de session (docs/protocol.md, règle serveur 6)
// couvre aussi le cas « je ferme l'app et je la relance ».
//
// Un jeton absent, tronqué ou bricolé à la main est remplacé par un jeton neuf
// puis écrit : mieux vaut un jeton neuf qu'un jeton que le serveur refusera.
// Un échec d'écriture n'est PAS bloquant — on retombe simplement sur le
// comportement d'avant VS-028.
static void session_load(App *app) {
    Str8 stored = str8_trim(ini_get(&app->ini, "session", str8_lit("")));
    if (proto_session_token_valid(stored)) {
        memcpy(app->session, stored.data, (size_t)stored.len);
        app->session[stored.len] = 0;
        return;
    }
    if (!proto_session_token(app->session, (isize)sizeof(app->session))) {
        // Sans générateur, pas de jeton : le hello l'omettra (il est optionnel)
        // et on perd seulement la reprise de session.
        app->session[0] = 0;
        vs_log("session: génération du jeton impossible, reprise de session désactivée");
        return;
    }
    if (stored.len > 0) vs_log("session: jeton persisté invalide, remplacé");

    Str8 token = str8_from_cstr(app->session);
    if (!ini_set(app->perm, &app->ini, "session", token)) {
        // vibesync.ini saturé (INI_MAX_ENTRIES). Le fichier est éditable à la
        // main : ignorer ce retour rendrait l'échec SILENCIEUX — ini_flush
        // réussirait sans la clé et le pseudo resterait bloqué au redémarrage,
        // c'est-à-dire très exactement le bug que VS-028 corrige. On évince donc
        // une entrée qui ne nous appartient pas plutôt que d'abandonner.
        Str8 evicted = str8_lit("");
        if (ini_make_room(&app->ini, g_ini_keys, VS_ARRAY_COUNT(g_ini_keys), &evicted)) {
            vs_log("ini: fichier saturé, entrée inconnue évincée pour le jeton de session : %.*s",
                   (int)evicted.len, (const char *)evicted.data);
        }
        if (!ini_set(app->perm, &app->ini, "session", token)) {
            vs_log("session: vibesync.ini saturé et rien à évincer, jeton NON persisté — le pseudo pourra "
                   "être refusé pendant 60 s après un redémarrage");
            ui_toast(&app->ui,
                     "vibesync.ini est plein : le jeton de session n'a pas pu être enregistré. "
                     "Retirez-en des lignes inutiles.",
                     1, now_ms());
            return;
        }
    }
    ini_flush_notify(app, "jeton de session", 1);
}

// --- panneau Réglages ---

static void settings_open(App *app) {
    UiApp *ui = &app->ui;
    ui_text_set(&ui->f_set_server, ini_get(&app->ini, "serveur", ui_text_str(&ui->f_server)));
    ui_text_set(&ui->f_set_name, ini_get(&app->ini, "pseudo", ui_text_str(&ui->f_name)));
    ui_text_set(&ui->f_set_room, ini_get(&app->ini, "salle", ui_text_str(&ui->f_room)));
    ui_text_set(&ui->f_set_vlc, ini_get(&app->ini, "vlc", str8_lit("")));
    snprintf(ui->settings_auto_vlc, sizeof(ui->settings_auto_vlc), "%.*s", (int)app->vlc_auto.len,
             app->vlc_auto.data);
    ui->settings_msg[0] = 0;
    ui->settings_msg_error = 0;
    ui->settings_vlc_state = 0;
    strbuf_set(&app->vlc_checked, str8_lit("\x01"));  // force une revalidation
    ui->focus = 0;
    ui->settings_open = 1;
}

// settings_apply valide puis enregistre. Renvoie 0 si un réglage est refusé.
static b32 settings_apply(App *app) {
    UiApp *ui = &app->ui;
    Str8 vlc = str8_trim(ui_text_str(&ui->f_set_vlc));
    if (vlc.len > 0 && !file_exists(app->scratch, vlc)) {
        snprintf(ui->settings_msg, sizeof(ui->settings_msg), "Rien n'a été enregistré.");
        ui->settings_msg_error = 1;
        return 0;
    }
    ini_set(app->perm, &app->ini, "serveur", str8_trim(ui_text_str(&ui->f_set_server)));
    ini_set(app->perm, &app->ini, "pseudo", str8_trim(ui_text_str(&ui->f_set_name)));
    ini_set(app->perm, &app->ini, "salle", str8_trim(ui_text_str(&ui->f_set_room)));
    ini_set(app->perm, &app->ini, "vlc", vlc);
    // Même point d'écriture : mêmes règles de secret. Pas de toast — le panneau
    // affiche son propre message juste en dessous, à l'endroit où l'utilisateur
    // vient de cliquer.
    b32 ok = ini_flush_notify(app, "réglages", 0);
    if (!ok) {
        snprintf(ui->settings_msg, sizeof(ui->settings_msg), "vibesync.ini non modifiable.");
        ui->settings_msg_error = 1;
        return 0;
    }
    // Rechargement à chaud : le chemin VLC vaut pour le prochain lancement, les
    // valeurs par défaut repeuplent l'écran de connexion.
    apply_vlc_path(app, ini_get(&app->ini, "vlc", str8_lit("")));
    ui_text_set(&ui->f_server, ui_text_str(&ui->f_set_server));
    ui_text_set(&ui->f_name, ui_text_str(&ui->f_set_name));
    ui_text_set(&ui->f_room, ui_text_str(&ui->f_set_room));
    return 1;
}

// settings_validate rafraîchit le voyant du chemin VLC quand il a changé.
// Renvoie 1 si l'affichage doit être redessiné.
static b32 settings_validate(App *app) {
    UiApp *ui = &app->ui;
    if (!ui->settings_open) return 0;
    Str8 vlc = str8_trim(ui_text_str(&ui->f_set_vlc));
    if (strbuf_eq(&app->vlc_checked, vlc)) return 0;
    strbuf_set(&app->vlc_checked, vlc);
    i32 state = vlc.len == 0 ? 0 : (file_exists(app->scratch, vlc) ? 1 : 2);
    b32 changed = state != ui->settings_vlc_state;
    ui->settings_vlc_state = state;
    return changed;
}

// --- adresse du serveur et joignabilité ---

// to_wss réécrit une URL ws:// en wss:// (le port explicite est conservé).
static Str8 to_wss(Arena *a, Str8 url) {
    if (!str8_starts_with(url, str8_lit("ws://"))) return url;
    return str8_cat(a, str8_lit("wss://"), str8_sub(url, 5, -1));
}

// probe_health normalise l'adresse saisie puis lance un GET /healthz hors du
// thread UI. L'aperçu de l'adresse réellement utilisée est publié au passage.
static void probe_health(App *app) {
    UiApp *ui = &app->ui;
    TempArena t = temp_begin(app->scratch);
    Str8 raw = str8_trim(ui_text_str(&ui->f_server));
    Str8 norm;
    const char *err = NULL;
    if (!conn_normalize_url(app->scratch, raw, &norm, &err)) {
        ui->health = UI_HEALTH_FAIL;
        snprintf(ui->health_msg, sizeof(ui->health_msg), "%s", err ? err : "adresse invalide");
        ui->server_hint[0] = 0;
        ui->health_tls_hint = 0;
        temp_end(t);
        return;
    }
    NetUrl u;
    if (!net_parse_url(norm, &u)) {
        ui->health = UI_HEALTH_FAIL;
        snprintf(ui->health_msg, sizeof(ui->health_msg), "adresse illisible");
        ui->server_hint[0] = 0;
        temp_end(t);
        return;
    }
    // Aperçu : l'utilisateur voit ce qui sera réellement contacté.
    ui->health_tls_hint = 0;
    if (!str8_eq(raw, norm)) {
        snprintf(ui->server_hint, sizeof(ui->server_hint), "Adresse utilisée : %.*s", (int)norm.len,
                 norm.data);
        strbuf_set(&app->suggested_url, norm);
    } else {
        ui->server_hint[0] = 0;
    }
    ui->health = UI_HEALTH_TESTING;
    ui->health_msg[0] = 0;
    app->health_gen = health_request(&app->health, str8_from_cstr(u.host), u.port, u.secure);
    temp_end(t);
}

static void apply_health(App *app, const HealthResult *r) {
    UiApp *ui = &app->ui;
    ui->health_latency_ms = r->latency_ms;
    if (r->kind == HEALTH_OK) {
        ui->health = UI_HEALTH_OK;
        ui->health_msg[0] = 0;
    } else {
        ui->health = UI_HEALTH_FAIL;
        snprintf(ui->health_msg, sizeof(ui->health_msg), "%s", health_text(r));
    }
    // Le serveur ne répond qu'en TLS : c'est le piège ws:// vu sur le terrain.
    if (r->tls_available) {
        TempArena t = temp_begin(app->scratch);
        Str8 norm;
        if (conn_normalize_url(app->scratch, str8_trim(ui_text_str(&ui->f_server)), &norm, NULL)) {
            strbuf_set(&app->suggested_url, to_wss(app->scratch, norm));
        }
        temp_end(t);
        ui->health_tls_hint = 1;
        snprintf(ui->server_hint, sizeof(ui->server_hint),
                 "Le serveur répond en chiffré : passez en wss://");
    }
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
    for (isize i = 0; i < out->cmd_count; i++) {
        isize k = (isize)out->cmds[i].kind;
        if (k >= 0 && k < VS_ARRAY_COUNT(app->cmd_counts)) app->cmd_counts[k]++;
    }
    worker_push_cmds(&app->vlc, out->cmds, out->cmd_count);
    out->cmd_count = 0;
}

// --- vue : recopie de l'état moteur vers l'UI (thread UI uniquement) ---

// refresh_user_files marque les lignes de la liste dont le fichier est DÉJÀ le
// nôtre (VS-040) : ce sont celles que le double-clic doit ignorer, et que le
// survol ne doit pas allumer. Notre fichier se lit dans le moteur et non dans
// le miroir de la vue, qui aurait un broadcast de retard (leçon de VS-039).
static void refresh_user_files(App *app) {
    UiApp *ui = &app->ui;
    const VsDirOps *ops = vs_dir_ops();
    Str8 mine = app->engine.have_file ? strbuf_str(&app->engine.file_name) : str8_lit("");
    for (isize i = 0; i < ui->user_count; i++) {
        UiUser *u = &ui->users[i];
        if (!u->has_file || u->file[0] == 0) {
            u->same_file = 0;
            continue;
        }
        TempArena t = temp_begin(app->scratch);
        u->same_file = ops->name_eq_ci(app->scratch, str8_from_cstr(u->file), mine);
        temp_end(t);
    }
}

static void refresh_view(App *app) {
    UiApp *ui = &app->ui;
    ui->phase = app->engine.phase;
    // L'état affiché vient de conn.c, pas du moteur : c'est lui qui sait si on
    // réessaie, si on attend, ou si le serveur nous a refusés.
    ui->connecting = app->conn.phase == CONN_TRYING;
    ui->retrying_wait = app->conn.phase == CONN_WAITING;
    ui->retry_seconds = conn_seconds_until_retry(&app->conn, vs_now_ns());
    ui->retrying = ui->retrying_wait || (ui->connecting && app->conn.attempts > 1);
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
    // Chats composés hors ligne : recopiés à chaque frame depuis le moteur,
    // jamais insérés dans l'historique — sinon l'écho du serveur après la
    // reconnexion les afficherait en double.
    ui->pending_count = VS_MIN(engine_pending_chat_count(&app->engine), (isize)UI_MAX_PENDING);
    for (isize i = 0; i < ui->pending_count; i++) {
        Str8 s = engine_pending_chat(&app->engine, i);
        snprintf(ui->pending[i], sizeof(ui->pending[i]), "%.*s", (int)s.len, s.data);
    }
    // Dossiers médias : recopiés pour l'affichage du panneau Réglages.
    ui->media_dir_count = app->media_dir_count;
    for (isize i = 0; i < app->media_dir_count; i++) {
        Str8 d = strbuf_str(&app->media_dirs[i]);
        snprintf(ui->media_dirs[i], sizeof(ui->media_dirs[i]), "%.*s", (int)d.len, d.data);
    }
    // À chaque pas, pas seulement à l'arrivée d'un `users` : ouvrir NOTRE
    // fichier doit éteindre tout de suite les lignes devenues identiques.
    refresh_user_files(app);
}

// refresh_watch_banner propose d'ouvrir le média qu'un autre membre a déclaré,
// dès qu'il diffère du nôtre. Fermable, et il ne réapparaît pas une fois écarté
// pour ce fichier.
//
// VS-039 : la version d'origine sortait dès que NOUS avions un fichier ouvert,
// si bien que le cas « épisode suivant » — un participant change de média en
// cours de séance — ne proposait jamais rien aux autres. Comparer au nôtre
// couvre les deux cas d'un coup : sans fichier ouvert, tout nom déclaré diffère
// du nôtre (vide).
static void refresh_watch_banner(App *app) {
    UiApp *ui = &app->ui;
    const VsDirOps *ops = vs_dir_ops();
    // VS-040 : « ce participant a-t-il un fichier qui vaut d'être proposé ? »
    // est UNE règle (ui_user_openable), partagée avec le double-clic sur sa
    // ligne. Elle a besoin de same_file, recalculé juste avant depuis le moteur.
    refresh_user_files(app);
    for (isize i = 0; i < ui->user_count; i++) {
        UiUser *u = &ui->users[i];
        if (!ui_user_openable(u)) continue;
        TempArena t = temp_begin(app->scratch);
        b32 refused = ops->name_eq_ci(app->scratch, str8_from_cstr(u->file),
                                      str8_from_cstr(ui->watch_dismissed));
        temp_end(t);
        if (refused) return;  // bandeau fermé pour ce fichier : ne pas insister
        snprintf(ui->watch_who, sizeof(ui->watch_who), "%s", u->name);
        snprintf(ui->watch_file, sizeof(ui->watch_file), "%s", u->file);
        ui->watch_show = 1;
        return;
    }
    ui->watch_show = 0;
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

    // Reconnexion : backoff 1 s → 10 s, et uniquement sur panne réseau.
    if (!app->ws_open && conn_should_attempt(&app->conn, now) && net_state(app->net) == NET_STATE_DEAD) {
        conn_attempt_started(&app->conn);
        if (!net_connect(app->net, app->url)) {
            conn_on_socket_down(&app->conn, now);
            ui_set_status(&app->ui, "Adresse de serveur inutilisable : corrigez le champ Serveur.", 1);
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
            conn_on_open(&app->conn);
            // Versions (VS-023) : champs additifs, absents des vieux serveurs.
            app->ui.version_server[0] = 0;
            app->ui.update_available = 0;
            if (m->server_version.len > 0) {
                snprintf(app->ui.version_server, sizeof(app->ui.version_server), "%.*s",
                         (int)m->server_version.len, m->server_version.data);
                if (proto_newer_version(m->server_version, str8_lit(VS_VERSION))) {
                    app->ui.update_available = 1;
                    // Fermée d'un clic, la bannière ne revient qu'à la connexion
                    // suivante : c'est ici, pas à la frame suivante.
                    app->ui.update_dismissed = 0;
                    snprintf(app->ui.update_version, sizeof(app->ui.update_version), "%.*s",
                             (int)m->server_version.len, m->server_version.data);
                }
            }
            if (m->download_url.len > 0) strbuf_set(&app->download_url, m->download_url);
            // Reprise « salle vierge » : le moteur a émis UN control seek,
            // l'utilisateur doit savoir pourquoi le film ne repart pas à zéro.
            if (out.have_resume_toast) {
                i64 total = (i64)f64_round(out.resume_toast_sec);
                if (total < 0) total = 0;
                char resume[80];
                snprintf(resume, sizeof(resume), "Reprise à %02lld:%02lld:%02lld", (long long)(total / 3600),
                         (long long)(total / 60 % 60), (long long)(total % 60));
                ui_toast(&app->ui, resume, 0, now_ms());
            }
            fill_users(app, m);
            refresh_watch_banner(app);
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
            refresh_watch_banner(app);
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
            // Pauses automatiques annoncées par le serveur : compteur publié
            // dans l'état du mode auto, c'est lui qui prouve en séance réelle
            // qu'un changement de fichier ne les déclenche plus (VS-039).
            if (strncmp(text, "Pause auto", 10) == 0) app->auto_pause_toasts++;
            ui_toast(&app->ui, text, level, now_ms());
            break;
        }
        case VS_IN_CHATEVENT: ui_chat_add(&app->ui, m->from, m->text, 0); break;
        case VS_IN_ERROR: {
            // Sur les refus connus, notre message prime sur celui du serveur :
            // il doit dire QUOI corriger, et le champ fautif prend le focus.
            char text[224];
            UiFieldRef focus = UI_FIELD_NONE;
            if (str8_eq_cstr(m->code, "name_taken")) {
                snprintf(text, sizeof(text),
                         "Ce pseudo est déjà pris dans la salle. Choisissez-en un autre puis reconnectez-vous.");
                focus = UI_FIELD_NAME;
            } else if (str8_eq_cstr(m->code, "bad_password")) {
                snprintf(text, sizeof(text),
                         "Mot de passe du serveur incorrect. Corrigez-le puis cliquez sur Se connecter.");
                focus = UI_FIELD_PASSWORD;
            } else if (str8_eq_cstr(m->code, "version_mismatch")) {
                snprintf(text, sizeof(text),
                         "Ce serveur ne parle pas le protocole v%s de ce client (v%s). Mettez le client à jour.",
                         VS_PROTOCOL_VERSION_TEXT, VS_VERSION);
                focus = UI_FIELD_SERVER;
            } else if (m->text.len > 0) {
                snprintf(text, sizeof(text), "%.*s", (int)m->text.len, m->text.data);
            } else {
                snprintf(text, sizeof(text), "Erreur serveur : %.*s", (int)m->code.len, m->code.data);
            }
            if (proto_error_is_fatal(m->code)) {
                // ARRÊT NET : pas de backoff, pas de nouvelle tentative. La
                // fermeture de socket qui suit sera ignorée (phase REFUSED).
                net_close(app->net);
                app->ws_open = 0;
                engine_disconnected(&app->engine);
                conn_on_refused(&app->conn);
                app->ui.screen = UI_SCREEN_CONNECT;
                app->ui.connecting = 0;
                app->ui.focus_request = focus;
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
                Str8 hello = proto_encode_hello(app->scratch, app->name, app->room,
                                                strbuf_str(&app->password), str8_from_cstr(app->session));
                b32 ok = net_send_text(app->net, hello);
                // Le hello encodé porte le mot de passe en clair : il est
                // effacé dès l'envoi, avant même de rendre la mémoire.
                secret_wipe(hello.data, hello.len);
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
                // Refusé par le serveur, ou annulé : la socket qui tombe est la
                // CONSÉQUENCE, pas une panne. Réessayer ici était exactement la
                // boucle « Nouvelle tentative… » signalée sur le terrain.
                if (app->conn.phase == CONN_REFUSED || app->conn.phase == CONN_IDLE) break;
                engine_session_lost(&app->engine);
                b32 first = app->conn.attempts <= 1;
                conn_on_socket_down(&app->conn, vs_now_ns());
                if (app->ui.screen == UI_SCREEN_CONNECT) {
                    ui_set_status(&app->ui,
                                  "Serveur injoignable. Nouvel essai automatique — ou cliquez sur Annuler "
                                  "pour corriger l'adresse.",
                                  1);
                    // Un diagnostic vaut mieux qu'un compteur : on sonde une
                    // fois pour dire DNS, TLS ou port fermé (et détecter le
                    // serveur qui ne répond qu'en chiffré).
                    if (first) probe_health(app);
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
    UiApp *ui = &app->ui;
    Str8 name = str8_trim(ui_text_str(&ui->f_name));
    Str8 room = str8_trim(ui_text_str(&ui->f_room));

    // 1. L'adresse est normalisée AVANT tout : « vibesync.exemple.fr » suffit.
    //    La forme retenue est réécrite dans le champ, pour que l'utilisateur
    //    voie exactement ce qui part.
    Str8 norm;
    const char *err = NULL;
    if (!conn_normalize_url(app->perm, ui_text_str(&ui->f_server), &norm, &err)) {
        ui_set_status(ui, err ? err : "Adresse de serveur invalide.", 1);
        ui->focus_request = UI_FIELD_SERVER;
        return;
    }
    ui_text_set(&ui->f_server, norm);
    if (name.len == 0) {
        ui_set_status(ui, "Choisissez un pseudo : c'est lui qui vous identifie dans la salle.", 1);
        ui->focus_request = UI_FIELD_NAME;
        return;
    }
    if (room.len == 0) {
        ui_set_status(ui, "Indiquez une salle : tous vos amis doivent taper la même.", 1);
        ui->focus_request = UI_FIELD_ROOM;
        return;
    }

    app->url = norm;
    app->name = str8_copy(app->perm, name);
    app->room = str8_copy(app->perm, room);
    strbuf_set(&app->password, ui_text_str(&ui->f_password));
    settings_save(app, 1);

    conn_start(&app->conn, vs_now_ns());
    conn_attempt_started(&app->conn);
    // La salle visée pilote la mémoire de séance et la file de chat hors ligne.
    engine_set_room(&app->engine, app->room);
    engine_connecting(&app->engine);
    ui_set_status(ui, "Connexion au serveur…", 0);
    if (!net_connect(app->net, app->url)) {
        ui_set_status(ui, "Connexion impossible : vérifiez l'adresse du serveur.", 1);
        engine_disconnected(&app->engine);
        conn_cancel(&app->conn);
        ui->focus_request = UI_FIELD_SERVER;
    }
}

// do_cancel_connect rend la main à l'utilisateur pendant une tentative.
static void do_cancel_connect(App *app) {
    net_close(app->net);
    app->ws_open = 0;
    engine_disconnected(&app->engine);
    conn_cancel(&app->conn);
    app->ui.connecting = 0;
    ui_set_status(&app->ui, "Tentative interrompue. Corrigez les champs puis relancez la connexion.", 0);
}

static void do_disconnect(App *app) {
    // « Quitter la salle » est un départ VOLONTAIRE : close 1000 avant de
    // couper, pour que le serveur retire le membre tout de suite et libère le
    // pseudo (docs/protocol.md §Erreurs et robustesse, VS-028).
    net_close_graceful(app->net, NET_CLOSE_GRACE_MS);
    app->ws_open = 0;
    // Plus de session : le clair conservé pour les reconnexions ne sert plus.
    secret_wipe(app->password.data, (isize)sizeof(app->password.data));
    app->password.len = 0;
    engine_disconnected(&app->engine);
    conn_cancel(&app->conn);
    app->ui.screen = UI_SCREEN_CONNECT;
    app->ui.connecting = 0;
    app->ui.user_count = 0;
    app->ui.chat_count = 0;
    app->ui.update_available = 0;
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

// pick_folder ouvre le sélecteur natif en mode dossier (FOS_PICKFOLDERS).
static b32 pick_folder(App *app, Str8 *out) {
    b32 got = 0;
    IFileOpenDialog *dlg = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog,
                                  (void **)&dlg);
    if (FAILED(hr) || !dlg) return 0;
    DWORD opts = 0;
    if (SUCCEEDED(dlg->lpVtbl->GetOptions(dlg, &opts))) {
        dlg->lpVtbl->SetOptions(dlg, opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);
    }
    dlg->lpVtbl->SetTitle(dlg, L"Choisir un dossier de médias");
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

// BROWSE_PATH_CAP : capacité, en unités UTF-16, du tampon que remplit le
// sélecteur. MAX_PATH (260) ne suffit plus : Windows accepte ~32 767
// caractères, et un chemin plus long ferait échouer GetOpenFileNameW sur un
// fichier pourtant valide. 64 Ko pris sur l'arène de travail, le temps de la
// boîte de dialogue.
#define BROWSE_PATH_CAP 32768

// browse_vlc_path ouvre le sélecteur de fichiers standard de Windows sur
// vlc.exe et écrit le chemin choisi (UTF-8, terminé par 0) dans `out`.
// Renvoie 0 si l'utilisateur annule ou si le chemin ne tient pas dans `cap` —
// dans ce dernier cas *too_long passe à 1. On refuse plutôt que de tronquer :
// un chemin tronqué ne désigne rien et serait rejeté à l'enregistrement avec
// un message incompréhensible.
//
// BLOQUANT : la boîte est modale et pompe sa propre boucle de messages. Elle
// est donc appelée depuis le thread UI et lui seul, exactement comme
// open_file_dialog et pick_folder.
static b32 browse_vlc_path(App *app, char *out, isize cap, b32 *too_long) {
    *too_long = 0;
    out[0] = 0;
    TempArena t = temp_begin(app->scratch);

    Str8 current = str8_trim(ui_text_str(&app->ui.f_set_vlc));
    Str8 pf = env_str(app->scratch, L"ProgramFiles");
    Str8 initial =
        vlc_browse_initial_dir(app->scratch, current, pf, browse_dir_exists, app->scratch);

    u16 *file = arena_push_array(app->scratch, u16, BROWSE_PATH_CAP);
    file[0] = 0;
    // Pré-sélection : la boîte s'ouvre sur le fichier déjà configuré plutôt
    // que sur un champ de nom vide.
    if (current.len > 0) {
        isize wlen = 0;
        u16 *w = utf8_to_utf16(app->scratch, current, &wlen);
        if (wlen > 0 && wlen < BROWSE_PATH_CAP) memcpy(file, w, (size_t)(wlen + 1) * sizeof(u16));
    }
    u16 *winitial = initial.len > 0 ? utf8_to_utf16(app->scratch, initial, NULL) : NULL;

    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = app->hwnd;
    // Filtre : paires <libellé>\0<motif>\0, la liste finissant par un \0 de
    // plus. Le premier filtre ne montre que vlc.exe — c'est le seul fichier
    // que ce champ accepte ; le second sert au VLC portable renommé.
    ofn.lpstrFilter = L"vlc.exe\0vlc.exe\0Exécutables (*.exe)\0*.exe\0\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = (LPWSTR)file;
    ofn.nMaxFile = BROWSE_PATH_CAP;
    ofn.lpstrInitialDir = (LPCWSTR)winitial;
    ofn.lpstrTitle = L"Choisir vlc.exe";
    // OFN_NOCHANGEDIR n'est pas cosmétique : sans lui, la boîte laisse le
    // répertoire COURANT du processus là où l'utilisateur a navigué. Tout
    // chemin relatif manipulé ensuite (journal, fichier ini de secours,
    // arguments passés à VLC) partirait alors ailleurs.
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    b32 got = 0;
    if (GetOpenFileNameW(&ofn)) {
        Str8 picked = utf16_to_utf8(app->scratch, file);
        if (picked.len < cap) {
            memcpy(out, picked.data, (size_t)picked.len);
            out[picked.len] = 0;
            got = 1;
        } else {
            *too_long = 1;
        }
    }
    temp_end(t);
    return got;
}

// request_media_open lance la recherche du fichier déclaré par un participant.
static void request_media_open(App *app, Str8 name) {
    UiApp *ui = &app->ui;
    if (name.len == 0) return;
    if (app->media_dir_count == 0) {
        snprintf(ui->media_notice, sizeof(ui->media_notice),
                 "Aucun dossier média configuré — cliquer pour ouvrir les Réglages");
        ui->media_notice_show = 1;
        return;
    }
    strbuf_set(&app->pending_find, name);
    ui->media_notice_show = 0;
    ui->media_searching = 1;
    char msg[224];
    snprintf(msg, sizeof(msg), "Recherche de « %.*s » dans vos dossiers…", (int)name.len, name.data);
    ui_toast(ui, msg, 0, now_ms());
    worker_find(&app->vlc, name, app->media_dirs, app->media_dir_count);
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
    if (ui->act_test_server) {
        ui->act_test_server = 0;
        probe_health(app);
        redraw(app);
    }
    if (ui->act_cancel_connect) {
        ui->act_cancel_connect = 0;
        do_cancel_connect(app);
        redraw(app);
    }
    if (ui->act_use_wss) {
        ui->act_use_wss = 0;
        if (app->suggested_url.len > 0) {
            ui_text_set(&ui->f_server, strbuf_str(&app->suggested_url));
            ui->server_hint[0] = 0;
            ui->health_tls_hint = 0;
            probe_health(app);  // vérifier tout de suite que la bascule marche
        }
        redraw(app);
    }
    if (ui->act_open_user_file) {
        ui->act_open_user_file = 0;
        isize i = ui->act_open_user_index;
        // Même règle que l'affordance (VS-040) : soi-même, participant sans
        // fichier ou fichier déjà le nôtre → rien. Revérifiée ici parce que la
        // liste a pu changer entre le dessin de la frame et son traitement.
        if (i >= 0 && i < ui->user_count && ui_user_openable(&ui->users[i])) {
            request_media_open(app, str8_from_cstr(ui->users[i].file));
        }
        redraw(app);
    }
    if (ui->act_open_watch_file) {
        ui->act_open_watch_file = 0;
        request_media_open(app, str8_from_cstr(ui->watch_file));
        redraw(app);
    }
    if (ui->act_dismiss_watch) {
        ui->act_dismiss_watch = 0;
        ui->watch_show = 0;
        // Refus explicite : ce fichier-là ne sera plus proposé.
        snprintf(ui->watch_dismissed, sizeof(ui->watch_dismissed), "%s", ui->watch_file);
        redraw(app);
    }
    if (ui->act_dismiss_notice) {
        ui->act_dismiss_notice = 0;
        ui->media_notice_show = 0;
        redraw(app);
    }
    if (ui->act_notice_settings) {
        ui->act_notice_settings = 0;
        ui->media_notice_show = 0;
        settings_open(app);
        redraw(app);
    }
    if (ui->act_media_add) {
        ui->act_media_add = 0;
        Str8 dir;
        if (app->media_dir_count < MEDIA_MAX_DIRS && pick_folder(app, &dir)) {
            b32 dup = 0;
            for (isize i = 0; i < app->media_dir_count; i++) {
                if (strbuf_eq(&app->media_dirs[i], dir)) dup = 1;
            }
            if (!dup) {
                strbuf_set(&app->media_dirs[app->media_dir_count++], dir);
                ini_flush_notify(app, "liste des dossiers médias", 1);
            }
        }
        redraw(app);
    }
    if (ui->act_media_remove) {
        ui->act_media_remove = 0;
        isize i = ui->act_media_remove_index;
        if (i >= 0 && i < app->media_dir_count) {
            for (isize j = i; j + 1 < app->media_dir_count; j++) app->media_dirs[j] = app->media_dirs[j + 1];
            app->media_dir_count--;
            ini_flush_notify(app, "liste des dossiers médias", 1);
        }
        redraw(app);
    }
    if (ui->act_remember_changed) {
        ui->act_remember_changed = 0;
        // Décocher doit effacer le secret TOUT DE SUITE, pas à la fermeture :
        // l'utilisateur qui décoche veut que ce soit fait. Le toast de succès
        // MENTIRAIT si l'écriture a échoué (« mot de passe oublié » alors que
        // le chiffré est toujours sur le disque) : c'est le seul geste où on
        // remplace le message plutôt que d'empiler celui d'ini_flush_notify.
        if (settings_save(app, 0)) {
            ui_toast(ui, ui->remember_password ? "Mot de passe mémorisé, chiffré par Windows."
                                               : "Mot de passe oublié.",
                     0, now_ms());
        } else {
            ui_toast(ui,
                     ui->remember_password
                         ? "Mot de passe NON mémorisé : vibesync.ini n'a pas pu être écrit."
                         : "Mot de passe NON effacé du disque : vibesync.ini n'a pas pu être écrit.",
                     1, now_ms());
        }
        redraw(app);
    }
    if (ui->act_update_dismiss) {
        ui->act_update_dismiss = 0;
        ui->update_dismissed = 1;
        redraw(app);
    }
    if (ui->act_update_download) {
        ui->act_update_download = 0;
        // Ouverture dans le navigateur : le client ne télécharge ni n'installe
        // rien lui-même. Adresse fournie par le serveur, donc validée d'abord.
        Str8 url = strbuf_str(&app->download_url);
        if (str8_starts_with(url, str8_lit("https://")) || str8_starts_with(url, str8_lit("http://"))) {
            TempArena t = temp_begin(app->scratch);
            u16 *w = utf8_to_utf16(app->scratch, url, NULL);
            ShellExecuteW(app->hwnd, L"open", (LPCWSTR)w, NULL, NULL, SW_SHOWNORMAL);
            temp_end(t);
        } else {
            ui_toast(ui, "Le serveur n'a pas fourni d'adresse de téléchargement valide.", 1, now_ms());
        }
    }
    if (ui->act_settings_open) {
        ui->act_settings_open = 0;
        settings_open(app);
        redraw(app);
    }
    if (ui->act_settings_detect) {
        ui->act_settings_detect = 0;
        ui_text_set(&ui->f_set_vlc, app->vlc_auto);
        redraw(app);
    }
    if (ui->act_settings_browse) {
        ui->act_settings_browse = 0;
        // Le champ est la seule destination : on remplace son contenu, et
        // settings_validate() (plus bas, à chaque tour) rafraîchit le voyant
        // « vlc.exe trouvé / Aucun VLC détecté » comme après une frappe.
        char picked[UI_TEXT_CAP];
        b32 too_long = 0;
        if (browse_vlc_path(app, picked, (isize)sizeof(picked), &too_long)) {
            ui_text_set(&ui->f_set_vlc, str8_from_cstr(picked));
            ui->focus = 0;
            ui->settings_msg[0] = 0;
            ui->settings_msg_error = 0;
        } else if (too_long) {
            snprintf(ui->settings_msg, sizeof(ui->settings_msg),
                     "Ce chemin est trop long pour le champ (%d octets au plus).", (int)(UI_TEXT_CAP - 1));
            ui->settings_msg_error = 1;
        }
        redraw(app);
    }
    if (ui->act_settings_cancel) {
        ui->act_settings_cancel = 0;
        ui->settings_open = 0;
        ui->focus = 0;
        redraw(app);
    }
    if (ui->act_settings_save) {
        ui->act_settings_save = 0;
        if (settings_apply(app)) {
            ui->settings_open = 0;
            ui->focus = 0;
            ui_toast(ui, "Réglages enregistrés.", 0, now_ms());
        }
        redraw(app);
    }
    // Le voyant du chemin VLC est recalculé hors frame : un accès disque par
    // changement de texte, pas un par image.
    if (settings_validate(app)) redraw(app);
    dispatch_output(app, &out);
    refresh_view(app);
}

// ------------------------------------------------- pilote du harnais réel ---
//
// Voir auto.h. Trois entrées seulement : auto_start (démarrage), auto_pump
// (une fois par TIMER_AUTO) et auto_write_status (publication de l'état).

// auto_write_status publie l'état courant, une ligne de JSON, pour que le
// script puisse asserter. Mêmes clés que l'état du client macOS
// (AppModel.autoWriteStatus) : le harnais Windows et le harnais mac lisent le
// même vocabulaire.
static void auto_write_status(App *app) {
    AutoPilot *ap = &app->autop;
    if (!ap->on || ap->status_path.len == 0) return;
    VsEngine *e = &app->engine;
    UiApp *ui = &app->ui;
    i64 now = vs_now_ns();

    b32 vlc_running;
    int vlc_port;
    char vlc_password[64];
    AcquireSRWLockExclusive(&app->vlc.lock);
    vlc_running = app->vlc.running;
    vlc_port = app->vlc.client.port;
    memcpy(vlc_password, app->vlc.client.password, sizeof(vlc_password));
    ReleaseSRWLockExclusive(&app->vlc.lock);

    TempArena t = temp_begin(app->scratch);
    JsonWriter w;
    jw_init(&w, app->scratch);
    jw_obj_begin(&w);
    jw_kv_i64(&w, "ts", vs_ns_to_unix_ms(now));
    jw_kv_i64(&w, "pid", (i64)GetCurrentProcessId());
    jw_kv_str(&w, "scenario", ap->scenario);
    jw_kv_str(&w, "name", ap->name);
    jw_kv_str(&w, "room", ap->room);
    jw_key(&w, "phase");
    jw_cstr(&w, e->phase == VS_PHASE_CONNECTED ? "connected"
                                               : (e->phase == VS_PHASE_CONNECTING ? "connecting" : "idle"));
    jw_kv_bool(&w, "connected", e->phase == VS_PHASE_CONNECTED);
    jw_kv_i64(&w, "users", (i64)ui->user_count);
    jw_kv_bool(&w, "ready", e->ready);
    jw_kv_str(&w, "file", strbuf_str(&e->file_name));
    jw_kv_bool(&w, "fileDeclared", e->have_file);
    jw_kv_bool(&w, "vlcRunning", vlc_running);
    // Port et mot de passe de l'interface locale de VLC. MODE AUTO UNIQUEMENT :
    // sans eux, un tiers ne peut pas jouer le rôle de l'utilisateur DANS VLC
    // (pause/seek envoyés hors du client), puisqu'ils sont tirés au hasard à
    // chaque lancement — et c'est très exactement ce que VS-029 demande de
    // prouver. Ils ne quittent jamais le fichier d'état du harnais (dossier
    // temporaire) et n'apparaissent JAMAIS dans vibesync.log.
    jw_kv_i64(&w, "vlcPort", vlc_running ? (i64)vlc_port : 0);
    jw_key(&w, "vlcPassword");
    jw_cstr(&w, vlc_running ? vlc_password : "");
    jw_key(&w, "vlcState");
    jw_cstr(&w, e->have_status ? vs_play_state_name(e->status.state) : "stopped");
    jw_kv_num(&w, "positionSec", e->have_status ? e->status.position_sec : 0);
    jw_kv_num(&w, "durationSec", e->have_status ? e->status.length_sec : 0);
    jw_kv_num(&w, "roomPositionSec", engine_expected_position(e, now));
    jw_kv_bool(&w, "paused", e->room_state.paused || !e->have_state);
    jw_kv_num(&w, "driftSec", e->drift);
    jw_kv_bool(&w, "buffering", e->buffering);
    jw_kv_i64(&w, "latencyMs", e->latency_ms);
    jw_kv_bool(&w, "correcting", e->correcting != VS_CORRECT_NONE);
    // Bandeau « X regarde <fichier> » (VS-026, élargi au changement de fichier
    // en cours de salle par VS-039) et pauses automatiques reçues : le harnais
    // doit pouvoir constater les deux.
    jw_kv_bool(&w, "watchShow", ui->watch_show);
    jw_key(&w, "watchFile");
    jw_cstr(&w, ui->watch_file);
    jw_kv_i64(&w, "autoPauseToasts", app->auto_pause_toasts);
    // Commandes envoyées à VLC depuis le lancement, par nature : `rateCmds`
    // doit rester à 0 sur une séance normale (VS-038).
    jw_kv_i64(&w, "pauseCmds", app->cmd_counts[VS_CMD_PAUSE]);
    jw_kv_i64(&w, "resumeCmds", app->cmd_counts[VS_CMD_RESUME]);
    jw_kv_i64(&w, "seekCmds", app->cmd_counts[VS_CMD_SEEK]);
    jw_kv_i64(&w, "rateCmds", app->cmd_counts[VS_CMD_RATE]);
    // Libellés de l'interface : c'est là que se lisent la cause d'un échec de
    // connexion et l'état du lancement de VLC.
    jw_key(&w, "error");
    jw_cstr(&w, ui->status_error ? ui->status : "");
    jw_key(&w, "connection");
    jw_cstr(&w, ui->status);
    jw_key(&w, "lastError");
    jw_cstr(&w, ui->toast_level > 0 ? ui->toast : "");
    jw_key(&w, "media");
    jw_cstr(&w, ui->media_notice_show ? ui->media_notice : ui->file_name);
    jw_obj_end(&w);

    Str8 line = str8_cat(app->scratch, jw_result(&w), str8_lit("\n"));
    if (!auto_write_atomic(app->scratch, ap->status_path, line)) {
        // Un état non écrit ne doit pas faire tomber l'application ; il doit
        // laisser une trace, une seule.
        static b32 warned = 0;
        if (!warned) {
            warned = 1;
            vs_log("auto: état non écrit dans \"%.*s\"", (int)ap->status_path.len,
                   (const char *)ap->status_path.data);
        }
    }
    temp_end(t);
}

// auto_run exécute une commande du pilote. Elle passe par les MÊMES entrées du
// moteur que les boutons de l'interface (engine_user_control…) : le harnais
// exerce le vrai chemin, pas un raccourci.
static void auto_run(App *app, const AutoCmd *cmd) {
    VsOutput out;
    vs_output_reset(&out);
    i64 now = vs_now_ns();
    switch (cmd->kind) {
        case AUTO_CMD_PLAY: engine_user_control(&app->engine, now, VS_ACT_PLAY, 0, 0, &out); break;
        case AUTO_CMD_PAUSE: engine_user_control(&app->engine, now, VS_ACT_PAUSE, 0, 0, &out); break;
        case AUTO_CMD_SEEK:
            engine_user_control(&app->engine, now, VS_ACT_SEEK, cmd->value, 1, &out);
            break;
        case AUTO_CMD_READY: engine_set_ready(&app->engine, cmd->flag, &out); break;
        case AUTO_CMD_CHAT: engine_chat(&app->engine, cmd->text, &out); break;
        case AUTO_CMD_OPEN: worker_open(&app->vlc, cmd->text); break;
        case AUTO_CMD_QUIT:
            // Chemin NORMAL de fermeture : c'est lui qui envoie la close 1000
            // et arrête VLC (VS-028).
            PostMessageW(app->hwnd, WM_CLOSE, 0, 0);
            break;
        case AUTO_CMD_NONE: break;
    }
    dispatch_output(app, &out);
    refresh_view(app);
}

// auto_pump : commandes en attente puis état publié.
static void auto_pump(App *app) {
    AutoPilot *ap = &app->autop;
    if (!ap->on) return;
    if (ap->cmds_path.len > 0) {
        TempArena t = temp_begin(app->scratch);
        Str8 text = auto_read_text(app->scratch, ap->cmds_path);
        // Le fichier n'est jamais réécrit par l'application : le script y
        // ajoute, nous comptons ce qui a déjà été fait. Seules les lignes
        // TERMINÉES par un saut de ligne sont exécutées — la dernière peut
        // être en cours d'écriture.
        isize start = 0, index = 0;
        for (isize i = 0; i < text.len; i++) {
            if (text.data[i] != '\n') continue;
            Str8 line = str8_sub(text, start, i - start);
            start = i + 1;
            index++;
            if (index <= ap->cmds_done) continue;
            ap->cmds_done = index;
            AutoCmd cmd;
            if (auto_parse(line, &cmd)) auto_run(app, &cmd);
        }
        temp_end(t);
    }
    i64 now = now_ms();
    if (now - ap->last_status_ms >= VS_AUTO_STATUS_PERIOD_MS) {
        ap->last_status_ms = now;
        auto_write_status(app);
    }
}

// auto_start : connexion immédiate, ouverture du média, premier état publié.
// Sans pilote, ne fait rien.
static void auto_start(App *app) {
    AutoPilot *ap = &app->autop;
    if (!ap->on) return;
    UiApp *ui = &app->ui;
    ui_text_set(&ui->f_server, ap->url);
    ui_text_set(&ui->f_name, ap->name);
    ui_text_set(&ui->f_room, ap->room);
    ui_text_set(&ui->f_password, ap->password);
    // Le harnais n'a aucune raison de laisser un secret derrière lui, même
    // chiffré : le mot de passe reste en mémoire et n'entre pas dans l'ini.
    ui->remember_password = 0;
    vs_log("auto: pilote actif (scénario \"%.*s\", salle \"%.*s\", fichier \"%.*s\")",
           (int)ap->scenario.len, (const char *)ap->scenario.data, (int)ap->room.len,
           (const char *)ap->room.data, (int)ap->file.len, (const char *)ap->file.data);
    do_connect(app);
    if (ap->file.len > 0) worker_open(&app->vlc, ap->file);
    auto_write_status(app);
    ap->last_status_ms = now_ms();
    SetTimer(app->hwnd, TIMER_AUTO, VS_AUTO_PUMP_MS, NULL);
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
            // La fenêtre disparaît : un toast ne serait jamais lu, seul le
            // journal peut témoigner de la perte des réglages.
            settings_save(app, 0);
            // Fermer la fenêtre est un départ volontaire au même titre que
            // « Quitter la salle » : la close 1000 part AVANT la destruction de
            // la socket (VS-028). Coût au pire NET_CLOSE_GRACE_MS, et zéro
            // quand la connexion est déjà tombée.
            net_close_graceful(app->net, NET_CLOSE_GRACE_MS);
            app->ws_open = 0;
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
        case WM_LBUTTONDBLCLK:
            SetCapture(hwnd);
            ui_on_mouse_double(&app->ui, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
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
        case WM_KEYDOWN: {
            u32 mods = 0;
            if (GetKeyState(VK_CONTROL) & 0x8000) mods |= UI_MOD_CTRL;
            if (GetKeyState(VK_SHIFT) & 0x8000) mods |= UI_MOD_SHIFT;
            ui_on_key(&app->ui, (u32)wp, mods);
            redraw(app);
            return 0;
        }
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
            if (wp == TIMER_AUTO) {
                auto_pump(app);
                redraw(app);
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
        case WM_APP_HEALTH: {
            HealthResult r;
            i64 gen = 0;
            b32 has = 0;
            AcquireSRWLockExclusive(&app->health.lock);
            has = app->health.has_result;
            r = app->health.result;
            gen = app->health.result_gen;
            app->health.has_result = 0;
            ReleaseSRWLockExclusive(&app->health.lock);
            // Une réponse à une adresse qu'on a déjà quittée n'intéresse plus.
            if (has && gen == app->health_gen) apply_health(app, &r);
            redraw(app);
            return 0;
        }
        case WM_APP_VLC_OPEN: {
            b32 ok, failed, find_done, find_found;
            char err[VLC_OPEN_ERROR_CAP];
            StrBuf name;
            i64 size;
            MediaFind found;
            AcquireSRWLockExclusive(&app->vlc.lock);
            ok = app->vlc.open_ok;
            failed = app->vlc.open_failed;
            memcpy(err, app->vlc.open_error, sizeof(err));
            name = app->vlc.open_name;
            size = app->vlc.open_size;
            find_done = app->vlc.find_done;
            find_found = app->vlc.find_found;
            found = app->vlc.find_result;
            app->vlc.open_ok = 0;
            app->vlc.open_failed = 0;
            app->vlc.find_done = 0;
            ReleaseSRWLockExclusive(&app->vlc.lock);

            if (find_done) {
                app->ui.media_searching = 0;
                Str8 wanted = strbuf_str(&app->pending_find);
                if (find_found) {
                    // Homonymes : le plus gros gagne. On le trace, c'est une
                    // heuristique et il faut pouvoir la contester.
                    char dbg[320];
                    snprintf(dbg, sizeof(dbg),
                             "vibesync: « %.*s » trouvé (%lld correspondance(s), %lld entrées) : %s\n",
                             (int)wanted.len, wanted.data, (long long)found.matches,
                             (long long)found.visited, (const char *)found.path.data);
                    OutputDebugStringA(dbg);
                    vs_write_stderr(str8_from_cstr(dbg));
                } else {
                    snprintf(app->ui.media_notice, sizeof(app->ui.media_notice),
                             "« %.*s » introuvable%s — cliquer pour ouvrir les Réglages", (int)wanted.len,
                             wanted.data, found.truncated ? " (recherche écourtée)" : "");
                    app->ui.media_notice_show = 1;
                    app->ui.watch_show = 0;
                }
            }
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

    UiApp *ui = &app->ui;
    // Scénario documenté : l'utilisateur a tapé ws://, le serveur ne répond
    // qu'en chiffré. Diagnostic explicite + bascule proposée, pas de boucle.
    ui_set_status(ui, "Serveur injoignable. Nouvel essai automatique — ou cliquez sur Annuler pour "
                      "corriger l'adresse.",
                  1);
    ui->health = UI_HEALTH_FAIL;
    snprintf(ui->health_msg, sizeof(ui->health_msg), "connexion refusée");
    ui->health_latency_ms = 42;
    ui->health_tls_hint = 1;
    snprintf(ui->server_hint, sizeof(ui->server_hint),
             "Le serveur répond en chiffré : passez en wss://");
    ui->retrying_wait = 1;
    ui->retry_seconds = 4;
    ui_text_set(&ui->f_server, str8_lit("ws://vibesync.thibault.fr/ws"));
    ui_text_set(&ui->f_name, str8_lit("thibault"));
    ui_text_set(&ui->f_room, str8_lit("soirée-film"));
    ui_text_set(&ui->f_password, str8_lit("secret"));
    ui->remember_password = 1;
    ui->screen = UI_SCREEN_CONNECT;

    // Sélection à la souris, pour de vrai : une frame de mesure, puis un
    // appui-glisser-relâcher sur le champ Pseudo (2e champ dessiné). La capture
    // exerce donc le hit-test et le rendu de sélection, elle ne les mime pas.
    ui->probe_index = 2;
    ui_frame(ui, app->mem_dc, w, h, now_ms());
    i32 fy = ui->probe_y + ui->probe_h / 2;
    i32 x0 = ui->probe_x + MulDiv(24, ui->dpi, 96);
    i32 x1 = ui->probe_x + MulDiv(200, ui->dpi, 96);  // au-delà du texte : jusqu'à la fin
    ui_on_mouse_down(ui, x0, fy);
    ui_frame(ui, app->mem_dc, w, h, now_ms());
    ui_on_mouse_move(ui, (x0 + x1) / 2, fy);
    ui_frame(ui, app->mem_dc, w, h, now_ms());
    ui_on_mouse_move(ui, x1, fy);
    ui_frame(ui, app->mem_dc, w, h, now_ms());
    ui_on_mouse_up(ui, x1, fy);
    ui->probe_index = 0;
    ui_frame(ui, app->mem_dc, w, h, now_ms());
    png_write(app->scratch, str8_cat(app->scratch, dir, str8_lit("\\ui-connexion.png")),
              (const u8 *)app->bits, w, h);

    // Panneau Réglages, superposé au même écran (état nominal, sans l'alerte).
    ui->retrying_wait = 0;
    ui->server_hint[0] = 0;
    ui->health = UI_HEALTH_OK;
    ui->health_latency_ms = 113;
    settings_open(app);
    ui_text_set(&ui->f_set_server, str8_lit("wss://vibesync.thibault.fr/ws"));
    ui_text_set(&ui->f_set_name, str8_lit("thibault"));
    ui_text_set(&ui->f_set_room, str8_lit("soirée-film"));
    ui_text_set(&ui->f_set_vlc, str8_lit("C:\\Program Files\\VideoLAN\\VLC\\vlc.exe"));
    snprintf(ui->settings_auto_vlc, sizeof(ui->settings_auto_vlc), "C:\\Program Files\\VideoLAN\\VLC\\vlc.exe");
    ui->settings_vlc_state = 1;
    ui->media_dir_count = 2;
    snprintf(ui->media_dirs[0], sizeof(ui->media_dirs[0]), "C:\\Users\\thibault\\Downloads");
    snprintf(ui->media_dirs[1], sizeof(ui->media_dirs[1]), "D:\\Films");
    ui_frame(ui, app->mem_dc, w, h, now_ms());
    png_write(app->scratch, str8_cat(app->scratch, dir, str8_lit("\\ui-reglages.png")), (const u8 *)app->bits,
              w, h);
    ui->settings_open = 0;

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
    ui->users[1].same_file = 1;  // même fichier que nous : ligne inerte (VS-040)
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
    snprintf(ui->version_server, sizeof(ui->version_server), "0.3.0");
    snprintf(ui->update_version, sizeof(ui->update_version), "0.3.0");
    ui->update_available = 1;
    ui->update_dismissed = 0;
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
    settings_load(app);
    session_load(app);  // après settings_load : le jeton vient de l'ini relu

    app->net = arena_push_struct(perm, Net);
    if (!net_init(app->net)) return 2;

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;  // CS_DBLCLKS : sélection de mot
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    // Icône de la ressource 1 : grande taille pour Alt+Tab et la barre des
    // tâches, petite taille pour la barre de titre (le .ico porte les deux).
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_SHARED);
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
    health_start(&app->health, hwnd);

    Str8 cl = utf16_to_utf8(scratch, (const u16 *)GetCommandLineW());

    // Mode capture : rendu des deux écrans en PNG, puis sortie.
    {
        Str8 dir, dpi_arg;
        if (cmd_opt(cl, "--capture", &dir)) {
            // « --dpi 144 » force l'échelle de la capture : c'est le seul moyen
            // de vérifier le rendu à 150 % sans changer l'écran de la machine.
            if (cmd_opt(cl, "--dpi", &dpi_arg)) {
                i64 v = 0;
                if (str_to_i64(dpi_arg, &v) && v >= 96 && v <= 384) ui_set_dpi(&app->ui, (i32)v);
            }
            capture_screens(app, str8_copy(scratch, dir));
            char msg[128];
            snprintf(msg, sizeof(msg), "captures ecrites en %lld ms\n",
                     (long long)((i64)GetTickCount64() - start));
            vs_write_stderr(str8_from_cstr(msg));
            worker_stop(&app->vlc);
            health_stop(&app->health);
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

    // Mode auto (VS-029) : piloté par l'environnement, pour le harnais de test
    // réel scripts/run-real-vm.ps1. Sans VIBESYNC_AUTO_URL, rien ne change.
    if (auto_from_env(perm, &app->autop)) auto_start(app);

    // Premier diagnostic de joignabilité : la pastille est renseignée avant même
    // que l'utilisateur touche au formulaire.
    if (app->ui.f_server.len > 0) probe_health(app);

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    settings_save(app, 0);  // plus d'écran à qui parler : journal seulement
    // Dernier geste d'hygiène : plus aucun clair en mémoire à la sortie.
    secret_wipe(app->password.data, (isize)sizeof(app->password.data));
    secret_wipe(app->ui.f_password.data, (isize)sizeof(app->ui.f_password.data));
    worker_stop(&app->vlc);
    health_stop(&app->health);
    net_destroy(app->net);
    ui_release(&app->ui);
    CoUninitialize();
    return 0;
}
