// ini_win32.c — moitié PLATEFORME des réglages (ADR-010, VS-030).
//
// Emplacement du fichier (%APPDATA%, en UTF-16) et E/S disque. Le format et
// toute la logique d'édition sont dans ini_core.c.
#include "ini.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string.h>

Str8 ini_path(Arena *a) {
    u16 wname[] = {'A', 'P', 'P', 'D', 'A', 'T', 'A', 0};
    DWORD n = GetEnvironmentVariableW((LPCWSTR)wname, NULL, 0);
    if (n == 0) return str8_lit("");
    u16 *buf = arena_push_array(a, u16, (isize)n + 1);
    DWORD got = GetEnvironmentVariableW((LPCWSTR)wname, (LPWSTR)buf, n);
    if (got == 0 || got > n) return str8_lit("");
    return str8_cat(a, utf16_to_utf8(a, buf), str8_lit("\\vibesync.ini"));
}

b32 ini_load_file(Arena *a, Str8 path, Ini *out) {
    ini_clear(out);
    if (path.len == 0) return 0;
    TempArena t = temp_begin(a);
    u16 *w = utf8_to_utf16(a, path, NULL);
    HANDLE h = CreateFileW((LPCWSTR)w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    temp_end(t);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > VS_MB(1)) {
        CloseHandle(h);
        return 0;
    }
    u8 *buf = arena_push_array(a, u8, (isize)size.QuadPart + 1);
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL);
    CloseHandle(h);
    if (!ok) return 0;
    // Un BOM UTF-8 éventuel (fichier édité au Bloc-notes) est sauté.
    Str8 text = str8(buf, (isize)got);
    if (text.len >= 3 && text.data[0] == 0xef && text.data[1] == 0xbb && text.data[2] == 0xbf) {
        text = str8_sub(text, 3, -1);
    }
    return ini_parse(a, text, out);
}

// ini_save_file écrit le fichier de réglages. Un échec est RARE mais réel chez
// l'utilisateur — disque plein (112), profil aux droits bricolés (5), antivirus
// ou éditeur qui garde le fichier ouvert (32) — et il faisait perdre réglages
// et jeton de session sans laisser de trace. Le code Win32 est journalisé ici,
// au seul endroit qui le connaisse ; la décision de prévenir l'utilisateur
// appartient à l'appelant (ini_flush_notify dans main.c).
b32 ini_save_file(Arena *scratch, Str8 path, Str8 content) {
    if (path.len == 0) {
        vs_log("ini: chemin de vibesync.ini inconnu (%%APPDATA%% introuvable), réglages non écrits");
        return 0;
    }
    TempArena t = temp_begin(scratch);
    u16 *w = utf8_to_utf16(scratch, path, NULL);
    HANDLE h = CreateFileW((LPCWSTR)w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD err = GetLastError();  // lu AVANT temp_end : rien ne doit s'intercaler
    temp_end(t);
    if (h == INVALID_HANDLE_VALUE) {
        vs_log("ini: ouverture en écriture refusée (erreur Win32 %lu) — \"%.*s\"", (unsigned long)err,
               (int)path.len, (const char *)path.data);
        return 0;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(h, content.data, (DWORD)content.len, &written, NULL);
    err = GetLastError();
    CloseHandle(h);
    if (!ok || written != (DWORD)content.len) {
        // Écriture partielle : le CREATE_ALWAYS a déjà tronqué le fichier, donc
        // ce cas laisse un vibesync.ini incomplet. Le dire est le minimum.
        vs_log("ini: écriture incomplète (%lu/%lld octets, erreur Win32 %lu) — \"%.*s\"",
               (unsigned long)written, (long long)content.len, (unsigned long)err, (int)path.len,
               (const char *)path.data);
        return 0;
    }
    return 1;
}
