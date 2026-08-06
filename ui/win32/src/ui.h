// ui.h — interface immediate-mode dessinée à la main en GDI.
//
// Aucun contrôle Win32 (hors la fenêtre elle-même) : chaque bouton, champ de
// saisie, liste et barre de position est dessiné et géré ici, sur un
// back-buffer que main.c blitte d'un coup. L'UI ne connaît ni le réseau ni
// VLC : main.c remplit les champs « vue » avant la frame et consomme les
// actions produites après.
#ifndef VS_UI_H
#define VS_UI_H

#include "base.h"
#include "engine.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define UI_TEXT_CAP 320
#define UI_MAX_CHAT 128
#define UI_MAX_USERS 16
#define UI_CHAT_LINE_CAP 240

// --- palette (thème sombre) ---
#define UI_BG 0x1a1b1eu
#define UI_PANEL 0x232529u
#define UI_PANEL_HI 0x2b2e34u
#define UI_LINE 0x33373eu
#define UI_TEXT 0xe8e8eau
#define UI_MUTED 0x9aa0a6u
#define UI_FAINT 0x6b7178u
#define UI_ACCENT 0x8b5cf6u
#define UI_ACCENT_HI 0xa17cffu
#define UI_ACCENT_DIM 0x4c3a8cu
#define UI_CYAN 0x22d3eeu
#define UI_OK 0x22c55eu
#define UI_WARN 0xf59e0bu
#define UI_DANGER 0xef4444u

typedef enum {
    UI_SCREEN_CONNECT = 0,
    UI_SCREEN_ROOM,
} UiScreen;

// État de la sonde /healthz affichée sur l'écran de connexion.
typedef enum {
    UI_HEALTH_UNKNOWN = 0,
    UI_HEALTH_TESTING,
    UI_HEALTH_OK,
    UI_HEALTH_FAIL,
} UiHealth;

// Champ visé par une demande de focus venue de main.c (message d'erreur qui
// désigne le champ à corriger).
typedef enum {
    UI_FIELD_NONE = 0,
    UI_FIELD_SERVER,
    UI_FIELD_NAME,
    UI_FIELD_ROOM,
    UI_FIELD_PASSWORD,
} UiFieldRef;

// --- modèle d'édition (découplé du rendu, donc testable sans fenêtre) ---
//
// UiText est un champ de saisie mono-ligne : contenu UTF-8, plus deux bornes en
// octets. `caret` est l'extrémité mobile de la sélection, `anchor` l'extrémité
// fixe ; les deux confondues signifient « pas de sélection ». Toutes les
// fonctions ci-dessous sont de purs calculs sur ces trois champs : aucune
// dépendance à GDI, à une fenêtre ou à une arène.
typedef struct {
    u8 data[UI_TEXT_CAP];
    isize len;
    isize caret;
    isize anchor;
    i32 scroll;  // défilement horizontal en pixels (mémorisé par le rendu)
} UiText;

void ui_text_set(UiText *t, Str8 s);
Str8 ui_text_str(const UiText *t);

// Frontières de points de code (jamais au milieu d'un caractère UTF-8).
isize ui_text_prev_cp(const UiText *t, isize i);
isize ui_text_next_cp(const UiText *t, isize i);

isize ui_text_sel_lo(const UiText *t);
isize ui_text_sel_hi(const UiText *t);
b32 ui_text_has_sel(const UiText *t);
Str8 ui_text_selection(const UiText *t);

// ui_text_move place le caret ; `extend` conserve l'ancre (Maj+flèches, drag).
void ui_text_move(UiText *t, isize pos, b32 extend);
void ui_text_select_range(UiText *t, isize lo, isize hi);
void ui_text_select_all(UiText *t);
void ui_text_clear_sel(UiText *t);

