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

b32 ini_save_file(Arena *scratch, Str8 path, Str8 content) {
    if (path.len == 0) return 0;
    TempArena t = temp_begin(scratch);
    u16 *w = utf8_to_utf16(scratch, path, NULL);
    HANDLE h = CreateFileW((LPCWSTR)w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    temp_end(t);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD written = 0;
    BOOL ok = WriteFile(h, content.data, (DWORD)content.len, &written, NULL);
    CloseHandle(h);
    return ok && written == (DWORD)content.len;
}
