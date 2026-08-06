// base.h — socle handmade : types, assert, arènes mémoire, chaînes UTF-8.
//
// Aucune dépendance en dehors de <stdint.h>/<stddef.h> et, dans base.c, de
// l'API Win32 (VirtualAlloc) et de l'UCRT (snprintf/strtod, livrés avec
// Windows 10). Toute allocation passe par une arène : pas un seul malloc.
#ifndef VS_BASE_H
#define VS_BASE_H

#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef float f32;
typedef double f64;
typedef int32_t b32;
typedef ptrdiff_t isize;

// Version applicative : injectée par build.bat depuis le fichier VERSION de la
// racine, sous forme de jeton brut que le préprocesseur met en chaîne. Sans
// injection (compilation à la main), on assume « dev ».
#define VS_STRINGIFY_(x) #x
#define VS_STRINGIFY(x) VS_STRINGIFY_(x)
#ifdef VIBESYNC_VERSION_RAW
#define VS_VERSION VS_STRINGIFY(VIBESYNC_VERSION_RAW)
#else
#define VS_VERSION "dev"
#endif
#define VS_PROTOCOL_VERSION_TEXT "1"

#define VS_ARRAY_COUNT(a) ((isize)(sizeof(a) / sizeof((a)[0])))
#define VS_MIN(a, b) ((a) < (b) ? (a) : (b))
#define VS_MAX(a, b) ((a) > (b) ? (a) : (b))
#define VS_KB(n) ((isize)(n) * 1024)
#define VS_MB(n) (VS_KB(n) * 1024)
#define VS_UNUSED(x) ((void)(x))