// ui_text_delete_sel supprime la sélection ; renvoie 1 si quelque chose a été
// supprimé. Toutes les insertions et suppressions passent par elle : une frappe
// remplace naturellement la sélection.
b32 ui_text_delete_sel(UiText *t);
void ui_text_insert_cp(UiText *t, u32 cp);
// ui_text_insert_str insère une chaîne UTF-8 en filtrant les sauts de ligne et
// tabulations (le champ est mono-ligne). Tronque proprement si la place manque.
void ui_text_insert_str(UiText *t, Str8 s);
void ui_text_backspace(UiText *t, b32 word);
void ui_text_delete_fwd(UiText *t, b32 word);

// Découpage en mots : classes espace / mot / ponctuation, comme les éditeurs
// Windows. Les octets ≥ 0x80 (lettres accentuées) comptent comme du mot.
void ui_text_word_bounds(const UiText *t, isize pos, isize *lo, isize *hi);
isize ui_text_word_left(const UiText *t, isize pos);
isize ui_text_word_right(const UiText *t, isize pos);

// UiTextMetrics donne la largeur en pixels du préfixe [0, byte_off) tel qu'il
// est affiché. Le rendu la branche sur GetTextExtentPoint32W ; les tests la
// branchent sur une police fictive. C'est le seul point de contact entre le
// modèle d'édition et le dessin.
typedef struct {
    i32 (*prefix_width)(void *ctx, const UiText *t, isize byte_off);
    void *ctx;
} UiTextMetrics;

// ui_text_hit renvoie l'offset en octets de la frontière la plus proche de `x`
// (x = pixels depuis le début du texte, défilement déjà déduit).
isize ui_text_hit(const UiText *t, const UiTextMetrics *m, i32 x);

typedef struct {
    char name[64];
    char file[128];
    b32 ready;
    b32 is_self;
    b32 has_file;
    i64 latency_ms;
} UiUser;

typedef struct {
    char from[48];
    char text[UI_CHAT_LINE_CAP];
    b32 system;
} UiChatLine;

// Modificateurs d'une frappe : le champ de saisie a besoin de Maj (extension de
// sélection) autant que de Ctrl (raccourcis).
#define UI_MOD_CTRL 1u
#define UI_MOD_SHIFT 2u

typedef struct {
    u32 vk;
    u32 mods;
} UiKeyEvent;

