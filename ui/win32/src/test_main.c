// test_main.c — point d'entrée de la suite de tests console
// (vibesync_tests.exe).
//
// Depuis VS-030 (ADR-010), la suite est scindée en deux :
//   test_core.c   PORTABLE — base, json, protocol, engine, conn, parsing du
//                 status VLC, ini en mémoire, algorithme des dossiers médias,
//                 et le REJEU DES VECTEURS DE CONFORMITÉ test/vectors/*.json
//                 qui gèle le moteur de sync ;
//   test_win32.c  PLATEFORME — UTF-16, net.c, WebSocket réel, ini sur disque,
//                 UI/GDI, recherche sur une vraie arborescence, DPAPI, faux
//                 VLC HTTP sur socket.
//
// Ce fichier ne fait qu'ordonnancer les deux et rendre le verdict : les
// vecteurs restent en dernier, ils impriment une ligne par fichier.
//
// Sortie non nulle en cas d'échec, avec diagnostic.

#include "base.h"
#include "test_util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    Arena *a = arena_create(VS_MB(64));
    if (!a) {
        printf("arène impossible à créer\n");
        return 2;
    }
    Str8 override = str8_lit("");
    if (argc > 1) override = str8_from_cstr(argv[1]);

    test_core_run(a);
    test_win32_run(a);
    test_core_vectors(a, override);

    printf("\n%d vérifications, %d échec(s)\n", g_checks, g_failures);
    arena_destroy(a);
    return g_failures == 0 ? 0 : 1;
}
