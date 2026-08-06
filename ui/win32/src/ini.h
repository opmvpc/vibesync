// ini.h — réglages persistants dans %APPDATA%\vibesync.ini.
//
// Format maison volontairement trivial : une ligne « clé=valeur » par réglage,
// UTF-8, commentaires « # » ou « ; », en-têtes de section tolérés et ignorés.
// Tout vit dans une arène : ni malloc, ni dépendance.
#ifndef VS_INI_H
#define VS_INI_H

#include "base.h"

#define INI_MAX_ENTRIES 64

typedef struct {
    Str8 key;
    Str8 value;
} IniEntry;

typedef struct {
    IniEntry entries[INI_MAX_ENTRIES];
    isize count;
} Ini;

void ini_clear(Ini *ini);
// ini_parse lit un contenu en mémoire. Les clés/valeurs sont copiées dans `a`.
// Une clé répétée écrase la précédente (la dernière gagne). Renvoie 0 si des
// entrées ont été ignorées faute de place.
b32 ini_parse(Arena *a, Str8 text, Ini *out);
Str8 ini_get(const Ini *ini, const char *key, Str8 def);
// ini_set ajoute ou remplace une valeur (copiée dans `a`).
b32 ini_set(Arena *a, Ini *ini, const char *key, Str8 value);
// ini_write sérialise, prêt à être écrit sur disque (lignes CRLF).
Str8 ini_write(Arena *a, const Ini *ini);

// ini_path renvoie %APPDATA%\vibesync.ini (chaîne vide si introuvable).
Str8 ini_path(Arena *a);
b32 ini_load_file(Arena *a, Str8 path, Ini *out);
b32 ini_save_file(Arena *scratch, Str8 path, Str8 content);

#endif // VS_INI_H
