// media.h — dossiers médias et recherche du fichier déclaré par un participant.
//
// Les noms de fichiers circulent déjà dans `setFile`/`users` : il suffit de
// retrouver le même nom chez soi. La recherche est récursive mais BORNÉE
// (profondeur et nombre d'entrées) et bloquante : elle vit sur un thread
// dédié, jamais sur le thread UI — une arborescence réseau peut prendre des
// secondes et la fenêtre ne doit pas geler.
#ifndef VS_MEDIA_H
#define VS_MEDIA_H

#include "base.h"
#include "platform.h"

#define MEDIA_MAX_DIRS 8
// Bornes de sécurité : au-delà, on rend ce qu'on a trouvé et on le dit.
#define MEDIA_MAX_DEPTH 6
#define MEDIA_MAX_ENTRIES 50000

typedef struct {
    b32 found;
    StrBuf path;     // chemin complet du meilleur candidat
    i64 size_bytes;  // taille du candidat retenu
    isize matches;   // correspondances vues (> 1 = homonymes)
    isize visited;   // entrées parcourues (diagnostic et tests de bornes)
    b32 truncated;   // borne d'entrées atteinte : résultat possiblement partiel
} MediaFind;

// media_find cherche le nom EXACT (comparaison insensible à la casse) dans les
// dossiers donnés. En cas d'homonymes, le plus gros fichier gagne : entre un
// extrait et le film complet, c'est le film qu'on veut (heuristique assumée,
// le nombre de correspondances est rendu pour pouvoir le tracer).
// Les points de reparse (jonctions, liens) sont ignorés : pas de boucle.
b32 media_find(Arena *scratch, const StrBuf *dirs, isize dir_count, Str8 name, MediaFind *out);

// media_find_with est la même recherche, sur une primitive de parcours donnée
// (media_core.c, portable). media_find n'est que ce parcours-là appliqué à
// celui de la plateforme : c'est le point d'extraction d'ADR-010, et ce qui
// permet de tester l'algorithme borné sans toucher au disque.
b32 media_find_with(Arena *scratch, const VsDirOps *ops, const StrBuf *dirs, isize dir_count, Str8 name,
                    MediaFind *out);

// media_default_dir renvoie le dossier Téléchargements de l'utilisateur :
// FOLDERID_Downloads (repli %USERPROFILE%\Downloads) sous Windows,
// ~/Downloads (repli ~/Movies puis ~) sous macOS.
b32 media_default_dir(Arena *a, Str8 *out);

// --- persistance dans vibesync.ini ---
//
// Les chemins sont joints par « | », caractère interdit dans un chemin Windows :
// pas d'échappement, pas d'ambiguïté.
Str8 media_dirs_join(Arena *a, const StrBuf *dirs, isize count);
// media_dirs_split renvoie le nombre de dossiers écrits (les entrées vides sont
// ignorées, le surplus au-delà de `max` est abandonné).
isize media_dirs_split(Str8 packed, StrBuf *out, isize max);

#endif // VS_MEDIA_H
