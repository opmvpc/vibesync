// media_core.c — moitié PORTABLE de la recherche de médias (ADR-010, VS-030).
//
// L'algorithme borné (profondeur, nombre d'entrées, homonyme le plus gros) ne
// connaît du système que la primitive de parcours `VsDirOps` (platform.h) :
// il ne manipule que de l'UTF-8. Les implémentations du parcours et les
// dossiers par défaut sont ailleurs : ui/win32/src/media_win32.c
// (FindFirstFileW) et core/posix/media_posix.c (opendir/readdir).
#include "media.h"

#include <string.h>

// Tampon de chemin partagé par toute la descente : on empile et dépile un
// suffixe plutôt que de réallouer à chaque niveau. 32 768 OCTETS — la limite
// des chemins longs Windows est de 32 768 unités UTF-16 (le manifeste les
// active) ; en UTF-8 la borne est donc conservatrice pour un chemin non ASCII,
// et strictement identique pour les chemins ASCII.
#define MEDIA_PATH_CAP 32768

typedef struct {
    Arena *scratch;
    const VsDirOps *ops;

    u8 *path;  // chemin courant, terminé par 0
    isize len;
    Str8 needle;

    isize visited;
    b32 truncated;

    b32 found;
    u8 *best;
    isize best_len;
    i64 best_size;
    isize matches;
} FindCtx;

// push_component ajoute « <sep>nom » au chemin courant. 0 si ça déborde.
static b32 push_component(FindCtx *c, Str8 name) {
    if (c->len + name.len + 2 >= MEDIA_PATH_CAP) return 0;
    c->path[c->len++] = c->ops->sep;
    memcpy(c->path + c->len, name.data, (size_t)name.len);
    c->len += name.len;
    c->path[c->len] = 0;
    return 1;
}

static void pop_to(FindCtx *c, isize len) {
    c->len = len;
    c->path[len] = 0;
}

// is_dot reconnaît « . » et « .. », les deux entrées que tout parcours rend et
// qu'il ne faut jamais suivre.
static b32 is_dot(Str8 name) {
    if (name.len == 1 && name.data[0] == '.') return 1;
    return name.len == 2 && name.data[0] == '.' && name.data[1] == '.';
}

// consider retient un candidat : à nom égal, le plus gros fichier gagne.
static void consider(FindCtx *c, i64 size) {
    c->matches++;
    if (c->found && size <= c->best_size) return;
    memcpy(c->best, c->path, (size_t)(c->len + 1));
    c->best_len = c->len;
    c->best_size = size;
    c->found = 1;
}

static void scan(FindCtx *c, isize depth) {
    if (c->truncated || depth > MEDIA_MAX_DEPTH) return;
    isize base = c->len;
    // Même garde qu'avant le découpage : l'implémentation ajoute un motif de
    // deux unités (« \* ») au chemin, un dossier trop profond est abandonné.
    if (c->len + 3 >= MEDIA_PATH_CAP) return;

    // Portée temporaire par dossier : l'itérateur et les noms qu'il rend
    // vivent dans l'arène, la descente en garde au plus MEDIA_MAX_DEPTH.
    TempArena t = temp_begin(c->scratch);
    VsDirIter *it = c->ops->open(c->scratch, str8(c->path, c->len));
    if (!it) {  // dossier illisible : on passe
        temp_end(t);
        return;
    }

    VsDirEntry e;
    while (c->ops->next(it, &e)) {
        if (c->visited >= MEDIA_MAX_ENTRIES) {
            c->truncated = 1;
            break;
        }
        c->visited++;
        if (is_dot(e.name)) continue;
        // Jonctions et liens : ignorés, sinon une boucle de répertoires
        // ferait tourner la recherche jusqu'à la borne d'entrées.
        if (e.is_link) continue;

        if (e.is_dir) {
            if (depth >= MEDIA_MAX_DEPTH) continue;  // profondeur bornée
            if (!push_component(c, e.name)) continue;
            scan(c, depth + 1);
            pop_to(c, base);
            if (c->truncated) break;
            continue;
        }
        if (c->ops->name_eq_ci(c->scratch, e.name, c->needle)) {
            if (!push_component(c, e.name)) continue;
            consider(c, e.size_bytes);
            pop_to(c, base);
        }
    }

    c->ops->close(it);
    pop_to(c, base);
    temp_end(t);
}

b32 media_find_with(Arena *scratch, const VsDirOps *ops, const StrBuf *dirs, isize dir_count, Str8 name,
                    MediaFind *out) {
    memset(out, 0, sizeof(*out));
    if (name.len == 0 || dir_count <= 0) return 0;

    TempArena t = temp_begin(scratch);
    FindCtx c;
    memset(&c, 0, sizeof(c));
    c.scratch = scratch;
    c.ops = ops;
    c.path = arena_push_array(scratch, u8, MEDIA_PATH_CAP);
    c.best = arena_push_array(scratch, u8, MEDIA_PATH_CAP);
    c.needle = name;

    for (isize i = 0; i < dir_count && !c.truncated; i++) {
        Str8 dir = strbuf_str(&dirs[i]);
        if (dir.len == 0) continue;
        isize dlen = dir.len;
        if (dlen == 0 || dlen + 2 >= MEDIA_PATH_CAP) continue;
        // Une barre finale ferait « C:\a\\* » : on la retire.
        while (dlen > 1 && (dir.data[dlen - 1] == '\\' || dir.data[dlen - 1] == '/')) dlen--;
        memcpy(c.path, dir.data, (size_t)dlen);
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
        strbuf_set(&out->path, str8(c.best, c.best_len));
    }
    temp_end(t);
    return out->found;
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
