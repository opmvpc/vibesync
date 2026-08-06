#include "media.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <string.h>

// Tampon de chemin partagé par toute la descente : on empile et dépile un
// suffixe plutôt que de réallouer à chaque niveau. 32 768 unités = la limite
// des chemins longs Windows (le manifeste les active).
#define MEDIA_PATH_CAP 32768

typedef struct {
    u16 *path;  // chemin courant, terminé par 0
    isize len;
    const u16 *needle;
    isize needle_len;

    isize visited;
    b32 truncated;

    b32 found;
    u16 *best;
    isize best_len;
    i64 best_size;
    isize matches;
} FindCtx;

static b32 name_eq_ci(const u16 *a, isize alen, const u16 *b, isize blen) {
    if (alen != blen) return 0;
    // Comparaison ordinale insensible à la casse : pas de dépendance à la
    // locale, contrairement à lstrcmpiW.
    return CompareStringOrdinal((LPCWCH)a, (int)alen, (LPCWCH)b, (int)blen, TRUE) == CSTR_EQUAL;
}

static isize wlen(const u16 *s) {
    isize n = 0;
    while (s[n]) n++;
    return n;
}

// push_component ajoute « \nom » au chemin courant. 0 si ça déborde.
static b32 push_component(FindCtx *c, const u16 *name, isize name_len) {
    if (c->len + name_len + 2 >= MEDIA_PATH_CAP) return 0;
    c->path[c->len++] = '\\';
    memcpy(c->path + c->len, name, (size_t)name_len * sizeof(u16));
    c->len += name_len;
    c->path[c->len] = 0;
    return 1;
}

static void pop_to(FindCtx *c, isize len) {
    c->len = len;
    c->path[len] = 0;
}

// consider retient un candidat : à nom égal, le plus gros fichier gagne.
static void consider(FindCtx *c, i64 size) {
    c->matches++;
    if (c->found && size <= c->best_size) return;
    memcpy(c->best, c->path, (size_t)(c->len + 1) * sizeof(u16));
    c->best_len = c->len;
    c->best_size = size;
    c->found = 1;
}

static void scan(FindCtx *c, isize depth) {
    if (c->truncated || depth > MEDIA_MAX_DEPTH) return;
    isize base = c->len;
    if (!push_component(c, (const u16 *)L"*", 1)) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((LPCWSTR)c->path, &fd);
    pop_to(c, base);
    if (h == INVALID_HANDLE_VALUE) return;  // dossier illisible : on passe

    do {
        if (c->visited >= MEDIA_MAX_ENTRIES) {
            c->truncated = 1;
            break;
        }
        c->visited++;
        const u16 *name = (const u16 *)fd.cFileName;
        if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
        // Jonctions et liens : ignorés, sinon une boucle de répertoires
        // ferait tourner la recherche jusqu'à la borne d'entrées.
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        isize nlen = wlen(name);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth >= MEDIA_MAX_DEPTH) continue;  // profondeur bornée
            if (!push_component(c, name, nlen)) continue;
            scan(c, depth + 1);
            pop_to(c, base);
            if (c->truncated) break;
            continue;
        }
        if (name_eq_ci(name, nlen, c->needle, c->needle_len)) {
            if (!push_component(c, name, nlen)) continue;
            consider(c, ((i64)fd.nFileSizeHigh << 32) | (i64)fd.nFileSizeLow);
            pop_to(c, base);
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    pop_to(c, base);
}

b32 media_find(Arena *scratch, const StrBuf *dirs, isize dir_count, Str8 name, MediaFind *out) {
    memset(out, 0, sizeof(*out));
    if (name.len == 0 || dir_count <= 0) return 0;

    TempArena t = temp_begin(scratch);
    FindCtx c;
    memset(&c, 0, sizeof(c));
    c.path = arena_push_array(scratch, u16, MEDIA_PATH_CAP);
    c.best = arena_push_array(scratch, u16, MEDIA_PATH_CAP);
    isize needle_len = 0;
    c.needle = utf8_to_utf16(scratch, name, &needle_len);
    c.needle_len = needle_len;

    for (isize i = 0; i < dir_count && !c.truncated; i++) {
        Str8 dir = strbuf_str(&dirs[i]);
        if (dir.len == 0) continue;
        isize dlen = 0;
        u16 *w = utf8_to_utf16(scratch, dir, &dlen);
        if (dlen == 0 || dlen + 2 >= MEDIA_PATH_CAP) continue;
        // Une barre finale ferait « C:\a\\* » : on la retire.
        while (dlen > 1 && (w[dlen - 1] == '\\' || w[dlen - 1] == '/')) dlen--;
        memcpy(c.path, w, (size_t)dlen * sizeof(u16));
        c.len = dlen;
        c.path[c.len] = 0;
        scan(&c, 1);
    }

    out->visited = c.visited;
    out->truncated = c.truncated;
    out->matches = c.matches;
    if (c.found) {
        out->found = 1;
        out->size_bytes = c.best_size;
        c.best[c.best_len] = 0;
        strbuf_set(&out->path, utf16_to_utf8(scratch, c.best));
    }
    temp_end(t);
    return out->found;
}

b32 media_default_dir(Arena *a, Str8 *out) {
    // FOLDERID_Downloads — la constante n'est pas exportée par tous les
    // en-têtes mingw, on la pose ici.
    static const GUID downloads = {
        0x374DE290, 0x123F, 0x4565, {0x91, 0x64, 0x39, 0xC4, 0x92, 0x5E, 0x46, 0x7B}};
    PWSTR wpath = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&downloads, 0, NULL, &wpath)) && wpath) {
        *out = str8_copy(a, utf16_to_utf8(a, (const u16 *)wpath));
        CoTaskMemFree(wpath);
        return out->len > 0;
    }
    if (wpath) CoTaskMemFree(wpath);
    // Repli : %USERPROFILE%\Downloads.
    u16 var[] = {'U', 'S', 'E', 'R', 'P', 'R', 'O', 'F', 'I', 'L', 'E', 0};
    DWORD n = GetEnvironmentVariableW((LPCWSTR)var, NULL, 0);
    if (n == 0) return 0;
    TempArena t = temp_begin(a);
    u16 *buf = arena_push_array(a, u16, (isize)n + 1);
    DWORD got = GetEnvironmentVariableW((LPCWSTR)var, (LPWSTR)buf, n);
    Str8 home = (got > 0 && got <= n) ? utf16_to_utf8(a, buf) : str8_lit("");
    temp_end(t);
    if (home.len == 0) return 0;
    *out = str8_cat(a, str8_copy(a, home), str8_lit("\\Downloads"));
    return 1;
}

Str8 media_dirs_join(Arena *a, const StrBuf *dirs, isize count) {
    Builder b;
    builder_init(&b, a, 256);
    isize written = 0;
    for (isize i = 0; i < count; i++) {
        Str8 d = strbuf_str(&dirs[i]);
        if (d.len == 0) continue;
        if (written > 0) builder_byte(&b, '|');
        builder_str(&b, d);
        written++;
    }
    return builder_result(&b);
}

isize media_dirs_split(Str8 packed, StrBuf *out, isize max) {
    isize n = 0, start = 0;
    for (isize i = 0; i <= packed.len && n < max; i++) {
        if (i < packed.len && packed.data[i] != '|') continue;
        Str8 part = str8_trim(str8_sub(packed, start, i - start));
        start = i + 1;
        if (part.len == 0) continue;
        strbuf_set(&out[n++], part);
    }
    return n;
}
