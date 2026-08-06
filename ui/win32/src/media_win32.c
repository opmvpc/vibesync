// media_win32.c — moitié PLATEFORME de la recherche de médias (ADR-010,
// VS-030) : parcours FindFirstFileW, comparaison ordinale des noms et dossier
// par défaut. L'algorithme borné vit dans media_core.c et ne voit que
// l'UTF-8.
#include "media.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <string.h>

// ------------------------------------------------------- parcours FindFirst ---

// L'itérateur vit dans l'arène de l'appelant, et RECYCLE la même place pour
// chaque nom converti : `mark` est la position juste après la structure, où
// l'on revient à chaque entrée. Sans ça, 50 000 entrées laisseraient autant de
// conversions UTF-8 derrière elles.
struct VsDirIter {
    Arena *arena;
    isize mark;
    HANDLE h;
    WIN32_FIND_DATAW fd;
    b32 first;
    b32 done;
};

static VsDirIter *dir_open(Arena *a, Str8 dir) {
    VsDirIter *it = arena_push_struct(a, VsDirIter);
    it->arena = a;
    it->mark = arena_pos(a);
    Str8 pattern = str8_cat(a, dir, str8_lit("\\*"));
    u16 *w = utf8_to_utf16(a, pattern, NULL);
    it->h = FindFirstFileW((LPCWSTR)w, &it->fd);
    arena_pop_to(a, it->mark);
    if (it->h == INVALID_HANDLE_VALUE) return NULL;
    it->first = 1;
    return it;
}

static b32 dir_next(VsDirIter *it, VsDirEntry *out) {
    if (it->done) return 0;
    if (it->first) it->first = 0;
    else if (!FindNextFileW(it->h, &it->fd)) {
        it->done = 1;
        return 0;
    }
    arena_pop_to(it->arena, it->mark);
    memset(out, 0, sizeof(*out));
    out->name = utf16_to_utf8(it->arena, (const u16 *)it->fd.cFileName);
    out->is_dir = (it->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    out->is_link = (it->fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    out->size_bytes = ((i64)it->fd.nFileSizeHigh << 32) | (i64)it->fd.nFileSizeLow;
    return 1;
}

static void dir_close(VsDirIter *it) {
    if (it->h != INVALID_HANDLE_VALUE) FindClose(it->h);
    it->h = INVALID_HANDLE_VALUE;
    it->done = 1;
}

// dir_name_eq_ci : comparaison ordinale insensible à la casse, pas de
// dépendance à la locale, contrairement à lstrcmpiW. La comparaison se fait en
// UTF-16 — c'est la sémantique du système de fichiers Windows, et la longueur
// se compare en unités UTF-16, jamais en octets.
static b32 dir_name_eq_ci(Arena *scratch, Str8 a, Str8 b) {
    TempArena t = temp_begin(scratch);
    isize alen = 0, blen = 0;
    u16 *wa = utf8_to_utf16(scratch, a, &alen);
    u16 *wb = utf8_to_utf16(scratch, b, &blen);
    b32 eq = 0;
    if (alen == blen) {
        eq = CompareStringOrdinal((LPCWCH)wa, (int)alen, (LPCWCH)wb, (int)blen, TRUE) == CSTR_EQUAL;
    }
    temp_end(t);
    return eq;
}

static const VsDirOps g_dir_ops = {dir_open, dir_next, dir_close, dir_name_eq_ci, '\\'};

const VsDirOps *vs_dir_ops(void) { return &g_dir_ops; }

b32 media_find(Arena *scratch, const StrBuf *dirs, isize dir_count, Str8 name, MediaFind *out) {
    return media_find_with(scratch, vs_dir_ops(), dirs, dir_count, name, out);
}

// ------------------------------------------------------------ dossier défaut ---

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
