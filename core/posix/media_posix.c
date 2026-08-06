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

// ------------------------------------------------------- normalisation NFC ---
//
// LE point dur de la recherche de médias sur macOS (VS-031 §3, tranché par
// VS-033). Un même nom de fichier y circule sous DEUX formes canoniques :
// HFS+ impose la forme DÉCOMPOSÉE (« é » = « e » + U+0301), APFS conserve ce
// qu'on lui a donné, et un nom venu d'un participant Windows arrive composé
// (« é » = U+00E9). Comparés point de code par point de code, ces deux noms
// sont différents — alors que c'est le même fichier, et que le Finder les
// affiche à l'identique. C'est l'écart le plus PROBABLE en séance réelle : un
// film accentué envoyé par un ami ne se retrouvait pas chez soi.
//
// On compose donc (NFC) LES DEUX CÔTÉS de la comparaison, au vol, sans table
// Unicode importée ni CoreFoundation : une lettre ASCII suivie d'une marque
// combinante devient la précomposée Latin-1 correspondante — exactement la
// plage que fold_cp sait déjà replier. Ce qui n'est pas dans la table n'est pas
// composé et reste comparé tel quel : jamais de faux positif.
//
// Cette normalisation est propre à POSIX : c'est le système de fichiers qui la
// rend nécessaire, et `name_eq_ci` est justement la primitive que platform.h
// laisse à la plateforme (« la sémantique du système de fichiers »). Windows,
// dont NTFS stocke les noms tels quels (en pratique composés), garde
// CompareStringOrdinal, qui ne normalise pas non plus.

typedef struct {
    u32 base;      // lettre ASCII de base
    u32 mark;      // marque combinante suivante
    u32 composed;  // précomposée du supplément Latin-1
} ComposePair;

// Les cinq accents du français, plus tilde, rond en chef et cédille : tout le
// supplément Latin-1 décomposable. Au-delà (Latin étendu, grec, cyrillique) on
// ne compose pas, comme on ne replie pas la casse — même frontière assumée.
static const ComposePair g_compose[] = {
    // U+0300 accent grave
    {'A', 0x0300, 0xc0}, {'E', 0x0300, 0xc8}, {'I', 0x0300, 0xcc}, {'O', 0x0300, 0xd2},
    {'U', 0x0300, 0xd9}, {'a', 0x0300, 0xe0}, {'e', 0x0300, 0xe8}, {'i', 0x0300, 0xec},
    {'o', 0x0300, 0xf2}, {'u', 0x0300, 0xf9},
    // U+0301 accent aigu
    {'A', 0x0301, 0xc1}, {'E', 0x0301, 0xc9}, {'I', 0x0301, 0xcd}, {'O', 0x0301, 0xd3},
    {'U', 0x0301, 0xda}, {'Y', 0x0301, 0xdd}, {'a', 0x0301, 0xe1}, {'e', 0x0301, 0xe9},
    {'i', 0x0301, 0xed}, {'o', 0x0301, 0xf3}, {'u', 0x0301, 0xfa}, {'y', 0x0301, 0xfd},
    // U+0302 accent circonflexe
    {'A', 0x0302, 0xc2}, {'E', 0x0302, 0xca}, {'I', 0x0302, 0xce}, {'O', 0x0302, 0xd4},
    {'U', 0x0302, 0xdb}, {'a', 0x0302, 0xe2}, {'e', 0x0302, 0xea}, {'i', 0x0302, 0xee},
    {'o', 0x0302, 0xf4}, {'u', 0x0302, 0xfb},
    // U+0303 tilde
    {'A', 0x0303, 0xc3}, {'N', 0x0303, 0xd1}, {'O', 0x0303, 0xd5}, {'a', 0x0303, 0xe3},
    {'n', 0x0303, 0xf1}, {'o', 0x0303, 0xf5},
    // U+0308 tréma
    {'A', 0x0308, 0xc4}, {'E', 0x0308, 0xcb}, {'I', 0x0308, 0xcf}, {'O', 0x0308, 0xd6},
    {'U', 0x0308, 0xdc}, {'Y', 0x0308, 0x178}, {'a', 0x0308, 0xe4}, {'e', 0x0308, 0xeb},
    {'i', 0x0308, 0xef}, {'o', 0x0308, 0xf6}, {'u', 0x0308, 0xfc}, {'y', 0x0308, 0xff},
    // U+030A rond en chef
    {'A', 0x030a, 0xc5}, {'a', 0x030a, 0xe5},
    // U+0327 cédille
    {'C', 0x0327, 0xc7}, {'c', 0x0327, 0xe7},
};

// compose_pair renvoie la précomposée, ou 0 si la paire n'en a pas.
static u32 compose_pair(u32 base, u32 mark) {
    if (base >= 0x80) return 0;  // seule une base ASCII se compose ici
    for (isize i = 0; i < VS_ARRAY_COUNT(g_compose); i++) {
        if (g_compose[i].base == base && g_compose[i].mark == mark) return g_compose[i].composed;
    }
    return 0;
}

// next_cp lit le point de code suivant, marque combinante ABSORBÉE si elle
// compose. Une séquence UTF-8 invalide est rendue octet par octet, sans repli
// ni composition. `*i` avance de ce qui a été consommé.
static u32 next_cp(Str8 s, isize *i) {
    u32 cp = 0;
    isize n = utf8_decode(s, *i, &cp);
    if (n == 0) {
        cp = s.data[*i];
        n = 1;
    }
    *i += n;
    if (*i >= s.len) return cp;
    u32 mark = 0;
    isize m = utf8_decode(s, *i, &mark);
    if (m == 0) return cp;
    u32 composed = compose_pair(cp, mark);
    if (composed == 0) return cp;
    *i += m;
    return composed;
}

// dir_name_eq_ci compare deux noms point de code par point de code, après
// composition (NFC) puis repli de casse. La longueur se compare en points de
// code, jamais en octets.
static b32 dir_name_eq_ci(Arena *scratch, Str8 a, Str8 b) {
    VS_UNUSED(scratch);  // aucune conversion à héberger, contrairement à l'UTF-16
    isize i = 0, j = 0;
    while (i < a.len && j < b.len) {
        if (fold_cp(next_cp(a, &i)) != fold_cp(next_cp(b, &j))) return 0;
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
