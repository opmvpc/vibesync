// platform.h — frontière entre le C portable et l'implémentation par système.
//
// ADR-010 : les fichiers `*_core.c` sont du C11 pur (aucun <windows.h>, aucun
// wchar_t, tout texte en UTF-8) et seront extraits tels quels dans la couche
// commune win32/macOS. Tout ce dont ils ont besoin du système passe par les
// prototypes déclarés ici (et par la poignée de primitives de base.h :
// arènes, horloge, aléa, journal), implémentés une fois par plateforme :
//
//   Win32  : ui/win32/src/{base,media,vlc,ini}_win32.c
//   POSIX  : core/posix/{base,media}_posix.c (macOS, VS-031)
#ifndef VS_PLATFORM_H
#define VS_PLATFORM_H

#include "base.h"

// -------------------------------------------------- parcours de répertoire ---
//
// Primitive minimale exigée par l'algorithme borné de recherche de médias
// (media_core.c). Elle rend les entrées BRUTES, « . » et « .. » comprises :
// c'est le parcours portable qui décide de les ignorer, et le compteur
// d'entrées visitées doit rester identique à celui de FindFirstFileW.

typedef struct {
    Str8 name;       // nom de l'entrée en UTF-8, valable jusqu'au prochain next/close
    b32 is_dir;      // répertoire
    b32 is_link;     // jonction / lien symbolique : jamais suivi (boucles)
    i64 size_bytes;  // taille du fichier (0 pour un répertoire)
} VsDirEntry;

// VsDirIter est opaque : sa forme appartient à l'implémentation.
typedef struct VsDirIter VsDirIter;

typedef struct {
    // open ouvre `dir` pour énumération. NULL si le dossier est illisible.
    // L'itérateur et les noms rendus vivent dans l'arène `a` : l'appelant
    // encadre l'ensemble d'une portée temporaire.
    VsDirIter *(*open)(Arena *a, Str8 dir);
    // next rend l'entrée suivante ; 0 quand l'énumération est finie.
    b32 (*next)(VsDirIter *it, VsDirEntry *out);
    void (*close)(VsDirIter *it);
    // name_eq_ci compare deux noms de fichier sans tenir compte de la casse,
    // avec la sémantique du système de fichiers de la plateforme (ordinale,
    // indépendante de la locale). `scratch` sert aux conversions internes.
    b32 (*name_eq_ci)(Arena *scratch, Str8 a, Str8 b);
    u8 sep;  // séparateur de chemin ('\\' sous Windows)
} VsDirOps;

// vs_dir_ops renvoie l'implémentation de la plateforme.
const VsDirOps *vs_dir_ops(void);

#endif // VS_PLATFORM_H
