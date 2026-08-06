// media_posix.c — moitié PLATEFORME de la recherche de médias pour macOS/POSIX
// (ADR-010, VS-031) : parcours opendir/readdir, comparaison des noms sans
// tenir compte de la casse, dossier par défaut. L'algorithme borné vit dans
// media_core.c et ne voit que l'UTF-8.
//
// Pendant de media_win32.c ; les deux respectent le contrat de platform.h :
// les entrées sont rendues BRUTES (« . » et « .. » compris) pour que le
// compteur `visited` soit le même des deux côtés.
#include "media.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// ------------------------------------------------------- parcours opendir ---

// Comme sous Windows, l'itérateur vit dans l'arène de l'appelant et RECYCLE la
// même place à chaque entrée : `mark` est posée après la structure ET après la
// copie du chemin du dossier (qui, elle, doit survivre à toute l'énumération,
// puisqu'elle sert à composer le chemin passé à lstat).
struct VsDirIter {
    Arena *arena;
    isize mark;
    Str8 dir;
    DIR *d;
};

static VsDirIter *dir_open(Arena *a, Str8 dir) {
    VsDirIter *it = arena_push_struct(a, VsDirIter);
    it->arena = a;
    it->dir = str8_copy(a, dir);
    it->mark = arena_pos(a);
    char *c = str8_cstr(a, dir);
    it->d = opendir(c);
    arena_pop_to(a, it->mark);
    if (!it->d) return NULL;
    return it;
}

static b32 dir_next(VsDirIter *it, VsDirEntry *out) {
    if (!it->d) return 0;
    arena_pop_to(it->arena, it->mark);
    struct dirent *de = readdir(it->d);
    if (!de) return 0;
    memset(out, 0, sizeof(*out));
    out->name = str8_copy(it->arena, str8_from_cstr(de->d_name));

    // d_type suffit pour les dossiers et les liens (APFS et HFS+ le
    // renseignent) ; la taille, elle, exige un lstat — qu'on ne paie donc que
    // pour les fichiers ordinaires et les entrées de type inconnu (systèmes de
    // fichiers réseau).
    b32 need_stat = 1;
    switch (de->d_type) {
        case DT_DIR:
            out->is_dir = 1;
            need_stat = 0;
            break;
        case DT_LNK:
            // lstat ne suit pas le lien : is_link vaut le FILE_ATTRIBUTE_
            // REPARSE_POINT de Windows, et media_core.c ne le suit jamais.
            out->is_link = 1;
            need_stat = 0;
            break;
        default: break;
    }
    if (need_stat) {
        TempArena t = temp_begin(it->arena);
        u8 sep = '/';
        Str8 full = str8_cat(it->arena, str8_cat(it->arena, it->dir, str8(&sep, 1)), out->name);
        char *path = str8_cstr(it->arena, full);
        struct stat st;
        if (lstat(path, &st) == 0) {
            out->is_dir = S_ISDIR(st.st_mode) != 0;
            out->is_link = S_ISLNK(st.st_mode) != 0;
            if (!out->is_dir && !out->is_link) out->size_bytes = (i64)st.st_size;
        }
        temp_end(t);
    }
    return 1;
}

static void dir_close(VsDirIter *it) {
    if (it->d) closedir(it->d);
    it->d = NULL;
}

// fold_cp : repli de casse SIMPLE, limité à l'ASCII et au supplément Latin-1.
//
// Sous Windows la comparaison est faite par CompareStringOrdinal en UTF-16,
// c'est-à-dire un repli Unicode complet. Ici il n'y a pas d'équivalent sans
// table (et ADR-008 interdit d'en importer une) : on couvre ce qui compte pour
// des noms de fichiers francophones — a-z et À-Þ ↔ à-þ. Les écarts possibles
// sont documentés dans docs/research/2026-08-06-vs031-vscore-spm.md.
static u32 fold_cp(u32 cp) {
    if (cp < 0x80) return (cp >= 'A' && cp <= 'Z') ? cp + 32 : cp;
    if (cp >= 0xc0 && cp <= 0xde && cp != 0xd7) return cp + 32;  // × (0xd7) n'est pas une lettre
    return cp;
}

// dir_name_eq_ci compare deux noms point de code par point de code, après
// repli de casse. La longueur se compare en points de code, jamais en octets.
static b32 dir_name_eq_ci(Arena *scratch, Str8 a, Str8 b) {
    VS_UNUSED(scratch);  // aucune conversion à héberger, contrairement à l'UTF-16
    isize i = 0, j = 0;
    while (i < a.len && j < b.len) {
        u32 ca = 0, cb = 0;
        isize na = utf8_decode(a, i, &ca);
        isize nb = utf8_decode(b, j, &cb);
        if (na == 0) {  // séquence invalide : comparaison octet à octet, sans repli
            ca = a.data[i];
            na = 1;
        }
        if (nb == 0) {
            cb = b.data[j];
            nb = 1;
        }
        if (fold_cp(ca) != fold_cp(cb)) return 0;
        i += na;
        j += nb;
    }
    return i >= a.len && j >= b.len;
}

static const VsDirOps g_dir_ops = {dir_open, dir_next, dir_close, dir_name_eq_ci, '/'};

const VsDirOps *vs_dir_ops(void) { return &g_dir_ops; }

b32 media_find(Arena *scratch, const StrBuf *dirs, isize dir_count, Str8 name, MediaFind *out) {
    return media_find_with(scratch, vs_dir_ops(), dirs, dir_count, name, out);
}

// ------------------------------------------------------------ dossier défaut ---

// dir_exists : un dossier proposé par défaut doit exister, sinon la boîte de
// réglages s'ouvre sur un chemin mort.
static b32 dir_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

b32 media_default_dir(Arena *a, Str8 *out) {
    const char *home = getenv("HOME");
    if (!home || !*home) return 0;
    // Même choix que sous Windows (FOLDERID_Downloads) : c'est là qu'arrive un
    // fichier partagé entre amis. Repli sur ~/Movies, dossier standard de macOS.
    static const char *const names[] = {"/Downloads", "/Movies"};
    char path[1024];
    for (isize i = 0; i < VS_ARRAY_COUNT(names); i++) {
        int n = snprintf(path, sizeof(path), "%s%s", home, names[i]);
        if (n <= 0 || n >= (int)sizeof(path)) continue;
        if (!dir_exists(path)) continue;
        *out = str8_copy(a, str8_from_cstr(path));
        return out->len > 0;
    }
    *out = str8_copy(a, str8_from_cstr(home));
    return out->len > 0;
}
