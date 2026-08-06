// test_util.h — harnais partagé par les deux moitiés de la suite (VS-030).
//
// La suite est scindée comme le code (ADR-010) :
//   test_core.c   tests PORTABLES — compilables et exécutables sur macOS,
//                 rejeu des vecteurs de conformité compris ;
//   test_win32.c  tests de la moitié plateforme (Winsock, WinHTTP, DPAPI,
//                 GDI, disque, UTF-16) ;
//   test_main.c   le main Windows, qui appelle les deux.
//
// Le compteur de vérifications est commun : le total affiché doit rester
// identique au découpage près (aucune vérification perdue ni ajoutée).
#ifndef VS_TEST_UTIL_H
#define VS_TEST_UTIL_H

#include "base.h"

#include <string.h>

extern int g_failures;
extern int g_checks;
extern const char *g_section;

void failf(const char *fmt, ...);
void section(const char *name);

#define CHECK(cond, ...)              \
    do {                              \
        g_checks++;                   \
        if (!(cond)) failf(__VA_ARGS__); \
    } while (0)

static inline b32 approx(f64 a, f64 b, f64 tol) {
    f64 d = a - b;
    if (d < 0) d = -d;
    return d <= tol;
}

static inline Str8 S(const char *s) { return str8_from_cstr(s); }

// contains : recherche naïve de sous-chaîne, suffisante pour vérifier qu'un
// drapeau figure bien dans une ligne de commande.
static inline b32 contains(Str8 hay, const char *needle) {
    Str8 n = str8_from_cstr(needle);
    if (n.len == 0) return 1;
    for (isize i = 0; i + n.len <= hay.len; i++) {
        if (memcmp(hay.data + i, n.data, (size_t)n.len) == 0) return 1;
    }
    return 0;
}

// --- points d'entrée ---

// test_core_run : tout le portable sauf les vecteurs (qui restent en dernier,
// comme avant le découpage, parce qu'ils impriment une ligne par vecteur).
void test_core_run(Arena *a);
void test_core_vectors(Arena *a, Str8 override);
// test_win32_run n'existe que sur Windows.
void test_win32_run(Arena *a);

#endif // VS_TEST_UTIL_H
