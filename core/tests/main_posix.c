// main_posix.c — point d'entrée POSIX de la suite de tests C (ADR-010, VS-031).
//
// Pendant de test_main.c (Windows) : il n'ordonnance que la moitié PORTABLE
// (test_core.c), la moitié plateforme Windows (test_win32.c) n'existant pas
// ici. Compilé et exécuté par scripts/test-core-macos.sh.
//
// Sortie non nulle en cas d'échec, avec diagnostic.

#include "base.h"
#include "test_util.h"

#include <stdio.h>

int main(int argc, char **argv) {
    Arena *a = arena_create(VS_MB(64));
    if (!a) {
        printf("arène impossible à créer\n");
        return 2;
    }
    Str8 override = str8_lit("");
    if (argc > 1) override = str8_from_cstr(argv[1]);

    test_core_run(a);
    test_core_vectors(a, override);

    printf("\n%d vérifications, %d échec(s)\n", g_checks, g_failures);
    arena_destroy(a);
    return g_failures == 0 ? 0 : 1;
}