typedef struct {
    // ---- vue (remplie par main.c avant chaque frame) ----
    UiScreen screen;
    VsPhase phase;
    b32 retrying;
    b32 connecting;
    char status[224];
    b32 status_error;

    char room[64];
    UiUser users[UI_MAX_USERS];
    isize user_count;
    b32 ready;
    b32 paused;
    f64 position_sec;
    f64 duration_sec;
    f64 drift_sec;
    i64 latency_ms;
    b32 vlc_running;
    b32 buffering;
    b32 correcting;
    char file_name[160];

    UiChatLine chat[UI_MAX_CHAT];
    isize chat_count;
    isize chat_scroll;  // 0 = collé en bas

    char toast[224];
    i64 toast_until_ms;
    int toast_level;  // 0 info, 1 warn, 2 erreur

    // ---- joignabilité du serveur (écran de connexion) ----
    UiHealth health;
    char health_msg[160];    // « en ligne », « nom introuvable (DNS) »…
    i64 health_latency_ms;
    b32 health_tls_hint;     // le serveur répond en TLS alors qu'on vise ws://
    char server_hint[280];   // adresse normalisée, si elle diffère de la saisie
    b32 retrying_wait;       // attente de backoff : bouton Annuler proposé
    i64 retry_seconds;

    // ---- versions et mise à jour (VS-023) ----
    char version_client[24];
    char version_server[24];
    b32 update_available;   // serveur plus récent que ce client
    b32 update_dismissed;   // bannière fermée pour cette session
    char update_version[24];

    // ---- saisie ----
    UiText f_server, f_name, f_room, f_password, f_chat;
    // Mémorisation du mot de passe (VS-025) : l'état est persisté, le secret
    // lui-même est chiffré par DPAPI côté main.c.
    b32 remember_password;

    // ---- panneau Réglages (superposé à l'écran courant) ----
    b32 settings_open;
    UiText f_set_server, f_set_name, f_set_room, f_set_vlc;
    char settings_auto_vlc[300];  // chemin détecté automatiquement (vue)
    i32 settings_vlc_state;       // 0 vide (auto), 1 trouvé, 2 introuvable
    char settings_msg[224];
    b32 settings_msg_error;

    // ---- actions produites par la frame ----
    b32 act_connect;
    b32 act_disconnect;
    b32 act_ready;
    b32 act_play;
    b32 act_pause;
    b32 act_seek;
    f64 act_seek_pos;
    b32 act_open_file;
    b32 act_chat_send;
    b32 act_settings_open;
    b32 act_settings_save;
    b32 act_settings_cancel;
    b32 act_settings_detect;
    b32 act_test_server;       // « Tester » ou sortie du champ Serveur
    b32 act_cancel_connect;    // interrompt une tentative / une attente
    b32 act_use_wss;           // accepte la bascule ws:// → wss://
    b32 act_update_download;
    b32 act_update_dismiss;
    b32 act_remember_changed;  // la case « se souvenir » vient de basculer

    // focus_request : main.c désigne le champ à corriger après une erreur.
    UiFieldRef focus_request;

    // ---- interne ----
    u64 hot, active, focus, focus_prev;
    i32 mouse_x, mouse_y;
    b32 mouse_down, mouse_pressed, mouse_released, mouse_double;
    i32 wheel;
    u32 chars[32];
    isize char_count;
    UiKeyEvent keys[32];
    isize key_count;
    b32 dragging_seek;
    b32 dragging_text;   // un champ suit la souris (sélection au drag)
    b32 hot_is_text;     // le survol courant est un champ (curseur en I)
    b32 input_locked;    // vrai pendant le dessin de l'écran couvert par un modal
    i64 caret_blink_ms;

    b32 take_next_focus;  // Tab : le prochain champ dessiné prend le focus

    // Sonde de diagnostic : si probe_index vaut n > 0, le rectangle du n-ième
    // champ dessiné dans la frame est publié ci-dessous. Le mode --capture s'en
    // sert pour piloter une vraie sélection à la souris sans deviner la mise en
    // page ; sans elle, la capture ne prouverait rien.
    i32 probe_index;
    i32 probe_x, probe_y, probe_w, probe_h;
    i32 field_seq;  // compteur de champs de la frame courante

    i32 dpi;
    i32 width, height;
    HFONT f_body, f_small, f_bold, f_title, f_huge;
    HWND hwnd;
    Arena *scratch;  // arène de frame (conversions UTF-16, mises en forme)
    b32 need_timer;  // vrai si la frame veut être rafraîchie régulièrement
} UiApp;

void ui_init(UiApp *app);
void ui_release(UiApp *app);
// ui_set_dpi (re)crée les fontes pour un DPI donné.
void ui_set_dpi(UiApp *app, i32 dpi);
// ui_frame dessine tout et traite les entrées accumulées.
void ui_frame(UiApp *app, HDC dc, i32 w, i32 h, i64 now_ms);

// --- entrées, appelées depuis la procédure de fenêtre ---
void ui_on_mouse_move(UiApp *app, i32 x, i32 y);
void ui_on_mouse_down(UiApp *app, i32 x, i32 y);
void ui_on_mouse_double(UiApp *app, i32 x, i32 y);
void ui_on_mouse_up(UiApp *app, i32 x, i32 y);
void ui_on_wheel(UiApp *app, i32 delta);
void ui_on_char(UiApp *app, u32 cp);
void ui_on_key(UiApp *app, u32 vk, u32 mods);

// --- alimentation par main.c ---
void ui_toast(UiApp *app, const char *text, int level, i64 now_ms);
void ui_chat_add(UiApp *app, Str8 from, Str8 text, b32 system);
void ui_set_status(UiApp *app, const char *text, b32 is_error);

// ui_format_time formate une durée en h:mm:ss ou m:ss (« 0:00 », « 1:23:45 »).
void ui_format_time(f64 sec, char *buf, isize cap);

#endif // VS_UI_H
