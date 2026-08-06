// ini_core.c — moitié PORTABLE des réglages (ADR-010, VS-030).
//
// Analyse, lecture, écriture et éviction d'un contenu déjà en mémoire : aucun
// accès au disque, aucun chemin de plateforme. Le fichier lui-même
// (%APPDATA%\vibesync.ini) est l'affaire d'ini_win32.c.
#include "ini.h"

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

b32 ini_remove_at(Ini *ini, isize i) {
    if (i < 0 || i >= ini->count) return 0;
    for (isize j = i; j + 1 < ini->count; j++) ini->entries[j] = ini->entries[j + 1];
    ini->count--;
    return 1;
}

b32 ini_remove(Ini *ini, const char *key) {
    Str8 k = str8_from_cstr(key);
    for (isize i = 0; i < ini->count; i++) {
        if (str8_eq(ini->entries[i].key, k)) return ini_remove_at(ini, i);
    }
    return 0;
}

b32 ini_make_room(Ini *ini, const char *const *keep, isize keep_count, Str8 *out_key) {
    // De la fin vers le début : la dernière entrée inconnue est la plus
    // récemment ajoutée à la main, donc la moins précieuse.
    for (isize i = ini->count - 1; i >= 0; i--) {
        b32 keep_it = 0;
        for (isize k = 0; k < keep_count; k++) {
            if (str8_eq_cstr(ini->entries[i].key, keep[k])) {
                keep_it = 1;
                break;
            }
        }
        if (keep_it) continue;
        // La Str8 est copiée AVANT le décalage ; ses octets vivent dans l'arène
        // et ne bougent pas, seule la table d'entrées est réarrangée.
        if (out_key) *out_key = ini->entries[i].key;
        return ini_remove_at(ini, i);
    }
    return 0;
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
