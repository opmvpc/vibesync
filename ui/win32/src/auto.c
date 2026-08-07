// auto.c — mode auto du client Windows (VS-029), pendant de AutoPilot.swift.
//
// Lecture de l'environnement, analyse des commandes, E/S des deux fichiers du
// harnais. Le câblage sur le moteur et l'UI vit dans main.c : ici, rien qui
// connaisse l'application.
#include "auto.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string.h>

// ---------------------------------------------------------- environnement ---

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

// auto_var lit VIBESYNC_AUTO_<clé>, rognée aux deux bouts.
static Str8 auto_var(Arena *a, const char *key) {
    return str8_trim(env_var(a, key));
}

b32 auto_from_env(Arena *a, AutoPilot *out) {
    memset(out, 0, sizeof(*out));
    Str8 url = auto_var(a, "VIBESYNC_AUTO_URL");
    if (url.len == 0) return 0;
    out->on = 1;
    out->url = url;
    out->name = auto_var(a, "VIBESYNC_AUTO_NAME");
    if (out->name.len == 0) out->name = str8_lit("auto");
    out->room = auto_var(a, "VIBESYNC_AUTO_ROOM");
    if (out->room.len == 0) out->room = str8_lit("salon");
    // Le mot de passe n'est PAS rogné : un espace final est un caractère du
    // secret comme un autre (même règle que côté Swift).
    out->password = env_var(a, "VIBESYNC_AUTO_PASSWORD");
    out->file = auto_var(a, "VIBESYNC_AUTO_FILE");
    out->status_path = auto_var(a, "VIBESYNC_AUTO_STATUS");
    out->cmds_path = auto_var(a, "VIBESYNC_AUTO_CMDS");
    out->scenario = auto_var(a, "VIBESYNC_AUTO_SCENARIO");
    out->last_status_ms = 0;
    return 1;
}

// -------------------------------------------------------------- commandes ---

// verb_is compare un verbe sans tenir compte de la casse (ASCII seul : tous
// les verbes du protocole du harnais le sont).
static b32 verb_is(Str8 v, const char *name) {
    Str8 n = str8_from_cstr(name);
    if (v.len != n.len) return 0;
    for (isize i = 0; i < v.len; i++) {
        u8 c = v.data[i];
        if (c >= 'A' && c <= 'Z') c = (u8)(c + 32);
        if (c != n.data[i]) return 0;
    }
    return 1;
}

b32 auto_parse(Str8 raw, AutoCmd *out) {
    memset(out, 0, sizeof(*out));
    Str8 line = str8_trim(raw);
    if (line.len == 0 || line.data[0] == '#') return 0;
    isize space = str8_find_char(line, ' ', 0);
    Str8 verb = line, rest = str8_lit("");
    if (space > 0) {
        verb = str8_sub(line, 0, space);
        rest = str8_trim(str8_sub(line, space + 1, -1));
    }
    if (verb_is(verb, "play")) {
        out->kind = AUTO_CMD_PLAY;
        return 1;
    }
    if (verb_is(verb, "pause")) {
        out->kind = AUTO_CMD_PAUSE;
        return 1;
    }
    if (verb_is(verb, "seek")) {
        f64 sec = 0;
        if (!str_to_f64(rest, &sec) || !f64_is_finite(sec)) return 0;
        out->kind = AUTO_CMD_SEEK;
        out->value = sec;
        return 1;
    }
    if (verb_is(verb, "ready")) {
        out->kind = AUTO_CMD_READY;
        out->flag = !(str8_eq_cstr(rest, "0") || verb_is(rest, "false"));
        return 1;
    }
    if (verb_is(verb, "unready")) {
        out->kind = AUTO_CMD_READY;
        out->flag = 0;
        return 1;
    }
    if (verb_is(verb, "chat")) {
        if (rest.len == 0) return 0;
        out->kind = AUTO_CMD_CHAT;
        out->text = rest;
        return 1;
    }
    if (verb_is(verb, "open")) {
        if (rest.len == 0) return 0;
        out->kind = AUTO_CMD_OPEN;
        out->text = rest;
        return 1;
    }
    if (verb_is(verb, "quit")) {
        out->kind = AUTO_CMD_QUIT;
        return 1;
    }
    return 0;
}

// -------------------------------------------------------------------- E/S ---

Str8 auto_read_text(Arena *a, Str8 path) {
    if (path.len == 0) return str8_lit("");
    TempArena t = temp_begin(a);
    u16 *w = utf8_to_utf16(a, path, NULL);
    // FILE_SHARE_WRITE : le script écrit dans ce fichier pendant qu'on le lit.
    HANDLE h = CreateFileW((LPCWSTR)w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    temp_end(t);
    if (h == INVALID_HANDLE_VALUE) return str8_lit("");
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > VS_AUTO_CMDS_MAX) {
        CloseHandle(h);
        return str8_lit("");
    }
    u8 *buf = arena_push_array(a, u8, (isize)size.QuadPart + 1);
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL);
    CloseHandle(h);
    if (!ok) return str8_lit("");
    return str8(buf, (isize)got);
}

b32 auto_write_atomic(Arena *scratch, Str8 path, Str8 content) {
    if (path.len == 0) return 0;
    TempArena t = temp_begin(scratch);
    Str8 tmp_path = str8_cat(scratch, path, str8_lit(".tmp"));
    u16 *wtmp = utf8_to_utf16(scratch, tmp_path, NULL);
    u16 *wdst = utf8_to_utf16(scratch, path, NULL);
    HANDLE h = CreateFileW((LPCWSTR)wtmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           NULL);
    b32 ok = 0;
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ok = WriteFile(h, content.data, (DWORD)content.len, &written, NULL) &&
             written == (DWORD)content.len;
        CloseHandle(h);
    }
    // Le renommage échoue si le script a le fichier d'état ouvert au même
    // instant (Windows ne permet pas de remplacer un fichier ouvert sans
    // FILE_SHARE_DELETE). Ce n'est pas une erreur, c'est une course : on
    // réessaie brièvement plutôt que de perdre la publication.
    for (int attempt = 0; ok && attempt < 5; attempt++) {
        if (MoveFileExW((LPCWSTR)wtmp, (LPCWSTR)wdst, MOVEFILE_REPLACE_EXISTING)) break;
        if (attempt == 4) ok = 0;
        else Sleep(20);
    }
    if (!ok) DeleteFileW((LPCWSTR)wtmp);
    temp_end(t);
    return ok;
}
