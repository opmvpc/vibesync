// secret.h — mémorisation du mot de passe serveur, chiffré par l'OS.
//
// DPAPI (CryptProtectData) : la clé appartient au compte Windows courant, elle
// ne quitte jamais l'OS et nous n'écrivons pas une ligne de cryptographie. Le
// blob est stocké en hexadécimal dans vibesync.ini ; recopié sur une autre
// machine ou un autre compte, il est simplement indéchiffrable — cas traité
// comme « pas de mot de passe mémorisé », sans bruit.
//
// Règle absolue : aucun chemin de code n'écrit le mot de passe en clair sur
// disque. Voir ini_flush() dans main.c, seul point d'écriture du fichier.
#ifndef VS_SECRET_H
#define VS_SECRET_H

#include "base.h"

// secret_protect chiffre `plain` et renvoie le blob en hexadécimal minuscule.
// 0 si DPAPI refuse (compte sans profil chargé, par exemple).
b32 secret_protect(Arena *a, Str8 plain, Str8 *out_hex);

// secret_unprotect déchiffre un blob hexadécimal produit par secret_protect.
// 0 — silencieusement — si le blob est corrompu, tronqué, non hexadécimal, ou
// chiffré par un autre compte/une autre machine.
b32 secret_unprotect(Arena *a, Str8 hex, Str8 *out_plain);

// secret_wipe efface un tampon sans que le compilateur puisse l'élider
// (SecureZeroMemory). À appeler dès qu'un clair ne sert plus.
void secret_wipe(void *p, isize n);

// secret_hex_decode est exposée pour les tests : refuse une longueur impaire
// ou un caractère non hexadécimal.
b32 secret_hex_decode(Arena *a, Str8 hex, u8 **out, isize *out_len);

#endif // VS_SECRET_H