// vs_fatal écrit un diagnostic sur stderr et termine le process (code 3).
void vs_fatal(const char *file, int line, const char *fmt, ...);
#define VS_ASSERT(c)                                                     \
    do {                                                                 \
        if (!(c)) vs_fatal(__FILE__, __LINE__, "assertion: %s", #c);     \
    } while (0)

// ---------------------------------------------------------------- arènes ---

typedef struct Arena Arena;

// arena_create réserve `reserve_bytes` d'espace d'adressage (aucune page
// engagée avant usage). Renvoie NULL si la réservation échoue.
Arena *arena_create(isize reserve_bytes);
void arena_destroy(Arena *a);

// arena_push renvoie `size` octets alignés et remis à zéro. En cas de
// dépassement de la réserve : erreur fatale (bug de dimensionnement).
void *arena_push(Arena *a, isize size, isize align);
#define arena_push_array(a, T, n) (T *)arena_push((a), (isize)sizeof(T) * (isize)(n), (isize)_Alignof(T))
#define arena_push_struct(a, T) arena_push_array(a, T, 1)

isize arena_pos(const Arena *a);
void arena_pop_to(Arena *a, isize pos);
void arena_reset(Arena *a);
isize arena_capacity(const Arena *a);

// Portée temporaire : tout ce qui est alloué entre begin et end est libéré.
typedef struct {
    Arena *arena;
    isize pos;
} TempArena;
TempArena temp_begin(Arena *a);
void temp_end(TempArena t);

// ---------------------------------------------------------------- chaînes ---

// Str8 est une tranche UTF-8 (non terminée par NUL sauf mention contraire).
typedef struct {
    u8 *data;
    isize len;
} Str8;

#define str8_lit(s) ((Str8){(u8 *)(s), (isize)sizeof(s) - 1})

Str8 str8(u8 *data, isize len);
Str8 str8_from_cstr(const char *s);
b32 str8_eq(Str8 a, Str8 b);
b32 str8_eq_cstr(Str8 a, const char *b);
Str8 str8_copy(Arena *a, Str8 s);
Str8 str8_cat(Arena *a, Str8 x, Str8 y);
// str8_cstr renvoie une copie terminée par NUL (le NUL est hors de `len`).
char *str8_cstr(Arena *a, Str8 s);
b32 str8_starts_with(Str8 s, Str8 prefix);
isize str8_find_char(Str8 s, u8 c, isize from);
Str8 str8_sub(Str8 s, isize from, isize len);
Str8 str8_trim(Str8 s);

// StrBuf est un tampon de taille fixe, copiable, sans allocation : utilisé
// dans les structures d'état (moteur, messages) pour éviter toute question de
// durée de vie.
#define VS_STRBUF_CAP 512
typedef struct {
    isize len;
    u8 data[VS_STRBUF_CAP];
} StrBuf;

void strbuf_set(StrBuf *b, Str8 s);
Str8 strbuf_str(const StrBuf *b);
b32 strbuf_eq(const StrBuf *b, Str8 s);

// ------------------------------------------------------------ constructeur ---

// Builder accumule des octets dans une arène ; il s'étend en place tant qu'il
// est la dernière allocation de l'arène (cas courant), sinon il se recopie.
typedef struct {
    Arena *arena;
    u8 *data;
    isize len;
    isize cap;
} Builder;

void builder_init(Builder *b, Arena *a, isize initial_cap);
void builder_bytes(Builder *b, const void *data, isize len);
void builder_str(Builder *b, Str8 s);
void builder_cstr(Builder *b, const char *s);
void builder_byte(Builder *b, u8 c);
void builder_i64(Builder *b, i64 v);
void builder_f64(Builder *b, f64 v);
Str8 builder_result(const Builder *b);

// ---------------------------------------------------------------- nombres ---

b32 f64_is_finite(f64 v);
// f64_to_str écrit la représentation JSON la plus courte qui relit exactement
// la même valeur. Renvoie le nombre d'octets écrits (0 si non fini).
isize f64_to_str(f64 v, char *buf, isize cap);
// str_to_f64 lit un flottant complet (toute la chaîne doit être consommée).
b32 str_to_f64(Str8 s, f64 *out);
isize i64_to_str(i64 v, char *buf, isize cap);
b32 str_to_i64(Str8 s, i64 *out);
f64 f64_round(f64 v);
f64 f64_abs(f64 v);

// -------------------------------------------------------------- unicode ---

// utf8_to_utf16 renvoie une chaîne UTF-16 terminée par 0 (arène). Les octets
// invalides deviennent U+FFFD. `out_len` (optionnel) reçoit la longueur.
u16 *utf8_to_utf16(Arena *a, Str8 s, isize *out_len);
// utf16_to_utf8 convertit une chaîne UTF-16 terminée par 0.
Str8 utf16_to_utf8(Arena *a, const u16 *w);
// utf8_validate vérifie qu'une tranche est de l'UTF-8 bien formé.
b32 utf8_validate(Str8 s);
// utf8_encode écrit un point de code (1..4 octets) ; renvoie la taille.
isize utf8_encode(u32 cp, u8 *out);

// ----------------------------------------------------------------- temps ---

#define VS_TIME_ZERO INT64_MIN

// vs_now_ns est l'horloge murale en nanosecondes depuis l'epoch Unix.
i64 vs_now_ns(void);
// vs_ns_to_unix_ms convertit en millisecondes epoch (division plancher).
i64 vs_ns_to_unix_ms(i64 ns);
// vs_ns_seconds convertit une durée en secondes, avec la même arithmétique
// que time.Duration.Seconds() en Go (partie entière + reste).
f64 vs_ns_seconds(i64 dur_ns);

// ------------------------------------------------------------------ hasard ---

// vs_random_bytes remplit `out` depuis le générateur du système
// (BCryptGenRandom, RNG préféré de l'OS). 0 en cas d'échec.
b32 vs_random_bytes(u8 *out, isize n);
// vs_hex_encode écrit 2n caractères hexadécimaux minuscules + NUL.
void vs_hex_encode(const u8 *bytes, isize n, char *out);

// ------------------------------------------------------------------ sortie ---

void vs_write_stderr(Str8 s);

// ----------------------------------------------------------------- journal ---

// VS_LOG_MAX plafonne %APPDATA%\vibesync.log : au-delà, le fichier repart de
// zéro. Un journal qui grossit sans fin chez l'utilisateur est un bug, pas une
// fonctionnalité.
#define VS_LOG_MAX VS_MB(1)

// vs_log ajoute une ligne horodatée (UTC) à %APPDATA%\vibesync.log, et la
// recopie sur stderr et dans le débogueur. Sans journal, un échec de lancement
// de VLC chez un ami se résume à « ça marche pas » : c'est le seul témoin
// exploitable à distance (VS-029). Appelable depuis n'importe quel thread : les
// écritures sont sérialisées par un verrou interne, et la rotation décide sur
// une taille lue sous ce même verrou. Aucun secret n'y est jamais écrit — ni
// mot de passe de salle, ni mot de passe d'interface VLC.
void vs_log(const char *fmt, ...);

#endif // VS_BASE_H
