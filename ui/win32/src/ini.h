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
// ini_remove supprime une clé si elle existe. Renvoie 1 si quelque chose a été
// retiré. Sert à effacer un secret mémorisé : la clé doit DISPARAÎTRE du
// fichier, pas se retrouver avec une valeur vide.
b32 ini_remove(Ini *ini, const char *key);
// ini_remove_at supprime l'entrée d'indice `i` (les suivantes remontent).
// Renvoie 0 si l'indice est hors bornes.
b32 ini_remove_at(Ini *ini, isize i);
// ini_make_room évince la DERNIÈRE entrée dont la clé ne figure pas dans
// `keep`, pour qu'une clé indispensable puisse encore être écrite dans un
// fichier saturé (INI_MAX_ENTRIES). Sans elle, un vibesync.ini bricolé à la
// main bloquerait SILENCIEUSEMENT l'écriture du jeton de session — c'est-à-dire
// rendrait le bug VS-028 incorrigible. `out_key` (optionnel) reçoit la clé
// évincée ; sa mémoire vit dans l'arène et reste lisible après l'éviction.
// Renvoie 0 s'il n'y a rien à évincer (tout est à garder).
b32 ini_make_room(Ini *ini, const char *const *keep, isize keep_count, Str8 *out_key);
// ini_write sérialise, prêt à être écrit sur disque (lignes CRLF).
Str8 ini_write(Arena *a, const Ini *ini);

// ini_path renvoie %APPDATA%\vibesync.ini (chaîne vide si introuvable).
Str8 ini_path(Arena *a);
b32 ini_load_file(Arena *a, Str8 path, Ini *out);
b32 ini_save_file(Arena *scratch, Str8 path, Str8 content);

#endif // VS_INI_H
