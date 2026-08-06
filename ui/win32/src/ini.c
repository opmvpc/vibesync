#include "ini.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string.h>

void ini_clear(Ini *ini) { ini->count = 0; }

// find renvoie l'entrée existante pour une clé, ou NULL.
static IniEntry *find(Ini *ini, Str8 key) {
    for (isize i = 0; i < ini->count; i++) {
        if (str8_eq(ini->entries[i].key, key)) return &ini->entries[i];
    }
    return NULL;
}

static b32 put(Arena *a, Ini *ini, Str8 key, Str8 value) {
    if (key.len == 0) return 1;
    IniEntry *e = find(ini, key);
    if (e) {  // clé répétée : la dernière gagne
        e->value = str8_copy(a, value);
        return 1;
    }
    if (ini->count >= INI_MAX_ENTRIES) return 0;
    e = &ini->entries[ini->count++];
    e->key = str8_copy(a, key);
    e->value = str8_copy(a, value);
    return 1;
}

b32 ini_parse(Arena *a, Str8 text, Ini *out) {
    ini_clear(out);
    b32 complete = 1;
    isize i = 0;
    while (i < text.len) {
        isize eol = str8_find_char(text, '\n', i);
        if (eol < 0) eol = text.len;
        Str8 line = str8_trim(str8_sub(text, i, eol - i));
        i = eol + 1;
        if (line.len == 0) continue;
        if (line.data[0] == '#' || line.data[0] == ';') continue;
        if (line.data[0] == '[') continue;  // section : tolérée, ignorée
        isize eq = str8_find_char(line, '=', 0);
        if (eq <= 0) continue;
        Str8 key = str8_trim(str8_sub(line, 0, eq));
        Str8 value = str8_trim(str8_sub(line, eq + 1, -1));
        if (!put(a, out, key, value)) complete = 0;
    }
    return complete;
}

Str8 ini_get(const Ini *ini, const char *key, Str8 def) {
    Str8 k = str8_from_cstr(key);
    for (isize i = 0; i < ini->count; i++) {
        if (str8_eq(ini->entries[i].key, k)) return ini->entries[i].value;
    }
    return def;
}

b32 ini_set(Arena *a, Ini *ini, const char *key, Str8 value) {
    return put(a, ini, str8_from_cstr(key), value);
}

Str8 ini_write(Arena *a, const Ini *ini) {
    Builder b;
    builder_init(&b, a, 512);
    builder_cstr(&b, "# vibesync — réglages du client Windows\r\n");
    for (isize i = 0; i < ini->count; i++) {
        builder_str(&b, ini->entries[i].key);
        builder_byte(&b, '=');
        builder_str(&b, ini->entries[i].value);
        builder_cstr(&b, "\r\n");
    }
    return builder_result(&b);
}

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
