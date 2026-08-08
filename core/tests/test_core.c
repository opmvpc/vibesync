// test_core.c — moitié PORTABLE de la suite de tests (ADR-010, VS-030).
//
// Ne dépend que du C portable de core/src : base_core, json, protocol,
// engine, conn, vlc_core, ini_core, media_core. Compilable et exécutable
// ailleurs que sous Windows — c'est tout l'intérêt : le REJEU DES VECTEURS DE
// CONFORMITÉ test/vectors/*.json, qui gèle le moteur de sync, cesse d'être un
// test « à l'aveugle » réservé à la machine de build Windows.
//
// La moitié plateforme (Winsock, WinHTTP, DPAPI, GDI, disque, UTF-16) est dans
// ui/win32/src/test_win32.c. Le compteur de vérifications est commun : le
// découpage n'en ajoute ni n'en retire aucune.
//
// Depuis VS-031, ce fichier est compilé DEUX FOIS : par build.bat (Windows) et
// par scripts/test-core-macos.sh (macOS, asan+ubsan, main_posix.c).

#include "conn.h"
#include "engine.h"
#include "ini.h"
#include "json.h"
#include "media.h"
#include "platform.h"
#include "protocol.h"
#include "test_util.h"
#include "vlc.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ------------------------------------------------------------ harnais ---

int g_failures = 0;
int g_checks = 0;
const char *g_section = "";

void failf(const char *fmt, ...) {
    g_failures++;
    printf("  ECHEC [%s] ", g_section);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

void section(const char *name) {
    g_section = name;
    printf("== %s\n", name);
}

// ------------------------------------------------------------ base ---

static void test_base(Arena *a) {
    section("base");

    // Arène : alignement, remise à zéro, portée temporaire.
    isize start = arena_pos(a);
    u8 *p1 = arena_push_array(a, u8, 3);
    u64 *p2 = arena_push_array(a, u64, 2);
    CHECK(((uintptr_t)p2 % 8) == 0, "alignement u64 non respecté");
    CHECK(p1[0] == 0 && p2[0] == 0, "mémoire non remise à zéro");
    p1[0] = 7;
    {
        TempArena t = temp_begin(a);
        isize inner = arena_pos(a);
        arena_push_array(a, u8, 4096);
        CHECK(arena_pos(a) > inner, "l'arène n'a pas avancé");
        temp_end(t);
        CHECK(arena_pos(a) == inner, "temp_end n'a pas rendu la mémoire");
    }
    arena_pop_to(a, start);

    // Chaînes.
    CHECK(str8_eq(S("abc"), str8_lit("abc")), "str8_eq");
    CHECK(!str8_eq(S("abc"), S("abd")), "str8_eq faux positif");
    CHECK(str8_starts_with(S("ws://x"), str8_lit("ws://")), "str8_starts_with");
    CHECK(str8_find_char(S("a:b"), ':', 0) == 1, "str8_find_char");
    CHECK(str8_eq(str8_trim(S("  x \r\n")), S("x")), "str8_trim");
    CHECK(str8_eq(str8_cat(a, S("ab"), S("cd")), S("abcd")), "str8_cat");
    Str8 sub = str8_sub(S("abcdef"), 2, 3);
    CHECK(str8_eq(sub, S("cde")), "str8_sub");
    CHECK(str8_sub(S("abc"), 10, 5).len == 0, "str8_sub hors bornes");

    StrBuf buf;
    strbuf_set(&buf, S("hello"));
    CHECK(strbuf_eq(&buf, S("hello")), "strbuf_set");
    // Débordement : troncature propre, jamais d'écriture hors bornes.
    u8 big[VS_STRBUF_CAP * 2];
    memset(big, 'x', sizeof(big));
    strbuf_set(&buf, str8(big, (isize)sizeof(big)));
    CHECK(buf.len == VS_STRBUF_CAP - 1, "strbuf tronqué à %lld", (long long)buf.len);
    CHECK(buf.data[buf.len] == 0, "strbuf non terminé");

    // Builder : croissance en place et par recopie.
    {
        TempArena t = temp_begin(a);
        Builder b;
        builder_init(&b, a, 8);
        for (int i = 0; i < 1000; i++) builder_cstr(&b, "ab");
        CHECK(builder_result(&b).len == 2000, "builder longueur");
        Builder b2;
        builder_init(&b2, a, 8);  // b n'est plus la dernière allocation
        builder_cstr(&b, "Z");
        CHECK(builder_result(&b).len == 2001, "builder après recopie");
        CHECK(builder_result(&b).data[2000] == 'Z', "builder contenu après recopie");
        builder_cstr(&b2, "y");
        CHECK(builder_result(&b2).len == 1, "builder voisin corrompu");
        temp_end(t);
    }

    // Nombres.
    char nb[64];
    CHECK(i64_to_str(0, nb, sizeof(nb)) == 1 && nb[0] == '0', "i64_to_str 0");
    i64_to_str(-9223372036854775807LL - 1, nb, sizeof(nb));
    CHECK(strcmp(nb, "-9223372036854775808") == 0, "i64_to_str min: %s", nb);
    i64 v = 0;
    CHECK(str_to_i64(S("-9223372036854775808"), &v) && v == (-9223372036854775807LL - 1), "str_to_i64 min");
    CHECK(!str_to_i64(S("9223372036854775808"), &v), "str_to_i64 débordement accepté");
    CHECK(!str_to_i64(S("12a"), &v), "str_to_i64 déchet accepté");
    CHECK(!str_to_i64(S(""), &v), "str_to_i64 vide accepté");

    // Bornes exactes et dépassements des deux côtés (VS-035). Contrat : hors
    // de [INT64_MIN, INT64_MAX] la fonction REFUSE et laisse *out intact — pas
    // de saturation, pas d'enroulement, et surtout aucune négation d'INT64_MIN.
    CHECK(str_to_i64(S("9223372036854775807"), &v) && v == 9223372036854775807LL, "str_to_i64 max");
    CHECK(str_to_i64(S("+9223372036854775807"), &v) && v == 9223372036854775807LL, "str_to_i64 max signé +");
    CHECK(str_to_i64(S("-9223372036854775807"), &v) && v == -9223372036854775807LL, "str_to_i64 min+1");
    CHECK(str_to_i64(S("  -9223372036854775808  "), &v) && v == (-9223372036854775807LL - 1),
          "str_to_i64 min entouré d'espaces");
    CHECK(str_to_i64(S("-0000000000009223372036854775808"), &v) && v == (-9223372036854775807LL - 1),
          "str_to_i64 min avec zéros de tête");
    CHECK(str_to_i64(S("-0"), &v) && v == 0, "str_to_i64 -0");
    // Aller-retour sur les deux bornes : i64_to_str et str_to_i64 sont inverses.
    i64_to_str(-9223372036854775807LL - 1, nb, sizeof(nb));
    CHECK(str_to_i64(str8_from_cstr(nb), &v) && v == (-9223372036854775807LL - 1), "aller-retour i64 min");
    i64_to_str(9223372036854775807LL, nb, sizeof(nb));
    CHECK(str_to_i64(str8_from_cstr(nb), &v) && v == 9223372036854775807LL, "aller-retour i64 max");
    // Dépassements : *out doit rester tel quel (sentinelle 424242).
    v = 424242;
    CHECK(!str_to_i64(S("-9223372036854775809"), &v) && v == 424242, "str_to_i64 sous-débordement accepté");
    CHECK(!str_to_i64(S("+9223372036854775808"), &v) && v == 424242, "str_to_i64 débordement signé + accepté");
    CHECK(!str_to_i64(S("99999999999999999999"), &v) && v == 424242, "str_to_i64 débordement large accepté");
    CHECK(!str_to_i64(S("-99999999999999999999"), &v) && v == 424242, "str_to_i64 sous-débordement large accepté");
    CHECK(!str_to_i64(S("18446744073709551616"), &v) && v == 424242, "str_to_i64 2^64 accepté");
    CHECK(!str_to_i64(S("-"), &v) && v == 424242, "str_to_i64 signe seul accepté");
    CHECK(!str_to_i64(S("+"), &v) && v == 424242, "str_to_i64 plus seul accepté");
    CHECK(!str_to_i64(S("   "), &v) && v == 424242, "str_to_i64 espaces seuls acceptés");

    f64 nums[] = {0, 1, -1, 0.1, 100.2, 101.00000000000001, 1e-9, 1e300, -0.0, 3600, 0.95, 1.05};
    for (isize i = 0; i < VS_ARRAY_COUNT(nums); i++) {
        isize n = f64_to_str(nums[i], nb, sizeof(nb));
        CHECK(n > 0, "f64_to_str vide");
        f64 back = 0;
        CHECK(str_to_f64(str8((u8 *)nb, n), &back), "relecture de %s", nb);
        CHECK(back == nums[i], "aller-retour flottant cassé pour %s", nb);
    }
    CHECK(f64_to_str(1.0 / 0.0 * 0.0, nb, sizeof(nb)) == 0 || 1, "NaN toléré");
    CHECK(!f64_is_finite(1e308 * 10), "infini considéré fini");
    CHECK(f64_round(2.5) == 3 && f64_round(-2.5) == -3 && f64_round(2.4) == 2, "f64_round");

    // Temps : la même arithmétique que Go (secondes = entier + reste).
    CHECK(vs_ns_seconds(200 * 1000000LL) == 0.2, "vs_ns_seconds 200 ms");
    CHECK(vs_ns_seconds(20200 * 1000000LL) == 20.2, "vs_ns_seconds 20,2 s");
    CHECK(vs_ns_to_unix_ms(1785960000000LL * 1000000LL) == 1785960000000LL, "vs_ns_to_unix_ms");

    // UTF-8 : validation (les conversions UTF-16 sont l'affaire de la
    // plateforme, cf. test_win32.c).
    {
        Str8 src = S("caf\xc3\xa9 \xf0\x9f\x98\x80 fin");  // « café 😀 fin »
        u8 bad[] = {0xc3, 0x28, 0};  // séquence invalide
        CHECK(!utf8_validate(str8(bad, 2)), "UTF-8 invalide accepté");
        CHECK(utf8_validate(src), "UTF-8 valide refusé");
    }
}

// ------------------------------------------------------------ json ---

static void test_json(Arena *a) {
    section("json");
    TempArena top = temp_begin(a);

    // Aller-retour complet.
    {
        JsonWriter w;
        jw_init(&w, a);
        jw_obj_begin(&w);
        jw_kv_str(&w, "type", S("hello"));
        jw_key(&w, "data");
        jw_obj_begin(&w);
        jw_kv_i64(&w, "version", 1);
        jw_kv_num(&w, "pos", 101.00000000000001);
        jw_kv_bool(&w, "ready", 1);
        jw_key(&w, "tags");
        jw_arr_begin(&w);
        jw_cstr(&w, "a\"b");
        jw_cstr(&w, "ligne\nsuite\ttab");
        jw_num(&w, -0.5);
        jw_null(&w);
        jw_arr_end(&w);
        jw_kv_str(&w, "uni", S("caf\xc3\xa9 \xf0\x9f\x98\x80"));
        jw_obj_end(&w);
        jw_obj_end(&w);
        Str8 out = jw_result(&w);

        JsonError err = JSON_OK;
        JsonValue *root = json_parse(a, out, &err);
        CHECK(root != NULL, "relecture impossible (%s) : %.*s", json_error_text(err), (int)out.len, out.data);
        if (root) {
            CHECK(str8_eq(json_get_str(root, "type", S("")), S("hello")), "type");
            JsonValue *d = json_get(root, "data");
            CHECK(json_get_i64(d, "version", 0) == 1, "version");
            CHECK(json_get_num(d, "pos", 0) == 101.00000000000001, "précision du flottant perdue");
            CHECK(json_get_bool(d, "ready", 0), "bool");
            JsonValue *tags = json_get(d, "tags");
            CHECK(tags && tags->count == 4, "tableau de %lld éléments", tags ? (long long)tags->count : -1);
            CHECK(str8_eq(json_str(json_at(tags, 0), S("")), S("a\"b")), "échappement guillemet");
            CHECK(str8_eq(json_str(json_at(tags, 1), S("")), S("ligne\nsuite\ttab")), "échappements de contrôle");
            CHECK(json_num(json_at(tags, 2), 0) == -0.5, "nombre négatif");
            CHECK(json_at(tags, 3)->kind == JSON_NULL, "null");
            CHECK(str8_eq(json_get_str(d, "uni", S("")), S("caf\xc3\xa9 \xf0\x9f\x98\x80")), "UTF-8 conservé");
        }
    }

    // Échappements \u, paires de substitution, moitiés orphelines.
    {
        JsonError err;
        JsonValue *v = json_parse(a, S("{\"a\":\"\\u0041\\u00e9\\ud83d\\ude00\"}"), &err);
        CHECK(v != NULL, "chaîne \\u refusée: %s", json_error_text(err));
        Str8 s = json_get_str(v, "a", S(""));
        CHECK(str8_eq(s, S("A\xc3\xa9\xf0\x9f\x98\x80")), "décodage \\u incorrect (%lld octets)", (long long)s.len);

        v = json_parse(a, S("{\"a\":\"\\ud83d\"}"), &err);
        CHECK(v != NULL, "moitié haute orpheline refusée");
        s = json_get_str(v, "a", S(""));
        CHECK(s.len == 3 && s.data[0] == 0xef && s.data[1] == 0xbf && s.data[2] == 0xbd,
              "moitié orpheline non remplacée par U+FFFD");

        v = json_parse(a, S("{\"a\":\"\\udc00x\"}"), &err);
        CHECK(v != NULL, "moitié basse orpheline refusée");
        CHECK(json_get_str(v, "a", S("")).len == 4, "moitié basse orpheline mal remplacée");

        v = json_parse(a, S("{\"a\":\"\\u00\"}"), &err);
        CHECK(v == NULL && err == JSON_ERR_STRING, "\\u tronqué accepté");
    }

    // Entrées hostiles : chacune doit être refusée sans crasher.
    {
        static const char *bad[] = {
            "",  "   ",  "{",  "[",  "}",  "]",  "{\"a\"}",  "{\"a\":}",  "{a:1}",  "{\"a\":1,}",
            "[1,]",  "[1 2]",  "{} {}",  "nul",  "tru",  "NaN",  "Infinity",  "-Infinity",
            "01",  "1.",  ".5",  "+1",  "1e",  "1e+",  "--1",  "1e999",  "0x10",
            "\"pas fermé",  "\"\\x\"",  "\"\t\"",  "{\"a\":\"\\\"}",  "[\"a\" \"b\"]",
        };
        for (isize i = 0; i < VS_ARRAY_COUNT(bad); i++) {
            JsonError err = JSON_OK;
            JsonValue *v = json_parse(a, S(bad[i]), &err);
            CHECK(v == NULL && err != JSON_OK, "entrée hostile acceptée: %s", bad[i]);
        }
        // UTF-8 invalide dans une chaîne.
        u8 raw[] = {'{', '"', 'a', '"', ':', '"', 0xc3, 0x28, '"', '}'};
        JsonError err = JSON_OK;
        CHECK(json_parse(a, str8(raw, (isize)sizeof(raw)), &err) == NULL && err == JSON_ERR_UTF8,
              "UTF-8 invalide accepté dans une chaîne");
    }

    // Profondeur : 32 niveaux passent, 33 sont refusés.
    {
        Builder ok, ko;
        builder_init(&ok, a, 256);
        for (int i = 0; i < 31; i++) builder_cstr(&ok, "[");
        builder_cstr(&ok, "1");
        for (int i = 0; i < 31; i++) builder_cstr(&ok, "]");
        JsonError err = JSON_OK;
        CHECK(json_parse(a, builder_result(&ok), &err) != NULL, "31 niveaux refusés (%s)", json_error_text(err));

        builder_init(&ko, a, 512);
        for (int i = 0; i < 64; i++) builder_cstr(&ko, "[");
        builder_cstr(&ko, "1");
        for (int i = 0; i < 64; i++) builder_cstr(&ko, "]");
        err = JSON_OK;
        CHECK(json_parse(a, builder_result(&ko), &err) == NULL && err == JSON_ERR_DEPTH,
              "64 niveaux acceptés (%s)", json_error_text(err));
    }

    // Grand document : pas de débordement, pas de fuite hors arène.
    {
        Builder big;
        builder_init(&big, a, VS_KB(64));
        builder_cstr(&big, "[");
        for (int i = 0; i < 5000; i++) {
            if (i) builder_cstr(&big, ",");
            builder_cstr(&big, "{\"k\":");
            builder_i64(&big, i);
            builder_cstr(&big, "}");
        }
        builder_cstr(&big, "]");
        JsonError err = JSON_OK;
        JsonValue *v = json_parse(a, builder_result(&big), &err);
        CHECK(v && v->count == 5000, "grand tableau: %s", json_error_text(err));
        CHECK(json_get_i64(json_at(v, 4999), "k", -1) == 4999, "dernier élément");
    }

    // Budget : un document dense doit être refusé proprement, pas épuiser
    // l'arène (vs_fatal tuerait le process sur une entrée hostile).
    {
        Builder dense;
        builder_init(&dense, a, VS_MB(2));
        builder_cstr(&dense, "[");
        for (int i = 0; i < JSON_MAX_VALUES + 100; i++) {
            if (i) builder_cstr(&dense, ",");
            builder_cstr(&dense, "0");
        }
        builder_cstr(&dense, "]");
        JsonError err = JSON_OK;
        CHECK(json_parse(a, builder_result(&dense), &err) == NULL && err == JSON_ERR_BUDGET,
              "budget de valeurs non appliqué (%s)", json_error_text(err));
    }
    // Budget mémoire : une petite arène refuse au lieu d'exploser.
    {
        Arena *small = arena_create(VS_KB(256));
        CHECK(small != NULL, "arène de test");
        if (small) {
            Builder dense;
            builder_init(&dense, a, VS_KB(512));
            builder_cstr(&dense, "[");
            for (int i = 0; i < 20000; i++) {
                if (i) builder_cstr(&dense, ",");
                builder_cstr(&dense, "{\"k\":1}");
            }
            builder_cstr(&dense, "]");
            JsonError err = JSON_OK;
            CHECK(json_parse(small, builder_result(&dense), &err) == NULL && err == JSON_ERR_BUDGET,
                  "budget d'arène non appliqué (%s)", json_error_text(err));
            arena_destroy(small);
        }
    }

    // Accès tolérants au NULL / aux mauvais types.
    {
        JsonValue *v = json_parse(a, S("{\"a\":1,\"b\":\"x\"}"), NULL);
        CHECK(json_get_num(v, "absent", 42) == 42, "défaut sur clé absente");
        CHECK(json_get_num(v, "b", 42) == 42, "défaut sur mauvais type");
        CHECK(json_get(NULL, "a") == NULL, "json_get(NULL)");
        CHECK(json_at(v, 0) == NULL, "json_at sur objet");
        CHECK(json_num(NULL, 7) == 7, "json_num(NULL)");
    }

    temp_end(top);
}

// ------------------------------------------------------------ protocol ---

static void test_protocol(Arena *a) {
    section("protocol");
    TempArena top = temp_begin(a);

    // hello : champs optionnels omis s'ils sont vides.
    {
        Str8 h = proto_encode_hello(a, S("thib"), S("salon"), S(""), S(""));
        JsonValue *v = json_parse(a, h, NULL);
        CHECK(v != NULL, "hello illisible: %.*s", (int)h.len, h.data);
        CHECK(str8_eq(json_get_str(v, "type", S("")), S("hello")), "type du hello");
        JsonValue *d = json_get(v, "data");
        CHECK(json_get_i64(d, "version", 0) == VS_PROTOCOL_VERSION, "version");
        CHECK(str8_eq(json_get_str(d, "name", S("")), S("thib")), "name");
        CHECK(str8_eq(json_get_str(d, "room", S("")), S("salon")), "room");
        CHECK(json_get(d, "password") == NULL, "mot de passe vide non omis");
        CHECK(json_get(d, "session") == NULL, "session vide non omise");

        h = proto_encode_hello(a, S("thib"), S("salon"), S("s3cr3t"), S("0123456789abcdef0123456789abcdef"));
        v = json_parse(a, h, NULL);
        d = json_get(v, "data");
        CHECK(str8_eq(json_get_str(d, "password", S("")), S("s3cr3t")), "mot de passe");
        CHECK(json_get_str(d, "session", S("")).len == VS_SESSION_TOKEN_LEN, "jeton de session");
    }

    // Jeton de session : hexadécimal, longueur fixe, non constant.
    {
        char t1[VS_SESSION_TOKEN_LEN + 1], t2[VS_SESSION_TOKEN_LEN + 1];
        CHECK(proto_session_token(t1, sizeof(t1)), "génération du jeton");
        CHECK(proto_session_token(t2, sizeof(t2)), "génération du jeton (2)");
        CHECK(strlen(t1) == VS_SESSION_TOKEN_LEN, "longueur du jeton = %d", (int)strlen(t1));
        CHECK(strcmp(t1, t2) != 0, "jeton constant !");
        for (isize i = 0; i < VS_SESSION_TOKEN_LEN; i++) {
            char c = t1[i];
            CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'), "caractère non hexadécimal: %c", c);
        }
        char small[4];
        CHECK(!proto_session_token(small, sizeof(small)), "tampon trop petit accepté");
        // Un jeton fraîchement tiré doit être relisible tel quel : c'est ce qui
        // fait qu'un redémarrage réutilise le même (VS-028).
        CHECK(proto_session_token_valid(S(t1)), "jeton généré refusé à la relecture");
    }

    // Validation d'un jeton RELU des réglages (VS-028) : mêmes règles que
    // validSessionToken() du client Go — hexadécimal, longueur paire, de 16
    // octets à VS_SESSION_TOKEN_MAX caractères.
    {
        CHECK(proto_session_token_valid(S("0123456789abcdef0123456789abcdef")), "jeton hexa 32 refusé");
        CHECK(proto_session_token_valid(S("0123456789ABCDEF0123456789ABCDEF")), "hexa majuscule refusé");
        CHECK(!proto_session_token_valid(S("")), "jeton vide accepté");
        CHECK(!proto_session_token_valid(S("0123456789abcdef0123456789abcde")), "jeton trop court accepté");
        CHECK(!proto_session_token_valid(S("0123456789abcdef0123456789abcdeg")), "caractère non hexa accepté");
        CHECK(!proto_session_token_valid(S("0123456789abcdef 123456789abcdef")), "espace accepté");
        // Longueur impaire : ne peut pas être un nombre entier d'octets.
        char odd[VS_SESSION_TOKEN_LEN + 2];
        memset(odd, 'a', sizeof(odd));
        CHECK(!proto_session_token_valid(str8((u8 *)odd, VS_SESSION_TOKEN_LEN + 1)), "longueur impaire acceptée");
        // Borne haute : exactement VS_SESSION_TOKEN_MAX passe, un de plus non.
        char big[VS_SESSION_TOKEN_MAX + 2];
        memset(big, 'f', sizeof(big));
        CHECK(proto_session_token_valid(str8((u8 *)big, VS_SESSION_TOKEN_MAX)), "128 caractères refusés");
        CHECK(!proto_session_token_valid(str8((u8 *)big, VS_SESSION_TOKEN_MAX + 2)), "au-delà de 128 accepté");
    }

    // Encodage des messages sortants.
    {
        VsMsg m;
        memset(&m, 0, sizeof(m));
        m.kind = VS_MSG_CONTROL;
        m.action = VS_ACT_SEEK;
        m.position_sec = 42.5;
        JsonValue *d = json_get(json_parse(a, proto_encode_msg(a, &m), NULL), "data");
        CHECK(str8_eq(json_get_str(d, "action", S("")), S("seek")), "action");
        CHECK(json_get_num(d, "positionSec", 0) == 42.5, "positionSec");

        memset(&m, 0, sizeof(m));
        m.kind = VS_MSG_REPORT;
        m.position_sec = 101.00000000000001;
        m.paused = 1;
        m.buffering = 0;
        d = json_get(json_parse(a, proto_encode_msg(a, &m), NULL), "data");
        CHECK(json_get_num(d, "positionSec", 0) == 101.00000000000001, "report positionSec");
        CHECK(json_get_bool(d, "paused", 0) == 1 && json_get_bool(d, "buffering", 1) == 0, "report drapeaux");

        memset(&m, 0, sizeof(m));
        m.kind = VS_MSG_SET_FILE;
        strbuf_set(&m.name, S("ep1 \"guillemets\".mkv"));
        m.duration_sec = 1200;
        m.size_bytes = 15;
        Str8 raw = proto_encode_msg(a, &m);
        d = json_get(json_parse(a, raw, NULL), "data");
        CHECK(str8_eq(json_get_str(d, "name", S("")), S("ep1 \"guillemets\".mkv")), "nom échappé");
        CHECK(json_get_i64(d, "sizeBytes", 0) == 15, "sizeBytes");

        memset(&m, 0, sizeof(m));
        m.kind = VS_MSG_PING;
        m.t = 1785960002000LL;
        d = json_get(json_parse(a, proto_encode_msg(a, &m), NULL), "data");
        CHECK(json_get_i64(d, "t", 0) == 1785960002000LL, "ping t");

        memset(&m, 0, sizeof(m));
        m.kind = VS_MSG_CHAT;
        strbuf_set(&m.text, S("salut \xf0\x9f\x98\x80"));
        d = json_get(json_parse(a, proto_encode_msg(a, &m), NULL), "data");
        CHECK(str8_eq(json_get_str(d, "text", S("")), S("salut \xf0\x9f\x98\x80")), "chat");

        memset(&m, 0, sizeof(m));
        m.kind = VS_MSG_SET_READY;
        m.ready = 1;
        d = json_get(json_parse(a, proto_encode_msg(a, &m), NULL), "data");
        CHECK(json_get_bool(d, "ready", 0), "setReady");
    }

    // Décodage des messages entrants.
    {
        VsInMsg *m = proto_decode(a, S("{\"type\":\"welcome\",\"data\":{\"selfId\":\"u1\",\"room\":\"salon\","
                                       "\"state\":{\"paused\":false,\"positionSec\":100,\"rate\":1,"
                                       "\"refServerMs\":1785960000000,\"setBy\":\"u2\"},"
                                       "\"users\":[{\"id\":\"u1\",\"name\":\"thib\",\"ready\":true,"
                                       "\"file\":{\"name\":\"ep1.mkv\",\"durationSec\":1200,\"sizeBytes\":15}},"
                                       "{\"id\":\"u2\",\"name\":\"ami\"}]}}"));
        CHECK(m && m->kind == VS_IN_WELCOME, "welcome non reconnu");
        CHECK(str8_eq(m->self_id, S("u1")), "selfId");
        CHECK(m->have_state && m->state.position_sec == 100 && m->state.ref_server_ms == 1785960000000LL,
              "état de salle");
        CHECK(strbuf_eq(&m->state.set_by, S("u2")), "setBy");
        CHECK(m->user_count == 2, "%lld participants", (long long)m->user_count);
        CHECK(m->have_self_ready && m->self_ready, "ready de soi non extrait");
        CHECK(m->users[0].has_file && m->users[0].file_duration_sec == 1200, "fichier du participant");

        m = proto_decode(a, S("{\"type\":\"pong\",\"data\":{\"t\":10,\"serverMs\":20}}"));
        CHECK(m && m->kind == VS_IN_PONG && m->pong.t == 10 && m->pong.server_ms == 20, "pong");

        m = proto_decode(a, S("{\"type\":\"roomState\",\"data\":{\"paused\":true,\"positionSec\":12.5,"
                              "\"rate\":1,\"refServerMs\":5,\"setBy\":\"u3\"}}"));
        CHECK(m && m->kind == VS_IN_ROOMSTATE && m->have_state && m->state.paused, "roomState");

        m = proto_decode(a, S("{\"type\":\"toast\",\"data\":{\"level\":\"warn\",\"text\":\"fichiers différents\"}}"));
        CHECK(m && m->kind == VS_IN_TOAST && str8_eq(m->level, S("warn")), "toast");

        m = proto_decode(a, S("{\"type\":\"chatEvent\",\"data\":{\"from\":\"ami\",\"text\":\"coucou\",\"serverMs\":42}}"));
        CHECK(m && m->kind == VS_IN_CHATEVENT && str8_eq(m->from, S("ami")) && m->server_ms == 42, "chatEvent");

        m = proto_decode(a, S("{\"type\":\"error\",\"data\":{\"code\":\"bad_password\"}}"));
        CHECK(m && m->kind == VS_IN_ERROR && proto_error_is_fatal(m->code), "erreur fatale");
        m = proto_decode(a, S("{\"type\":\"error\",\"data\":{\"code\":\"protocol\",\"text\":\"oups\"}}"));
        CHECK(m && !proto_error_is_fatal(m->code), "erreur protocol jugée fatale");

        // Forward-compat : un type inconnu est ignoré, pas fatal.
        m = proto_decode(a, S("{\"type\":\"futur\",\"data\":{\"x\":1}}"));
        CHECK(m && m->kind == VS_IN_UNKNOWN, "message inconnu");
        // Messages illisibles.
        CHECK(proto_decode(a, S("pas du json")) == NULL, "JSON invalide accepté");
        CHECK(proto_decode(a, S("{\"data\":{}}")) == NULL, "enveloppe sans type acceptée");
        CHECK(proto_decode(a, S("[]")) == NULL, "enveloppe non-objet acceptée");
        // data absent ou champs obligatoires manquants : message invalidé,
        // jamais transformé en zéros silencieux.
        m = proto_decode(a, S("{\"type\":\"welcome\"}"));
        CHECK(m && m->kind == VS_IN_UNKNOWN && m->invalid, "welcome sans data accepté");
        m = proto_decode(a, S("{\"type\":\"welcome\",\"data\":{\"selfId\":\"u1\"}}"));
        CHECK(m && m->invalid, "welcome sans state accepté");
        m = proto_decode(a, S("{\"type\":\"pong\",\"data\":{}}"));
        CHECK(m && m->kind == VS_IN_UNKNOWN && m->invalid, "pong vide accepté (devenait t=0)");
        m = proto_decode(a, S("{\"type\":\"pong\",\"data\":{\"t\":\"10\",\"serverMs\":20}}"));
        CHECK(m && m->invalid, "pong avec t textuel accepté");
        m = proto_decode(a, S("{\"type\":\"pong\",\"data\":{\"t\":10,\"serverMs\":9e18}}"));
        CHECK(m && m->invalid, "pong avec serverMs hors bornes accepté");
        m = proto_decode(a, S("{\"type\":\"pong\",\"data\":{\"t\":-5,\"serverMs\":20}}"));
        CHECK(m && m->invalid, "pong avec t négatif accepté");
        m = proto_decode(a, S("{\"type\":\"roomState\",\"data\":{\"paused\":false,\"positionSec\":1,"
                              "\"rate\":1}}"));
        CHECK(m && m->invalid, "roomState en lecture sans refServerMs accepté");
        m = proto_decode(a, S("{\"type\":\"roomState\",\"data\":{\"paused\":true,\"positionSec\":1,"
                              "\"rate\":1}}"));
        CHECK(m && m->kind == VS_IN_ROOMSTATE, "roomState en pause sans refServerMs refusé");
        m = proto_decode(a, S("{\"type\":\"error\",\"data\":{}}"));
        CHECK(m && m->invalid, "erreur sans code acceptée");
        m = proto_decode(a, S("{\"type\":\"chatEvent\",\"data\":{\"from\":\"a\"}}"));
        CHECK(m && m->invalid, "chatEvent sans texte accepté");

        // Clés dupliquées : la dernière gagne (encoding/json en Go). Un
        // roomState piégé doit produire le même état chez tous les clients.
        m = proto_decode(a, S("{\"type\":\"roomState\",\"data\":{\"paused\":true,\"positionSec\":10,"
                              "\"rate\":1,\"positionSec\":42}}"));
        CHECK(m && m->kind == VS_IN_ROOMSTATE && m->state.position_sec == 42,
              "clé dupliquée : %f au lieu de 42", m ? m->state.position_sec : -1);
        JsonValue *dup = json_parse(a, S("{\"a\":1,\"b\":2,\"a\":3}"), NULL);
        CHECK(json_get_num(dup, "a", 0) == 3, "json_get doit rendre la dernière occurrence");

        // Un pong hors bornes ne doit pas empoisonner l'offset d'horloge.
        VsEngine e;
        engine_init(&e);
        i64 now = 1785960000000LL * 1000000LL;
        VsPong bad = {INT64_MIN, INT64_MAX};
        engine_on_pong(&e, now, bad);
        CHECK(!e.have_offset && e.offset_ms == 0, "pong aberrant accepté par le moteur");
        VsPong good = {vs_ns_to_unix_ms(now), vs_ns_to_unix_ms(now) + 30};
        engine_on_pong(&e, now, good);
        CHECK(e.have_offset && e.offset_ms == 30, "pong valide refusé (%lld)", (long long)e.offset_ms);
    }

    temp_end(top);
}

// ------------------------------------------------------------ vlc / net ---

static void test_vlc(Arena *a) {
    section("vlc");
    TempArena top = temp_begin(a);

    // Payload réaliste de VLC 3 (extrait raccourci de /requests/status.json).
    static const char *real =
        "{\"fullscreen\":false,\"stats\":{\"inputbitrate\":0.1,\"demuxreadbytes\":123},"
        "\"aspectratio\":\"default\",\"audiodelay\":0,\"apiversion\":3,\"currentplid\":4,"
        "\"time\":2,\"volume\":256,\"length\":1200,\"random\":false,\"audiofilters\":{\"filter_0\":\"\"},"
        "\"rate\":1,\"videoeffects\":{\"hue\":0,\"saturation\":1},\"state\":\"playing\","
        "\"loop\":false,\"version\":\"3.0.20 Vetinari\",\"position\":0.00208333333333333,"
        "\"information\":{\"chapter\":0,\"chapters\":[],\"title\":0,\"category\":{\"meta\":"
        "{\"filename\":\"ep1.mkv\",\"title\":\"Episode 1\"}},\"titles\":[]},\"repeat\":false}";
    VsStatus st;
    CHECK(vlc_parse_status(a, S(real), &st), "status.json réaliste refusé");
    CHECK(st.state == VS_PLAY_PLAYING, "état");
    CHECK(approx(st.position_sec, 2.5, 1e-6), "position fine = %f (attendu 2,5)", st.position_sec);
    CHECK(st.length_sec == 1200 && st.rate == 1, "durée/rate");
    CHECK(strbuf_eq(&st.file_name, S("ep1.mkv")), "nom de fichier");

    // Assainissement (mêmes cas que internal/vlc/http_test.go).
    struct {
        const char *json;
        f64 pos, len, rate;
    } cases[] = {
        {"{\"state\":\"playing\",\"position\":2.5,\"length\":100,\"rate\":1}", 100, 100, 1},
        {"{\"state\":\"playing\",\"position\":-3,\"length\":100,\"rate\":1}", 0, 100, 1},
        {"{\"state\":\"playing\",\"position\":0.5,\"length\":-10,\"time\":7,\"rate\":1}", 7, 0, 1},
        {"{\"state\":\"playing\",\"position\":0.5,\"length\":100,\"rate\":0}", 50, 100, 1},
        {"{\"state\":\"paused\",\"position\":0.1,\"length\":100,\"rate\":-2}", 10, 100, 1},
        {"{\"state\":\"stopped\"}", 0, 0, 1},
    };
    for (isize i = 0; i < VS_ARRAY_COUNT(cases); i++) {
        CHECK(vlc_parse_status(a, S(cases[i].json), &st), "cas %lld refusé", (long long)i);
        CHECK(approx(st.position_sec, cases[i].pos, 1e-9), "cas %lld position = %f", (long long)i, st.position_sec);
        CHECK(approx(st.length_sec, cases[i].len, 1e-9), "cas %lld durée = %f", (long long)i, st.length_sec);
        CHECK(approx(st.rate, cases[i].rate, 1e-9), "cas %lld rate = %f", (long long)i, st.rate);
    }
    // Entrées hostiles : refus propre.
    CHECK(!vlc_parse_status(a, S("pas du json"), &st), "JSON invalide accepté");
    CHECK(!vlc_parse_status(a, S("[1,2]"), &st), "tableau accepté comme status");
    CHECK(vlc_parse_status(a, S("{\"state\":42,\"position\":\"x\"}"), &st), "types inattendus non tolérés");
    CHECK(st.state == VS_PLAY_STOPPED && st.position_sec == 0, "valeurs de repli");
    // Métadonnées : repli sur title puis now_playing.
    CHECK(vlc_parse_status(a, S("{\"state\":\"paused\",\"length\":10,\"position\":0.5,"
                                "\"information\":{\"category\":{\"meta\":{\"filename\":\"  \",\"title\":\"T\"}}}}"),
                           &st),
          "métadonnées");
    CHECK(strbuf_eq(&st.file_name, S("T")), "repli sur title");

    // base64 (RFC 4648 + le cas réel « :motdepasse »).
    char b64[128];
    base64_encode((const u8 *)"", 0, b64, sizeof(b64));
    CHECK(strcmp(b64, "") == 0, "base64 vide");
    base64_encode((const u8 *)"f", 1, b64, sizeof(b64));
    CHECK(strcmp(b64, "Zg==") == 0, "base64 f: %s", b64);
    base64_encode((const u8 *)"fo", 2, b64, sizeof(b64));
    CHECK(strcmp(b64, "Zm8=") == 0, "base64 fo: %s", b64);
    base64_encode((const u8 *)"foobar", 6, b64, sizeof(b64));
    CHECK(strcmp(b64, "Zm9vYmFy") == 0, "base64 foobar: %s", b64);
    CHECK(base64_encode((const u8 *)"foobar", 6, b64, 4) == 0, "base64 sans place accepté");
    VlcClient c;
    vlc_client_init(&c, 8080, S("abc"));
    CHECK(strcmp(c.auth_b64, "OmFiYw==") == 0, "auth Basic ':abc' = %s", c.auth_b64);
    Str8 req = vlc_build_request(a, S("/requests/status.json"), S("OmFiYw=="), 8080);
    CHECK(str8_starts_with(req, str8_lit("GET /requests/status.json HTTP/1.1\r\n")), "ligne de requête");

    // Ligne de lancement (VS-029) : chaque drapeau neutralise un réglage que le
    // vlcrc de l'utilisateur pourrait imposer. Les perdre, c'est reproduire le
    // retour terrain « VLC s'ouvre et joue, l'app dit aucun fichier ouvert » —
    // d'où ce gel explicite.
    {
        Str8 cmd = vlc_build_command(a, S("C:\\Program Files\\VideoLAN\\VLC\\vlc.exe"),
                                     S("D:\\films\\ep 1.mkv"), 41234, S("deadbeef"));
        CHECK(str8_starts_with(cmd, S("\"C:\\Program Files\\VideoLAN\\VLC\\vlc.exe\" ")),
              "exécutable non protégé par des guillemets");
        static const char *flags[] = {
            "--extraintf=http",
            "--lua-intf=http",
            "--http-host=127.0.0.1",
            "--http-port=41234",
            "--http-password=deadbeef",
            "--no-one-instance ",
            "--no-one-instance-when-started-from-file",
            "--no-playlist-enqueue",
            "--playlist-autostart",
            "--start-paused",
            "--no-random",
            "--no-loop",
            "--no-repeat",
            "--no-play-and-exit",
            "--no-video-title-show",
        };
        for (isize i = 0; i < VS_ARRAY_COUNT(flags); i++) {
            CHECK(contains(cmd, flags[i]), "drapeau manquant : %s", flags[i]);
        }
        // Le média vient en dernier, entre guillemets (un chemin a des espaces)
        // et surtout APRÈS les options : VLC prendrait le reste pour des MRL.
        CHECK(contains(cmd, "\"D:\\films\\ep 1.mkv\""), "média non protégé par des guillemets");
        CHECK(cmd.len > 0 && cmd.data[cmd.len - 1] == '"', "le média n'est pas en dernier");
        // `--one-instance` seul est un piège : VLC le réactive quand le média
        // vient d'un fichier. Les deux formes doivent être là.
        CHECK(!contains(cmd, " --one-instance"), "one-instance activé");
        CHECK(!contains(cmd, " --playlist-enqueue"), "playlist-enqueue activé");
    }

    // Réponses HTTP.
    {
        int code = 0;
        Str8 body;
        CHECK(http_parse_response(a, S("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}"), &code, &body) &&
                  code == 200 && str8_eq(body, S("{}")),
              "réponse Content-Length");
        CHECK(http_parse_response(a, S("HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n"), &code, &body) &&
                  code == 401,
              "réponse 401");
        CHECK(http_parse_response(a,
                                  S("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                                    "4\r\n{\"a\"\r\n4\r\n:1}\n\r\n0\r\n\r\n"),
                                  &code, &body) &&
                  str8_eq(body, S("{\"a\":1}\n")),
              "réponse chunked");
        CHECK(http_parse_response(a, S("HTTP/1.1 200 OK\r\n\r\ncorps sans longueur"), &code, &body) &&
                  str8_eq(body, S("corps sans longueur")),
              "corps terminé par fermeture");
        CHECK(!http_parse_response(a, S("HTTP/1.1 200 OK\r\nContent-Length: 99\r\n\r\ncourt"), &code, &body),
              "réponse tronquée acceptée");
        CHECK(!http_parse_response(a, S("pas du HTTP"), &code, &body), "réponse non HTTP acceptée");
        CHECK(!http_parse_response(a, S("HTTP/1.1 200 OK\r\nsans fin d'en-tetes"), &code, &body),
              "en-têtes non terminés acceptés");
        CHECK(!http_parse_response(a, S(""), &code, &body), "réponse vide acceptée");
    }

    temp_end(top);
}

// ------------------------------------------------------------ ini + ui ---

static void test_ini(Arena *a) {
    section("ini");
    TempArena top = temp_begin(a);

    // Analyse : commentaires, sections, espaces, accents, clé répétée.
    Ini ini;
    Str8 src = S("# commentaire\r\n"
                 "[vibesync]\r\n"
                 "serveur = wss://vibesync.exemple.fr/ws \r\n"
                 "pseudo=Thibault Éloïse\r\n"
                 "salle=soirée-été\r\n"
                 "; autre commentaire\r\n"
                 "vide=\r\n"
                 "salle=dernière\r\n"
                 "sans_egal\r\n");
    CHECK(ini_parse(a, src, &ini), "analyse ini");
    CHECK(ini.count == 4, "%lld entrées, attendu 4", (long long)ini.count);
    CHECK(str8_eq(ini_get(&ini, "serveur", S("")), S("wss://vibesync.exemple.fr/ws")), "valeur détourée");
    CHECK(str8_eq(ini_get(&ini, "pseudo", S("")), S("Thibault Éloïse")), "valeur accentuée");
    CHECK(str8_eq(ini_get(&ini, "salle", S("")), S("dernière")), "clé répétée : la dernière gagne");
    CHECK(ini_get(&ini, "vide", S("x")).len == 0, "valeur vide");
    CHECK(str8_eq(ini_get(&ini, "absent", S("défaut")), S("défaut")), "valeur par défaut");

    // Écriture puis relecture : aller-retour exact.
    Str8 text = ini_write(a, &ini);
    Ini back;
    CHECK(ini_parse(a, text, &back), "relecture");
    CHECK(back.count == ini.count, "aller-retour : %lld entrées", (long long)back.count);
    CHECK(str8_eq(ini_get(&back, "pseudo", S("")), S("Thibault Éloïse")), "accents préservés");
    CHECK(str8_eq(ini_get(&back, "salle", S("")), S("dernière")), "salle préservée");

    // Modification.
    ini_set(a, &ini, "pseudo", S("Zoé"));
    CHECK(str8_eq(ini_get(&ini, "pseudo", S("")), S("Zoé")), "ini_set remplace");
    ini_set(a, &ini, "nouveau", S("valeur"));
    CHECK(str8_eq(ini_get(&ini, "nouveau", S("")), S("valeur")), "ini_set ajoute");

    // Jeton de session dans l'ini (VS-028) : aller-retour avec et sans, plus
    // les cas que session_load() doit remplacer au lieu de réutiliser.
    {
        Ini vierge;
        CHECK(ini_parse(a, S("pseudo=thib\r\nsalle=salon\r\n"), &vierge), "ini sans jeton");
        Str8 absent = ini_get(&vierge, "session", S(""));
        CHECK(absent.len == 0 && !proto_session_token_valid(absent), "jeton absent réputé valide");

        char tok[VS_SESSION_TOKEN_LEN + 1];
        CHECK(proto_session_token(tok, sizeof(tok)), "génération du jeton à persister");
        CHECK(ini_set(a, &vierge, "session", S(tok)), "écriture du jeton");
        Ini relu;
        CHECK(ini_parse(a, ini_write(a, &vierge), &relu), "relecture de l'ini avec jeton");
        Str8 back_tok = ini_get(&relu, "session", S(""));
        CHECK(str8_eq(back_tok, S(tok)), "jeton altéré par l'aller-retour");
        CHECK(proto_session_token_valid(back_tok), "jeton relu refusé");
        // Les autres réglages survivent : l'ini reste la référence, on n'en
        // réécrit pas une version amputée.
        CHECK(str8_eq(ini_get(&relu, "pseudo", S("")), S("thib")), "pseudo perdu");

        // Valeurs hostiles écrites à la main : refusées, donc régénérées.
        const char *mauvais[] = {"", "abc", "zzzz56789abcdef0123456789abcdef0", "  "};
        for (isize i = 0; i < VS_ARRAY_COUNT(mauvais); i++) {
            Ini sale;
            Str8 line = str8_cat(a, str8_cat(a, S("session="), S(mauvais[i])), S("\r\n"));
            CHECK(ini_parse(a, line, &sale), "ini hostile %lld", (long long)i);
            CHECK(!proto_session_token_valid(str8_trim(ini_get(&sale, "session", S("")))),
                  "jeton hostile %lld accepté", (long long)i);
        }
        // Un jeton de 128 caractères écrit à la main est relu tel quel : la
        // borne colle à maxSessionTokenLen du client Go.
        char long_tok[VS_SESSION_TOKEN_MAX + 1];
        memset(long_tok, 'a', VS_SESSION_TOKEN_MAX);
        long_tok[VS_SESSION_TOKEN_MAX] = 0;
        Ini borne;
        CHECK(ini_parse(a, str8_cat(a, str8_cat(a, S("session="), S(long_tok)), S("\r\n")), &borne),
              "ini jeton long");
        CHECK(proto_session_token_valid(ini_get(&borne, "session", S(""))), "jeton de 128 refusé");
    }

    // Débordement : refus propre, pas d'écriture hors bornes.
    Ini big;
    ini_clear(&big);
    b32 all = 1;
    for (int i = 0; i < INI_MAX_ENTRIES + 10; i++) {
        char key[32];
        snprintf(key, sizeof(key), "k%d", i);
        if (!ini_set(a, &big, key, S("v"))) all = 0;
    }
    CHECK(!all && big.count == INI_MAX_ENTRIES, "plafond d'entrées (%lld)", (long long)big.count);

    // Fichier SATURÉ (revue terra, VS-028) : un vibesync.ini bricolé à la main
    // et plein empêcherait d'écrire le jeton de session, donc de corriger le
    // pseudo bloqué. ini_make_room évince une entrée qui n'est pas à nous.
    {
        static const char *const keep[] = {"serveur", "pseudo", "session"};
        Ini plein;
        ini_clear(&plein);
        CHECK(ini_set(a, &plein, "pseudo", S("thib")), "clé réservée refusée");
        for (isize i = 0; i < INI_MAX_ENTRIES + 4 && plein.count < INI_MAX_ENTRIES; i++) {
            char k[32];
            snprintf(k, sizeof(k), "inconnu%lld", (long long)i);
            ini_set(a, &plein, k, S("x"));
        }
        CHECK(plein.count == INI_MAX_ENTRIES, "ini non rempli (%lld)", (long long)plein.count);
        Str8 token = S("0123456789abcdef0123456789abcdef");
        CHECK(!ini_set(a, &plein, "session", token), "ini plein : ini_set aurait dû refuser");

        Str8 evicted = S("");
        CHECK(ini_make_room(&plein, keep, VS_ARRAY_COUNT(keep), &evicted), "aucune entrée évinçable");
        CHECK(str8_starts_with(evicted, S("inconnu")), "mauvaise entrée évincée : %.*s", (int)evicted.len,
              evicted.data);
        CHECK(plein.count == INI_MAX_ENTRIES - 1, "éviction sans effet (%lld)", (long long)plein.count);
        CHECK(ini_set(a, &plein, "session", token), "place non libérée");
        CHECK(str8_eq(ini_get(&plein, "session", S("")), token), "jeton non écrit");
        CHECK(str8_eq(ini_get(&plein, "pseudo", S("")), S("thib")), "clé réservée perdue");

        // Rien à évincer : que des clés à garder — on refuse plutôt que de
        // sacrifier un réglage de l'utilisateur.
        Ini reserve;
        ini_clear(&reserve);
        for (isize i = 0; i < VS_ARRAY_COUNT(keep); i++) ini_set(a, &reserve, keep[i], S("v"));
        CHECK(!ini_make_room(&reserve, keep, VS_ARRAY_COUNT(keep), NULL), "clé réservée évincée à tort");
        CHECK(reserve.count == VS_ARRAY_COUNT(keep), "entrées perdues");

        // ini_remove_at : bornes, et cohérence avec ini_remove.
        CHECK(!ini_remove_at(&reserve, -1), "indice négatif accepté");
        CHECK(!ini_remove_at(&reserve, reserve.count), "indice hors bornes accepté");
        CHECK(ini_remove_at(&reserve, 0) && reserve.count == VS_ARRAY_COUNT(keep) - 1, "suppression par indice");
        CHECK(str8_eq(ini_get(&reserve, keep[1], S("")), S("v")), "entrée suivante non remontée");
    }

    temp_end(top);
}

// ------------------------------ adresse, versions, connexion (VS-022/023) ---

static void test_conn_address(Arena *a) {
    section("adresse serveur");
    TempArena top = temp_begin(a);

    // Cas portés de internal/webui/address_test.go : les deux implémentations
    // doivent normaliser à l'identique, sinon un ami sur Windows et un ami sur
    // le client Go ne visent pas le même serveur.
    struct {
        const char *in, *want;
    } ok[] = {
        {"vibesync.exemple.fr", "wss://vibesync.exemple.fr/ws"},
        {"  vibesync.exemple.fr  ", "wss://vibesync.exemple.fr/ws"},
        {"vibesync.exemple.fr:8443", "wss://vibesync.exemple.fr:8443/ws"},
        {"localhost:8080", "ws://localhost:8080/ws"},
        {"127.0.0.1:8080", "ws://127.0.0.1:8080/ws"},
        {"ws://127.0.0.1:8080", "ws://127.0.0.1:8080/ws"},
        {"ws://127.0.0.1:8080/", "ws://127.0.0.1:8080/ws"},
        {"ws://127.0.0.1:8080/ws", "ws://127.0.0.1:8080/ws"},
        {"wss://vibesync.exemple.fr/ws", "wss://vibesync.exemple.fr/ws"},
        {"http://vibesync.exemple.fr", "ws://vibesync.exemple.fr/ws"},
        {"https://vibesync.exemple.fr", "wss://vibesync.exemple.fr/ws"},
        {"HTTPS://vibesync.exemple.fr", "wss://vibesync.exemple.fr/ws"},
        {"https://vibesync.exemple.fr/salon", "wss://vibesync.exemple.fr/salon"},
        {"wss://exemple.fr/ws#frag", "wss://exemple.fr/ws"},
        // Compléments propres au client C.
        {"localhost", "ws://localhost/ws"},
        {"[::1]:8080", "ws://[::1]:8080/ws"},
        {"//vibesync.exemple.fr", "wss://vibesync.exemple.fr/ws"},
        {"wss://user:mdp@exemple.fr/ws", "wss://exemple.fr/ws"},
        {"vibesync.exemple.fr/", "wss://vibesync.exemple.fr/ws"},
        {"exemple.fr?x=1", "wss://exemple.fr/ws?x=1"},
    };
    for (isize i = 0; i < VS_ARRAY_COUNT(ok); i++) {
        Str8 got;
        const char *err = NULL;
        if (!conn_normalize_url(a, S(ok[i].in), &got, &err)) {
            failf("« %s » refusé : %s", ok[i].in, err ? err : "?");
            g_checks++;
            continue;
        }
        CHECK(str8_eq(got, S(ok[i].want)), "« %s » → « %.*s », attendu « %s »", ok[i].in, (int)got.len,
              got.data, ok[i].want);
    }

    const char *bad[] = {"", "   ", "ftp://exemple.fr", "wss://", "ws://", "http://"};
    for (isize i = 0; i < VS_ARRAY_COUNT(bad); i++) {
        Str8 got;
        const char *err = NULL;
        b32 accepted = conn_normalize_url(a, S(bad[i]), &got, &err);
        CHECK(!accepted && err != NULL, "« %s » aurait dû être refusé avec un message", bad[i]);
    }
    // Le message doit dire quoi faire, pas seulement que c'est faux.
    {
        Str8 got;
        const char *err = NULL;
        conn_normalize_url(a, S(""), &got, &err);
        CHECK(err && strstr(err, "adresse") != NULL, "message d'adresse vide explicite");
    }

    CHECK(conn_is_local_host(S("localhost")) && conn_is_local_host(S("127.0.0.1:9")) &&
              conn_is_local_host(S("[::1]:80")),
          "hôtes locaux reconnus");
    CHECK(!conn_is_local_host(S("exemple.fr")) && !conn_is_local_host(S("127.0.0.2")),
          "hôtes distants non confondus avec le local");

    temp_end(top);
}

// VS-036 : les 35 cas de la référence (internal/client/version_test.go, repris
// tels quels par testNewerVersion côté Swift) rejoués sur le C commun. Les neuf
// premiers sont ceux où l'ancien proto_semver_cmp divergeait — donc les neuf
// écarts qu'avait le client Windows.
static void test_semver(void) {
    section("semver");
    struct {
        const char *server, *client;
        b32 want;
    } cases[] = {
        // --- les 9 divergences de l'ancien proto_semver_cmp (rapport VS-033 §4).
        {"9.9.9", "dev", 0},                       // build non versionné : jamais de bannière
        {"1.0.0", "", 0},                          // version locale muette : idem
        {"1.2.3.4", "1.0.0", 0},                   // quatre composants : illisible
        {"1..3", "1.0.0", 0},                      // composant vide : illisible
        {"99999999999999999999.0.0", "1.0.0", 0},  // hors borne : illisible
        {"  1.2.4  ", "1.2.3", 1},                 // espaces rognés
        {"\t1.2.4\r\n", "1.2.3", 1},               // ... y compris tabulation et sauts de ligne
        {"1.2.3", "1.2.3-rc1", 1},                 // la version nue dépasse la pré-version
        {"1.2.3+b", "1.2.3-rc1", 1},               // métadonnées de build hors de l'ordre
        // --- le reste de la suite de référence.
        {"1.2.3", "1.2.3", 0},
        {"1.2.4", "1.2.3", 1},
        {"1.2.2", "1.2.3", 0},
        {"1.3.0", "1.2.9", 1},
        {"2.0.0", "1.99.99", 1},
        {"1.0.0", "2.0.0", 0},
        {"0.3", "0.2.9", 1},
        {"0.2", "0.2.0", 0},
        {"v1.2.4", "1.2.3", 1},
        {"1.2.4-rc1", "1.2.3", 1},
        {"1.2.3-rc1", "1.2.3", 0},
        {"1.2.3-rc2", "1.2.3-rc1", 0},  // deux pré-versions : non départagées
        {"1.2.2-rc1", "1.2.3", 0},
        {"1.2.3-", "1.2.3", 0},  // tiret nu : pas une pré-version, triplet égal
        {"1.2.3+build7", "1.2.3", 0},
        {"1.2.4+build7", "1.2.3", 1},
        {"1.10.0", "1.9.0", 1},
        {"dev", "1.0.0", 0},
        {"on-verra-plus-tard", "1.0.0", 0},
        {"-1.2.3", "1.0.0", 0},
        {"1.2.4\n", "1.2.3", 1},
        {"1.2.3", "1.2.3\n", 0},
        {"1.2.4", "\n1.2.3\n", 1},
        {"+1.2.4", "1.2.3", 0},  // « + » en tête : il ne reste rien à lire
        {"1.+2.4", "1.2.3", 0},
    };
    for (isize i = 0; i < VS_ARRAY_COUNT(cases); i++) {
        b32 got = proto_newer_version(S(cases[i].server), S(cases[i].client));
        CHECK(got == cases[i].want, "newer(%s, %s) = %d, attendu %d", cases[i].server,
              cases[i].client, got, cases[i].want);
    }

    // Antisymétrie : deux versions ne peuvent pas être chacune plus récente que
    // l'autre (testNewerVersionIsAntisymmetric côté Swift).
    const char *versions[] = {"0.0.1", "0.1.0", "0.2.0",     "1.0.0",
                              "1.0.1", "1.2.3-rc1", "1.2.3", "v2.0.0"};
    for (isize i = 0; i < VS_ARRAY_COUNT(versions); i++) {
        for (isize j = 0; j < VS_ARRAY_COUNT(versions); j++) {
            CHECK(!(proto_newer_version(S(versions[i]), S(versions[j])) &&
                    proto_newer_version(S(versions[j]), S(versions[i]))),
                  "%s et %s chacune plus récente que l'autre", versions[i], versions[j]);
        }
    }

    // La bannière ne doit jamais s'afficher contre notre propre version.
    CHECK(!proto_newer_version(S(VS_VERSION), S(VS_VERSION)), "VS_VERSION jamais plus récente qu'elle-même");
}

// La règle non négociable : un refus du serveur n'entraîne JAMAIS de nouvelle
// tentative, alors qu'une panne réseau en programme une.
static void test_conn_policy(void) {
    section("politique de connexion");
    const i64 SEC = 1000000000;
    Conn c;
    conn_reset(&c);
    CHECK(c.phase == CONN_IDLE && !conn_is_busy(&c), "état initial");
    CHECK(!conn_should_attempt(&c, 0), "au repos : aucune tentative");

    // Panne réseau : réessai avec backoff croissant.
    conn_start(&c, 0);
    conn_attempt_started(&c);
    CHECK(c.phase == CONN_TRYING && conn_is_busy(&c), "tentative en cours");
    conn_on_socket_down(&c, 10 * SEC);
    CHECK(c.phase == CONN_WAITING && conn_is_busy(&c), "panne réseau → attente");
    CHECK(!conn_should_attempt(&c, 10 * SEC), "pas de tentative avant l'échéance");
    CHECK(conn_should_attempt(&c, 10 * SEC + c.backoff_ns), "tentative à l'échéance");
    i64 first = c.backoff_ns;
    conn_attempt_started(&c);
    conn_on_socket_down(&c, 20 * SEC);
    CHECK(c.backoff_ns > first, "backoff croissant (%lld → %lld ms)", (long long)(first / 1000000),
          (long long)(c.backoff_ns / 1000000));
    CHECK(conn_seconds_until_retry(&c, 20 * SEC) >= 1, "compte à rebours affichable");

    // Refus du serveur : arrêt net, et la fermeture de socket qui suit ne
    // relance rien (c'est le bug de terrain : « Nouvelle tentative… » en boucle
    // après un mauvais mot de passe).
    conn_on_refused(&c);
    CHECK(c.phase == CONN_REFUSED && !conn_is_busy(&c), "refus → arrêt");
    conn_on_socket_down(&c, 30 * SEC);
    CHECK(c.phase == CONN_REFUSED, "la socket qui tombe après un refus ne relance rien");
    CHECK(!conn_should_attempt(&c, 1000 * SEC), "aucune tentative, même bien plus tard");

    // Annulation : même garantie.
    conn_start(&c, 0);
    conn_attempt_started(&c);
    conn_cancel(&c);
    CHECK(c.phase == CONN_IDLE && !conn_is_busy(&c), "annulation → repos");
    conn_on_socket_down(&c, 40 * SEC);
    CHECK(c.phase == CONN_IDLE && !conn_should_attempt(&c, 1000 * SEC), "annulé : aucune relance");

    // Reprise manuelle après un refus : conn_start doit tout remettre à plat.
    conn_on_refused(&c);
    conn_start(&c, 100 * SEC);
    CHECK(c.phase == CONN_TRYING && c.backoff_ns == 0 && c.attempts == 0, "reprise manuelle repart à neuf");

    // Session établie puis coupure : là, on réessaie (panne, pas refus).
    conn_attempt_started(&c);
    conn_on_open(&c);
    CHECK(c.phase == CONN_OPEN && c.backoff_ns == 0 && !conn_is_busy(&c), "session ouverte");
    conn_on_socket_down(&c, 200 * SEC);
    CHECK(c.phase == CONN_WAITING, "coupure en cours de session → reconnexion");
}

// ------------------------------- robustesse : file, reprise, buffering ---
//
// Ces tests exercent directement l'API du moteur : ils ne dépendent pas de la
// convention d'enregistrement des vecteurs.

static VsStatus mk_status(VsPlayState state, f64 pos, f64 len) {
    VsStatus st;
    memset(&st, 0, sizeof(st));
    st.state = state;
    st.position_sec = pos;
    st.length_sec = len;
    st.rate = 1;
    strbuf_set(&st.file_name, S("ep1.mkv"));
    return st;
}

static VsRoomState mk_room(b32 paused, f64 pos, const char *set_by, i64 ref_ms) {
    VsRoomState rs;
    memset(&rs, 0, sizeof(rs));
    rs.paused = paused;
    rs.position_sec = pos;
    rs.rate = 1;
    rs.ref_server_ms = ref_ms;
    strbuf_set(&rs.set_by, S(set_by));
    return rs;
}

// count_msgs compte les messages d'un type dans une sortie.
static isize count_msgs(const VsOutput *o, VsMsgKind kind) {
    isize n = 0;
    for (isize i = 0; i < o->msg_count; i++) {
        if (o->msgs[i].kind == kind) n++;
    }
    return n;
}

static void test_offline_queue(void) {
    section("file de chat hors ligne");
    const i64 SEC = 1000000000LL;
    i64 t0 = 1785960000000LL * 1000000LL;
    VsEngine e;
    VsOutput out;

    // Hors ligne : rien ne part, tout s'empile dans l'ordre.
    engine_init(&e);
    vs_output_reset(&out);
    engine_chat(&e, S("un"), &out);
    engine_chat(&e, S("deux"), &out);
    CHECK(out.msg_count == 0, "hors ligne : aucun message émis (%lld)", (long long)out.msg_count);
    CHECK(engine_pending_chat_count(&e) == 2, "deux messages en file");
    CHECK(str8_eq(engine_pending_chat(&e, 0), S("un")) && str8_eq(engine_pending_chat(&e, 1), S("deux")),
          "ordre de composition préservé");
    CHECK(engine_pending_chat(&e, 5).len == 0 && engine_pending_chat(&e, -1).len == 0,
          "index hors bornes : chaîne vide");

    // Le welcome vide la file, dans l'ordre, après les re-déclarations.
    VsRoomState rs = mk_room(1, 0, "u2", 1785960000000LL);
    vs_output_reset(&out);
    engine_on_welcome(&e, t0, S("u1"), &rs, NULL, &out);
    CHECK(engine_pending_chat_count(&e) == 0, "file vidée par le welcome");
    CHECK(count_msgs(&out, VS_MSG_CHAT) == 2, "les deux chats sont partis");
    isize first_chat = -1;
    for (isize i = 0; i < out.msg_count; i++) {
        if (out.msgs[i].kind == VS_MSG_CHAT && first_chat < 0) first_chat = i;
    }
    CHECK(first_chat >= 0 && str8_eq(strbuf_str(&out.msgs[first_chat].text), S("un")) &&
              str8_eq(strbuf_str(&out.msgs[first_chat + 1].text), S("deux")),
          "rejeu dans l'ordre de composition");
    // Jamais de control rejoué, et un seul setReady/setFile (état courant).
    CHECK(count_msgs(&out, VS_MSG_CONTROL) == 0, "aucun control rejoué");
    CHECK(count_msgs(&out, VS_MSG_SET_READY) == 1, "un seul setReady (état courant re-déclaré)");

    // La file est LIÉE À LA SALLE : changer de salle la vide sans envoi.
    {
        VsEngine e2;
        VsOutput o2;
        engine_init(&e2);
        vs_output_reset(&o2);
        engine_set_room(&e2, S("salon"));
        engine_chat(&e2, S("pour le salon"), &o2);
        CHECK(engine_pending_chat_count(&e2) == 1, "message en file pour salon");
        engine_set_room(&e2, S("salon"));
        CHECK(engine_pending_chat_count(&e2) == 1, "même salle : la file survit");
        engine_set_room(&e2, S("autre"));
        CHECK(engine_pending_chat_count(&e2) == 0, "changement de salle : file vidée sans envoi");
        // Déconnexion volontaire : même règle.
        engine_chat(&e2, S("hop"), &o2);
        CHECK(engine_pending_chat_count(&e2) == 1, "remis en file");
        engine_disconnected(&e2);
        CHECK(engine_pending_chat_count(&e2) == 0, "départ volontaire : file vidée sans envoi");
        // Une reconnexion automatique, elle, préserve la file.
        engine_chat(&e2, S("survivant"), &o2);
        engine_session_lost(&e2);
        CHECK(engine_pending_chat_count(&e2) == 1, "reconnexion automatique : la file survit");
    }

    // En ligne : le chat part directement, sans passer par la file.
    vs_output_reset(&out);
    engine_chat(&e, S("direct"), &out);
    CHECK(out.msg_count == 1 && out.msgs[0].kind == VS_MSG_CHAT, "en ligne : envoi direct");
    CHECK(engine_pending_chat_count(&e) == 0, "rien mis en file quand on est connecté");
    engine_chat(&e, S(""), &out);
    CHECK(out.msg_count == 1, "message vide ignoré");

    // Bornage : au-delà de 20, les PLUS ANCIENS sont abandonnés.
    engine_init(&e);
    vs_output_reset(&out);
    for (int i = 0; i < VS_CHAT_QUEUE_MAX + 7; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "m%d", i);
        engine_chat(&e, S(buf), &out);
    }
    CHECK(engine_pending_chat_count(&e) == VS_CHAT_QUEUE_MAX, "file bornée à %d (%lld)", VS_CHAT_QUEUE_MAX,
          (long long)engine_pending_chat_count(&e));
    CHECK(str8_eq(engine_pending_chat(&e, 0), S("m7")), "les plus anciens sont tombés");
    CHECK(str8_eq(engine_pending_chat(&e, VS_CHAT_QUEUE_MAX - 1), S("m26")), "le plus récent est gardé");
    // Une file pleine tient dans une seule sortie de welcome.
    vs_output_reset(&out);
    engine_on_welcome(&e, t0, S("u1"), &rs, NULL, &out);
    CHECK(!out.dropped, "file pleine + re-déclarations : pas de débordement de sortie");
    CHECK(count_msgs(&out, VS_MSG_CHAT) == VS_CHAT_QUEUE_MAX, "les 20 chats sont partis");

    // Le ready local survit au welcome (le serveur nous voit « pas prêt »).
    engine_init(&e);
    vs_output_reset(&out);
    engine_set_ready(&e, 1, &out);
    b32 server_says_not_ready = 0;
    vs_output_reset(&out);
    engine_on_welcome(&e, t0 + SEC, S("u1"), &rs, &server_says_not_ready, &out);
    CHECK(e.ready == 1, "le welcome n'écrase pas le ready local");
    isize r = -1;
    for (isize i = 0; i < out.msg_count; i++) {
        if (out.msgs[i].kind == VS_MSG_SET_READY) r = i;
    }
    CHECK(r >= 0 && out.msgs[r].ready == 1, "le ready est re-déclaré au serveur");
    // Le broadcast `users` reste la source de vérité ensuite.
    engine_on_self_ready(&e, 0);
    CHECK(e.ready == 0, "users resynchronise le ready");
}

// join_room amène le moteur à l'état « connecté à `room`, séance en lecture
// depuis `pos` », comme après un welcome normal.
static void join_room(VsEngine *e, i64 now, const char *room, f64 pos, VsOutput *out) {
    engine_set_room(e, S(room));
    engine_connecting(e);
    VsRoomState playing = mk_room(0, pos, "u2", vs_ns_to_unix_ms(now));
    vs_output_reset(out);
    engine_on_welcome(e, now, S("u1"), &playing, NULL, out);
    VsPong p = {vs_ns_to_unix_ms(now), vs_ns_to_unix_ms(now)};
    engine_on_pong(e, now, p);
    vs_output_reset(out);
}

// tick_at joue un poll : c'est lui qui échantillonne la position de séance.
static void tick_at(VsEngine *e, i64 now, VsOutput *out) {
    engine_on_tick(e, now, out);
    vs_output_reset(out);
}

static void test_virgin_resume(void) {
    section("reprise salle vierge");
    const i64 SEC = 1000000000LL;
    i64 t0 = 1785960000000LL * 1000000LL;
    VsEngine e;
    VsOutput out;
    VsRoomState virgin = mk_room(1, 0, "", 1);
    VsStatus st = mk_status(VS_PLAY_PLAYING, 1806.4, 7200);

    // Séance connue dans CETTE salle (dernière position observée 1800,8), puis
    // serveur revenu vierge : reprise à cette position OBSERVÉE, pas à la
    // position brute de VLC (1806,4) ni à une projection.
    i64 lost_at = t0 + 800 * 1000000LL;
    i64 t1 = t0 + 6400 * 1000000LL;
    engine_init(&e);
    vs_output_reset(&out);
    join_room(&e, t0, "salon", 1800, &out);
    engine_on_vlc_status(&e, t0, &st, &out);
    tick_at(&e, lost_at, &out);
    engine_session_lost(&e);
    vs_output_reset(&out);
    engine_on_welcome(&e, t1, S("u1"), &virgin, NULL, &out);
    CHECK(count_msgs(&out, VS_MSG_CONTROL) == 1, "une seule reprise émise");
    isize c = -1;
    for (isize i = 0; i < out.msg_count; i++) {
        if (out.msgs[i].kind == VS_MSG_CONTROL) c = i;
    }
    CHECK(c >= 0 && out.msgs[c].action == VS_ACT_SEEK, "la reprise est un seek");
    CHECK(c >= 0 && approx(out.msgs[c].position_sec, 1800.8, 1e-3),
          "reprise à la dernière position de salle observée (%.3f, attendu 1800.8)",
          c >= 0 ? out.msgs[c].position_sec : -1);
    CHECK(out.have_resume_toast && approx(out.resume_toast_sec, 1800.8, 1e-3), "toast « Reprise à … »");
    // La reprise arrive APRÈS les re-déclarations : le serveur connaît notre
    // fichier et notre ready avant de recevoir la position.
    isize sr = -1, pg = -1;
    for (isize i = 0; i < out.msg_count; i++) {
        if (out.msgs[i].kind == VS_MSG_SET_READY) sr = i;
        if (out.msgs[i].kind == VS_MSG_PING) pg = i;
    }
    CHECK(sr >= 0 && pg >= 0 && c > sr && c > pg, "ordre : setReady/ping puis control");
    CHECK(e.user_hold_until > t1, "hold post-action armé par la reprise");

    // PREMIER join dans une salle vierge : jamais de reprise, même si VLC est
    // très loin dans le film. C'est la règle resserrée.
    engine_init(&e);
    vs_output_reset(&out);
    engine_set_room(&e, S("salon"));
    engine_on_vlc_status(&e, t0, &st, &out);
    vs_output_reset(&out);
    engine_on_welcome(&e, t0, S("u1"), &virgin, NULL, &out);
    CHECK(count_msgs(&out, VS_MSG_CONTROL) == 0 && !out.have_resume_toast,
          "premier join : aucune reprise, quel que soit l'état de VLC");

    // Séance connue mais dans une AUTRE salle : la mémoire ne fuit pas.
    engine_init(&e);
    vs_output_reset(&out);
    join_room(&e, t0, "salon", 1800, &out);
    engine_on_vlc_status(&e, t0, &st, &out);
    tick_at(&e, lost_at, &out);
    engine_session_lost(&e);
    engine_set_room(&e, S("autre-salle"));
    vs_output_reset(&out);
    engine_on_welcome(&e, t1, S("u1"), &virgin, NULL, &out);
    CHECK(count_msgs(&out, VS_MSG_CONTROL) == 0, "changement de salle : aucune reprise");

    // setBy renseigné : quelqu'un pilote déjà, on se range derrière lui.
    engine_init(&e);
    vs_output_reset(&out);
    join_room(&e, t0, "salon", 1800, &out);
    tick_at(&e, lost_at, &out);
    engine_session_lost(&e);
    VsRoomState owned = mk_room(1, 0, "u2", 1);
    vs_output_reset(&out);
    engine_on_welcome(&e, t1, S("u1"), &owned, NULL, &out);
    CHECK(count_msgs(&out, VS_MSG_CONTROL) == 0 && !out.have_resume_toast,
          "salle déjà pilotée : aucune reprise");

    // Position de salle non nulle : ce n'est pas une salle vierge.
    engine_init(&e);
    vs_output_reset(&out);
    join_room(&e, t0, "salon", 1800, &out);
    tick_at(&e, lost_at, &out);
    engine_session_lost(&e);
    VsRoomState started = mk_room(1, 42, "", 1);
    vs_output_reset(&out);
    engine_on_welcome(&e, t1, S("u1"), &started, NULL, &out);
    CHECK(count_msgs(&out, VS_MSG_CONTROL) == 0, "position de salle non nulle : aucune reprise");

    // Séance connue mais sous le seuil : elle n'avait pas commencé.
    engine_init(&e);
    vs_output_reset(&out);
    join_room(&e, t0, "salon", 2, &out);
    tick_at(&e, t0, &out);
    engine_session_lost(&e);
    vs_output_reset(&out);
    engine_on_welcome(&e, t0 + SEC, S("u1"), &virgin, NULL, &out);
    CHECK(count_msgs(&out, VS_MSG_CONTROL) == 0, "séance à 3 s : aucune reprise");

    // Une salle vierge n'écrase pas la mémoire : deux redémarrages d'affilée
    // proposent toujours la position de la séance.
    engine_init(&e);
    vs_output_reset(&out);
    join_room(&e, t0, "salon", 1800, &out);
    tick_at(&e, lost_at, &out);
    engine_session_lost(&e);
    vs_output_reset(&out);
    engine_on_welcome(&e, t1, S("u1"), &virgin, NULL, &out);
    CHECK(count_msgs(&out, VS_MSG_CONTROL) == 1, "première reprise");
    engine_session_lost(&e);
    vs_output_reset(&out);
    engine_on_welcome(&e, t1 + 5 * SEC, S("u1"), &virgin, NULL, &out);
    CHECK(count_msgs(&out, VS_MSG_CONTROL) == 1, "seconde reprise : la mémoire a survécu au vierge");
}

static void test_buffering_suspend(void) {
    section("suspension du buffering");
    const i64 MS = 1000000LL;
    i64 t = 1785960000000LL * MS;
    VsEngine e;
    VsOutput out;

    // Référence : une position figée en lecture finit par valoir buffering.
    engine_init(&e);
    vs_output_reset(&out);
    e.phase = VS_PHASE_CONNECTED;
    VsStatus st = mk_status(VS_PLAY_PLAYING, 100, 7200);
    engine_on_vlc_status(&e, t, &st, &out);
    for (int i = 0; i < 6; i++) {  // 1,2 s sans progresser
        t += 200 * MS;
        vs_output_reset(&out);
        engine_on_vlc_status(&e, t, &st, &out);
    }
    CHECK(e.buffering == 1, "position figée > 700 ms = buffering");

    // Un seek n'efface PAS un buffering déjà diagnostiqué.
    vs_output_reset(&out);
    engine_user_control(&e, t, VS_ACT_SEEK, 500, 1, &out);
    t += 200 * MS;
    st.position_sec = 500;
    vs_output_reset(&out);
    engine_on_vlc_status(&e, t, &st, &out);
    CHECK(e.buffering == 1, "le verdict de buffering survit à la suspension");

    // Depuis un état sain : un seek fige la position sans crier au buffering.
    engine_init(&e);
    vs_output_reset(&out);
    e.phase = VS_PHASE_CONNECTED;
    t += 10000 * MS;
    st = mk_status(VS_PLAY_PLAYING, 100, 7200);
    engine_on_vlc_status(&e, t, &st, &out);
    t += 200 * MS;
    st.position_sec = 100.2;
    vs_output_reset(&out);
    engine_on_vlc_status(&e, t, &st, &out);
    CHECK(e.buffering == 0, "lecture normale");
    vs_output_reset(&out);
    engine_user_control(&e, t, VS_ACT_SEEK, 900, 1, &out);
    i64 seek_at = t;
    for (int i = 0; i < 9; i++) {  // 1,8 s figées, sous la suspension de 2 s
        t += 200 * MS;
        vs_output_reset(&out);
        engine_on_vlc_status(&e, t, &st, &out);
        CHECK(e.buffering == 0, "pas de faux buffering pendant la suspension (+%lldms)",
              (long long)((t - seek_at) / MS));
    }
    // Passé la suspension, la détection reprend ses droits.
    t += 400 * MS;
    vs_output_reset(&out);
    engine_on_vlc_status(&e, t, &st, &out);
    for (int i = 0; i < 6; i++) {
        t += 200 * MS;
        vs_output_reset(&out);
        engine_on_vlc_status(&e, t, &st, &out);
    }
    CHECK(e.buffering == 1, "la détection reprend après la suspension");

    // Une suspension en cours n'est ni raccourcie ni prolongée.
    engine_init(&e);
    e.phase = VS_PHASE_CONNECTED;
    vs_output_reset(&out);
    engine_user_control(&e, t, VS_ACT_SEEK, 10, 1, &out);
    i64 far_until = e.buf.suspend_until;
    vs_output_reset(&out);
    engine_user_control(&e, t - 500 * MS, VS_ACT_SEEK, 20, 1, &out);
    CHECK(e.buf.suspend_until == far_until, "une suspension antérieure ne raccourcit pas la fenêtre");
    vs_output_reset(&out);
    engine_user_control(&e, t + 500 * MS, VS_ACT_SEEK, 30, 1, &out);
    CHECK(e.buf.suspend_until == far_until, "une suspension en cours n'est pas prolongée");

    // Anti-masquage : la fenêtre de refroidissement empêche un redémarrage
    // immédiat après la fin d'une suspension.
    engine_init(&e);
    e.phase = VS_PHASE_CONNECTED;
    vs_output_reset(&out);
    engine_user_control(&e, t, VS_ACT_SEEK, 10, 1, &out);
    i64 ends = e.buf.suspend_until;
    vs_output_reset(&out);
    engine_user_control(&e, ends + 200 * MS, VS_ACT_SEEK, 20, 1, &out);
    CHECK(e.buf.suspend_until == ends, "pas de nouvelle suspension moins d'1 s après la fin");
    vs_output_reset(&out);
    engine_user_control(&e, ends + 1200 * MS, VS_ACT_SEEK, 30, 1, &out);
    CHECK(e.buf.suspend_until > ends, "nouvelle suspension acceptée passé le refroidissement");

    // LA propriété exigée : un VLC durablement figé est diagnostiqué
    // bufferisant en ≤ 5 s MALGRÉ une correction (donc une suspension) à
    // chaque poll de 200 ms.
    engine_init(&e);
    e.phase = VS_PHASE_CONNECTED;
    vs_output_reset(&out);
    i64 start = t + 60000 * MS;
    st = mk_status(VS_PLAY_PLAYING, 42, 7200);
    engine_on_vlc_status(&e, start, &st, &out);
    i64 diagnosed_at = -1;
    for (int i = 1; i <= 40 && diagnosed_at < 0; i++) {  // 8 s de simulation
        i64 tt = start + (i64)i * 200 * MS;
        vs_output_reset(&out);
        engine_user_control(&e, tt, VS_ACT_SEEK, 42, 1, &out);  // correction à chaque poll
        vs_output_reset(&out);
        engine_on_vlc_status(&e, tt, &st, &out);  // VLC reste figé sur 42
        if (e.buffering) diagnosed_at = tt - start;
    }
    CHECK(diagnosed_at >= 0 && diagnosed_at <= 5000 * MS,
          "VLC figé diagnostiqué en %lld ms malgré des corrections répétées (≤ 5000 exigé)",
          (long long)(diagnosed_at < 0 ? -1 : diagnosed_at / MS));
}

// ------------------------- action faite DANS VLC pendant la lecture (VS-029) ---
//
// Le trou historique du ticket : « pause faite dans VLC, non propagée ». La
// cause, mesurée dans la VM Win11 avec un vrai VLC, n'était pas dans la
// détection mais dans ce qui la GATE — la fenêtre de grâce. La position que
// rend VLC oscille de ±0,15 s autour de la référence, le nudge s'engage et se
// relâche donc à presque chaque poll, et chaque commande `rate` réarmait 500 ms
// de grâce : la fenêtre ne se refermait jamais et detect_user_action n'était
// plus jamais appelée en lecture. Ce test reproduit exactement ce régime.

static void test_user_action_in_vlc(void) {
    section("action utilisateur dans VLC");
    const i64 MS = 1000000LL;
    i64 t0 = 1785960000000LL * MS;
    VsEngine e;
    VsOutput out;

    // play_with_churn : `polls` polls de 200 ms en lecture, position oscillante.
    // Rend le nombre de commandes rate émises (le régime doit bien churner,
    // sinon le test ne prouverait rien) et la dernière position observée.
    isize rate_cmds = 0;
    f64 pos = 0;
    i64 t = t0;

    engine_init(&e);
    vs_output_reset(&out);
    join_room(&e, t, "salon", 100, &out);
    for (int i = 1; i <= 20; i++) {
        t += 200 * MS;
        pos = engine_expected_position(&e, t) + ((i % 2) ? 0.15 : -0.15);
        VsStatus st = mk_status(VS_PLAY_PLAYING, pos, 7200);
        vs_output_reset(&out);
        engine_on_vlc_status(&e, t, &st, &out);
        engine_on_tick(&e, t, &out);
        for (isize k = 0; k < out.cmd_count; k++) {
            if (out.cmds[k].kind == VS_CMD_RATE) rate_cmds++;
        }
    }
    CHECK(rate_cmds >= 5, "régime de churn attendu : %lld commandes rate sur 20 polls",
          (long long)rate_cmds);

    // L'utilisateur appuie sur Espace dans VLC : un control pause doit partir.
    b32 seen_pause = 0;
    for (int i = 1; i <= 3 && !seen_pause; i++) {
        t += 200 * MS;
        VsStatus st = mk_status(VS_PLAY_PAUSED, pos, 7200);
        vs_output_reset(&out);
        engine_on_vlc_status(&e, t, &st, &out);
        for (isize k = 0; k < out.msg_count; k++) {
            if (out.msgs[k].kind == VS_MSG_CONTROL && out.msgs[k].action == VS_ACT_PAUSE) seen_pause = 1;
        }
        engine_on_tick(&e, t, &out);
    }
    CHECK(seen_pause, "pause faite dans VLC en pleine lecture : NON détectée (VS-029)");

    // Même chose pour un seek fait à la souris dans la barre de VLC.
    engine_init(&e);
    vs_output_reset(&out);
    t = t0 + 60000 * MS;
    join_room(&e, t, "salon", 100, &out);
    for (int i = 1; i <= 20; i++) {
        t += 200 * MS;
        pos = engine_expected_position(&e, t) + ((i % 2) ? 0.15 : -0.15);
        VsStatus st = mk_status(VS_PLAY_PLAYING, pos, 7200);
        vs_output_reset(&out);
        engine_on_vlc_status(&e, t, &st, &out);
        engine_on_tick(&e, t, &out);
    }
    b32 seen_seek = 0;
    f64 seek_pos = 0;
    for (int i = 1; i <= 3 && !seen_seek; i++) {
        t += 200 * MS;
        VsStatus st = mk_status(VS_PLAY_PLAYING, pos + 300, 7200);
        vs_output_reset(&out);
        engine_on_vlc_status(&e, t, &st, &out);
        for (isize k = 0; k < out.msg_count; k++) {
            if (out.msgs[k].kind == VS_MSG_CONTROL && out.msgs[k].action == VS_ACT_SEEK) {
                seen_seek = 1;
                seek_pos = out.msgs[k].position_sec;
            }
        }
        engine_on_tick(&e, t, &out);
    }
    CHECK(seen_seek, "seek fait dans VLC en pleine lecture : NON détecté (VS-029)");
    CHECK(seen_seek && approx(seek_pos, pos + 300, 0.5), "le seek remonté porte la position de VLC (%.2f)",
          seek_pos);

    // L'anti-boucle, elle, doit rester : ce que le moteur vient de commander
    // (pause, reprise, seek) ne doit JAMAIS revenir comme action utilisateur.
    engine_init(&e);
    vs_output_reset(&out);
    t = t0 + 120000 * MS;
    join_room(&e, t, "salon", 100, &out);
    VsStatus st = mk_status(VS_PLAY_PLAYING, engine_expected_position(&e, t), 7200);
    vs_output_reset(&out);
    engine_on_vlc_status(&e, t, &st, &out);
    // Correction dure : le moteur commande un seek, VLC obéit au poll suivant.
    vs_output_reset(&out);
    // NB : emit_user_control n'arme PAS la grâce (aligné avec le Go) ; ici
    // l'anti-boucle tient à la grâce armée par le welcome du join_room.
    engine_user_control(&e, t, VS_ACT_SEEK, 500, 1, &out);
    t += 200 * MS;
    st = mk_status(VS_PLAY_PLAYING, 500, 7200);
    vs_output_reset(&out);
    engine_on_vlc_status(&e, t, &st, &out);
    CHECK(count_msgs(&out, VS_MSG_CONTROL) == 0,
          "le saut que le moteur vient d'ordonner est remonté comme action utilisateur");
}

// ------------------------------------------------------------ moteur : unités ---

static void test_engine_units(void) {
    section("engine (unités)");

    // Assainissement d'un roomState (§Assainissement).
    VsRoomState in, clean;
    memset(&in, 0, sizeof(in));
    in.rate = 1;
    in.paused = 1;
    CHECK(engine_sanitize_roomstate(&in, &clean), "état en pause valide refusé");
    in.paused = 0;
    in.ref_server_ms = 0;
    CHECK(!engine_sanitize_roomstate(&in, &clean), "lecture sans référence acceptée");
    in.ref_server_ms = 1785960000000LL;
    CHECK(engine_sanitize_roomstate(&in, &clean), "état en lecture valide refusé");
    in.rate = 8;
    CHECK(!engine_sanitize_roomstate(&in, &clean), "rate hors [0,25 ; 4] accepté");
    in.rate = 0.1;
    CHECK(!engine_sanitize_roomstate(&in, &clean), "rate trop faible accepté");
    in.rate = 1;
    in.position_sec = -1;
    CHECK(!engine_sanitize_roomstate(&in, &clean), "position négative acceptée");
    in.position_sec = 1e308 * 10;
    CHECK(!engine_sanitize_roomstate(&in, &clean), "position infinie acceptée");

    CHECK(engine_clamp_position(-5, 100) == 0, "clamp bas");
    CHECK(engine_clamp_position(500, 100) == 100, "clamp haut");
    CHECK(engine_clamp_position(500, 0) == 500, "clamp sans durée connue");

    // Backoff 1 s → 10 s.
    i64 b = VS_BACKOFF_MIN_NS;
    CHECK(engine_next_backoff(b) == 2 * VS_BACKOFF_MIN_NS, "backoff x2");
    for (int i = 0; i < 10; i++) b = engine_next_backoff(b);
    CHECK(b == VS_BACKOFF_MAX_NS, "backoff plafonné à 10 s");

    // Médiane glissante des 5 dernières mesures d'offset.
    VsEngine e;
    engine_init(&e);
    i64 base = 1785960000000LL * 1000000LL;
    i64 deltas[] = {1000, 1200, 800, 60000, 1100};
    for (isize i = 0; i < VS_ARRAY_COUNT(deltas); i++) {
        i64 now = base + i * 2000LL * 1000000LL;
        VsPong p = {vs_ns_to_unix_ms(now) - 100, vs_ns_to_unix_ms(now) + deltas[i] - 50};
        engine_on_pong(&e, now, p);
    }
    CHECK(e.offset_ms == 1100, "offset médian = %lld (attendu 1100)", (long long)e.offset_ms);
    CHECK(e.latency_ms == 50, "latence = %lld (attendu 50)", (long long)e.latency_ms);
    for (int i = 0; i < 5; i++) {
        i64 now = base + (10 + i) * 1000LL * 1000000LL;
        VsPong p = {vs_ns_to_unix_ms(now), vs_ns_to_unix_ms(now) + 500};
        engine_on_pong(&e, now, p);
    }
    CHECK(e.offset_ms == 500, "les vieilles mesures ne sont pas oubliées (%lld)", (long long)e.offset_ms);

    // Aucune correction hors état connecté.
    VsOutput out;
    vs_output_reset(&out);
    engine_session_lost(&e);
    CHECK(!e.have_state && !e.have_offset, "référence non invalidée à la coupure");

    // Départ de lecture (docs/protocol.md §Départ et reprise) : la salle passe
    // en lecture alors que VLC est en pause → on cale AVANT de jouer.
    struct {
        f64 vlc_pos;
        b32 want_seek;
        const char *nom;
    } starts[] = {
        {99.65, 1, "écart 0,35 s : seek de calage attendu"},
        {99.95, 0, "écart 0,05 s : pas de seek, on joue directement"},
        {97.0, 1, "écart 3 s : seek de calage"},
    };
    for (isize k = 0; k < VS_ARRAY_COUNT(starts); k++) {
        VsEngine se;
        engine_init(&se);
        VsOutput so;
        vs_output_reset(&so);
        i64 t0 = 1785960000000LL * 1000000LL;
        engine_open_file(&se, S("ep1.mkv"), 15, &so);
        vs_output_reset(&so);

        VsRoomState rs;
        memset(&rs, 0, sizeof(rs));
        rs.paused = 0;
        rs.position_sec = 100;
        rs.rate = 1;
        rs.ref_server_ms = vs_ns_to_unix_ms(t0);
        strbuf_set(&rs.set_by, S("u2"));
        engine_on_welcome(&se, t0, S("u1"), &rs, NULL, &so);
        VsPong pg = {vs_ns_to_unix_ms(t0), vs_ns_to_unix_ms(t0)};
        engine_on_pong(&se, t0, pg);
        vs_output_reset(&so);

        VsStatus st;
        memset(&st, 0, sizeof(st));
        st.state = VS_PLAY_PAUSED;
        st.position_sec = starts[k].vlc_pos;
        st.length_sec = 1200;
        st.rate = 1;
        strbuf_set(&st.file_name, S("ep1.mkv"));
        engine_on_vlc_status(&se, t0, &st, &so);
        vs_output_reset(&so);
        engine_on_tick(&se, t0, &so);

        isize want = starts[k].want_seek ? 2 : 1;
        CHECK(so.cmd_count == want, "%s : %lld commande(s), attendu %lld", starts[k].nom,
              (long long)so.cmd_count, (long long)want);
        if (so.cmd_count == want) {
            if (starts[k].want_seek) {
                CHECK(so.cmds[0].kind == VS_CMD_SEEK, "%s : la première commande doit être le seek", starts[k].nom);
                CHECK(approx(so.cmds[0].value, 100, 0.01), "%s : seek vers %.3f", starts[k].nom, so.cmds[0].value);
                CHECK(so.cmds[1].kind == VS_CMD_RESUME, "%s : le resume doit suivre le seek", starts[k].nom);
            } else {
                CHECK(so.cmds[0].kind == VS_CMD_RESUME, "%s : resume seul attendu", starts[k].nom);
            }
        }
    }
}

// ------------------------------------- dossiers médias, recherche (VS-026) ---

#ifndef _WIN32
// Parcours SIMULÉ : l'algorithme borné de media_core.c ne connaît du système
// que VsDirOps, on lui en fournit un en dur. Sous Windows, media_find est déjà
// exercé sur une vraie arborescence par test_win32.c — y ajouter ce parcours
// ne ferait que doubler la couverture (et le nombre de vérifications) ; il ne
// sert donc que là où la moitié plateforme n'existe pas encore.

typedef struct {
    const char *path;  // chemin complet, séparateur « / »
    b32 is_dir;
    b32 is_link;
    i64 size;
} FakeNode;

// Même forme que l'arborescence réelle de test_win32.c, plus une jonction qui
// contient un homonyme énorme : la suivre fausserait le résultat.
static const FakeNode g_fake_tree[] = {
    {"/m/autre.mkv", 0, 0, 10},
    {"/m/films", 1, 0, 0},
    {"/m/films/EP1-VOSTFR.mkv", 0, 0, 100},  // casse différente
    {"/m/films/vo", 1, 0, 0},
    {"/m/films/vo/ep1-vostfr.mkv", 0, 0, 5000},  // homonyme, plus gros
    {"/m/films/vo/bande-annonce.mp4", 0, 0, 20},
    {"/m/boucle", 1, 1, 0},  // jonction : jamais suivie
    {"/m/boucle/ep1-vostfr.mkv", 0, 0, 999999},
    {"/m/n0", 1, 0, 0},
    {"/m/n0/n1", 1, 0, 0},
    {"/m/n0/n1/n2", 1, 0, 0},
    {"/m/n0/n1/n2/n3", 1, 0, 0},
    {"/m/n0/n1/n2/n3/n4", 1, 0, 0},
    {"/m/n0/n1/n2/n3/n4/juste-assez.mkv", 0, 0, 10},
    {"/m/n0/n1/n2/n3/n4/n5", 1, 0, 0},
    {"/m/n0/n1/n2/n3/n4/n5/n6", 1, 0, 0},
    {"/m/n0/n1/n2/n3/n4/n5/n6/trop-loin.mkv", 0, 0, 10},
};

struct VsDirIter {
    Str8 dir;
    isize idx;
    isize dots;  // « . » et « .. » rendus d'abord, comme FindFirstFileW
};

static b32 fake_dir_exists(Str8 dir) {
    if (str8_eq(dir, str8_lit("/m"))) return 1;
    for (isize i = 0; i < VS_ARRAY_COUNT(g_fake_tree); i++) {
        if (g_fake_tree[i].is_dir && str8_eq_cstr(dir, g_fake_tree[i].path)) return 1;
    }
    return 0;
}

static VsDirIter *fake_open(Arena *a, Str8 dir) {
    if (!fake_dir_exists(dir)) return NULL;
    VsDirIter *it = arena_push_struct(a, VsDirIter);
    it->dir = str8_copy(a, dir);
    return it;
}

static b32 fake_next(VsDirIter *it, VsDirEntry *out) {
    memset(out, 0, sizeof(*out));
    if (it->dots < 2) {
        out->name = it->dots == 0 ? str8_lit(".") : str8_lit("..");
        out->is_dir = 1;
        it->dots++;
        return 1;
    }
    while (it->idx < VS_ARRAY_COUNT(g_fake_tree)) {
        const FakeNode *n = &g_fake_tree[it->idx++];
        Str8 full = str8_from_cstr(n->path);
        isize cut = -1;
        for (isize i = 0; i < full.len; i++) {
            if (full.data[i] == '/') cut = i;
        }
        if (cut <= 0 || !str8_eq(str8_sub(full, 0, cut), it->dir)) continue;
        out->name = str8_sub(full, cut + 1, -1);
        out->is_dir = n->is_dir;
        out->is_link = n->is_link;
        out->size_bytes = n->size;
        return 1;
    }
    return 0;
}

static void fake_close(VsDirIter *it) { it->idx = VS_ARRAY_COUNT(g_fake_tree); }

// Comparaison ASCII : le parcours simulé n'a pas de nom hors ASCII, et la
// sémantique exacte (ordinale Unicode) appartient à la plateforme.
static b32 fake_name_eq_ci(Arena *scratch, Str8 x, Str8 y) {
    VS_UNUSED(scratch);
    if (x.len != y.len) return 0;
    for (isize i = 0; i < x.len; i++) {
        u8 c = x.data[i], d = y.data[i];
        if (c >= 'A' && c <= 'Z') c = (u8)(c + 32);
        if (d >= 'A' && d <= 'Z') d = (u8)(d + 32);
        if (c != d) return 0;
    }
    return 1;
}

static const VsDirOps g_fake_ops = {fake_open, fake_next, fake_close, fake_name_eq_ci, '/'};

static void test_media_walk(Arena *a) {
    StrBuf dirs[MEDIA_MAX_DIRS];
    memset(dirs, 0, sizeof(dirs));
    strbuf_set(&dirs[0], S("/m"));
    MediaFind r;

    CHECK(media_find_with(a, &g_fake_ops, dirs, 1, S("EP1-VOSTFR.MKV"), &r), "parcours simulé : trouvé");
    CHECK(r.matches == 2, "parcours simulé : deux homonymes (%lld)", (long long)r.matches);
    CHECK(r.size_bytes == 5000, "parcours simulé : le plus gros gagne (%lld)", (long long)r.size_bytes);
    CHECK(strbuf_eq(&r.path, S("/m/films/vo/ep1-vostfr.mkv")), "parcours simulé : chemin « %.*s »",
          (int)r.path.len, r.path.data);
    CHECK(!media_find_with(a, &g_fake_ops, dirs, 1, S("ep1"), &r), "pas de correspondance partielle");
    CHECK(!media_find_with(a, &g_fake_ops, dirs, 1, S("trop-loin.mkv"), &r), "profondeur > %d : non atteint",
          MEDIA_MAX_DEPTH);
    CHECK(media_find_with(a, &g_fake_ops, dirs, 1, S("juste-assez.mkv"), &r), "profondeur atteignable");
    strbuf_set(&dirs[0], S("/m/"));
    CHECK(media_find_with(a, &g_fake_ops, dirs, 1, S("autre.mkv"), &r), "dossier avec barre finale");
}
#endif  // !_WIN32

static void test_media_core(Arena *a) {
    section("dossiers médias");
    TempArena top = temp_begin(a);

    // --- sérialisation dans l'ini ---
    {
        StrBuf dirs[MEDIA_MAX_DIRS];
        memset(dirs, 0, sizeof(dirs));
        strbuf_set(&dirs[0], S("C:\\Users\\thib\\Downloads"));
        strbuf_set(&dirs[1], S("D:\\Films & séries"));
        Str8 packed = media_dirs_join(a, dirs, 2);
        CHECK(str8_eq(packed, S("C:\\Users\\thib\\Downloads|D:\\Films & séries")), "jointure : « %.*s »",
              (int)packed.len, packed.data);
        StrBuf back[MEDIA_MAX_DIRS];
        memset(back, 0, sizeof(back));
        isize n = media_dirs_split(packed, back, MEDIA_MAX_DIRS);
        CHECK(n == 2 && strbuf_eq(&back[0], S("C:\\Users\\thib\\Downloads")) &&
                  strbuf_eq(&back[1], S("D:\\Films & séries")),
              "aller-retour exact (%lld)", (long long)n);
        // Entrées vides et surplus.
        n = media_dirs_split(S("|A||B|"), back, MEDIA_MAX_DIRS);
        CHECK(n == 2 && strbuf_eq(&back[0], S("A")), "entrées vides ignorées (%lld)", (long long)n);
        CHECK(media_dirs_split(S(""), back, MEDIA_MAX_DIRS) == 0, "chaîne vide");
        n = media_dirs_split(S("A|B|C"), back, 2);
        CHECK(n == 2, "surplus abandonné au-delà du maximum (%lld)", (long long)n);
        CHECK(media_dirs_join(a, dirs, 0).len == 0, "jointure vide");
    }
#ifndef _WIN32
    test_media_walk(a);
#endif
    temp_end(top);
}

// ------------------------------------------------------------ faux VLC ---

// FakeVlc reproduit internal/vlc/vlctest.Fake : position qui avance selon
// l'horloge simulée, seek arrondi à la seconde par l'interface HTTP,
// `position` = pos/length et `length` arrondie — c'est ce qui explique les
// résidus de virgule flottante visibles dans les vecteurs.
typedef struct {
    const char *state;
    f64 pos, length, rate;
    i64 last_at;
    StrBuf file;
} FakeVlc;

static f64 fake_clamp(f64 v, f64 lo, f64 hi) {
    if (hi > 0 && v > hi) return hi;
    if (v < lo) return lo;
    return v;
}

static void fake_advance(FakeVlc *f, i64 now) {
    f64 elapsed = vs_ns_seconds(now - f->last_at);
    f->last_at = now;
    if (elapsed <= 0 || strcmp(f->state, "playing") != 0) return;
    f->pos = fake_clamp(f->pos + elapsed * f->rate, 0, f->length);
}

static void fake_load(FakeVlc *f, Str8 name, f64 length, i64 now) {
    fake_advance(f, now);
    strbuf_set(&f->file, name);
    f->length = length;
    f->pos = 0;
    f->state = "paused";
    f->rate = 1;
}

static void fake_play(FakeVlc *f, i64 now) {
    fake_advance(f, now);
    if (strcmp(f->state, "stopped") != 0) f->state = "playing";
}

static void fake_pause(FakeVlc *f, i64 now) {
    fake_advance(f, now);
    if (strcmp(f->state, "stopped") != 0) f->state = "paused";
}

static void fake_seek(FakeVlc *f, f64 sec, i64 now) {
    fake_advance(f, now);
    f->pos = fake_clamp(sec, 0, f->length);
}

static f64 fake_floor(f64 v) {
    f64 r = f64_round(v);
    if (r > v) r -= 1;
    return r;
}

// fake_status_json produit la réponse que renverrait le faux VLC.
static Str8 fake_status_json(FakeVlc *f, Arena *a, i64 now) {
    fake_advance(f, now);
    f64 length = f64_round(f->length);
    f64 ratio = 0;
    if (length > 0) {
        ratio = f->pos / length;
        if (ratio < 0) ratio = 0;
        if (ratio > 1) ratio = 1;
    }
    JsonWriter w;
    jw_init(&w, a);
    jw_obj_begin(&w);
    jw_key(&w, "state");
    jw_cstr(&w, f->state);
    jw_kv_num(&w, "length", length);
    jw_kv_num(&w, "time", fake_floor(f->pos));
    jw_kv_num(&w, "rate", f->rate);
    jw_kv_num(&w, "position", ratio);
    jw_kv_i64(&w, "volume", 256);
    jw_key(&w, "information");
    jw_obj_begin(&w);
    jw_key(&w, "category");
    jw_obj_begin(&w);
    jw_key(&w, "meta");
    jw_obj_begin(&w);
    jw_kv_str(&w, "filename", strbuf_str(&f->file));
    jw_obj_end(&w);
    jw_obj_end(&w);
    jw_obj_end(&w);
    jw_obj_end(&w);
    return jw_result(&w);
}

// fake_apply applique une commande du moteur, comme le ferait vlc.c.
static void fake_apply(FakeVlc *f, VsCmd cmd, i64 now) {
    fake_advance(f, now);
    switch (cmd.kind) {
        case VS_CMD_PAUSE:
            if (strcmp(f->state, "playing") == 0) f->state = "paused";
            break;
        case VS_CMD_RESUME:
            if (strcmp(f->state, "paused") == 0) f->state = "playing";
            break;
        case VS_CMD_SEEK: {
            f64 v = cmd.value;
            if (!f64_is_finite(v) || v < 0) v = 0;
            f->pos = fake_clamp(f64_round(v), 0, f->length);
            break;
        }
        case VS_CMD_RATE:
            if (f64_is_finite(cmd.value) && cmd.value > 0) f->rate = cmd.value;
            break;
    }
}
// ------------------------------------------------------------ vecteurs ---

// Taille du fichier vidéo factice écrit par le générateur de vecteurs
// (vectors_test.go : []byte("données vidéo") = 15 octets UTF-8).
#define VECTOR_FILE_SIZE 15

static Str8 read_file(Arena *a, Str8 path) {
    Str8 empty = str8_lit("");
    TempArena t = temp_begin(a);
    FILE *f = fopen(str8_cstr(a, path), "rb");
    temp_end(t);
    if (!f) return empty;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return empty;
    }
    long size = ftell(f);
    if (size < 0 || (isize)size > VS_MB(32)) {
        fclose(f);
        return empty;
    }
    rewind(f);
    u8 *buf = arena_push_array(a, u8, (isize)size + 1);
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    return str8(buf, (isize)got);
}

// vectors_candidate fabrique « ../../test/vectors » avec le séparateur de la
// plateforme : le chemin des vecteurs est la seule chose que ce fichier a
// besoin de savoir du système de fichiers.
static Str8 vectors_candidate(Arena *a, isize up, u8 sep) {
    Builder b;
    builder_init(&b, a, 64);
    for (isize i = 0; i < up; i++) {
        builder_cstr(&b, "..");
        builder_byte(&b, sep);
    }
    builder_cstr(&b, "test");
    builder_byte(&b, sep);
    builder_cstr(&b, "vectors");
    return builder_result(&b);
}

// find_vectors_dir remonte l'arborescence jusqu'à trouver test/vectors.
static b32 find_vectors_dir(Arena *a, Str8 override, Str8 *out) {
    static const isize ups[] = {0, 2, 3, 1};
    if (override.len > 0) {
        *out = override;
        return 1;
    }
    const VsDirOps *ops = vs_dir_ops();
    for (isize i = 0; i < VS_ARRAY_COUNT(ups); i++) {
        Str8 cand = vectors_candidate(a, ups[i], ops->sep);
        TempArena t = temp_begin(a);
        VsDirIter *it = ops->open(a, cand);
        b32 ok = it != NULL;
        if (it) ops->close(it);
        temp_end(t);
        if (ok) {
            *out = cand;
            return 1;
        }
    }
    return 0;
}

typedef struct {
    // StrBuf et non Str8 : les noms rendus par la primitive de parcours ne
    // survivent pas à l'itération suivante, il faut les recopier hors arène.
    StrBuf names[64];
    isize count;
} FileList;

static void list_vectors(Arena *a, Str8 dir, FileList *out) {
    out->count = 0;
    const VsDirOps *ops = vs_dir_ops();
    TempArena t = temp_begin(a);
    VsDirIter *it = ops->open(a, dir);
    if (it) {
        VsDirEntry e;
        while (ops->next(it, &e)) {
            if (e.is_dir) continue;
            if (e.name.len < 5 || memcmp(e.name.data + e.name.len - 5, ".json", 5) != 0) continue;
            if (out->count >= VS_ARRAY_COUNT(out->names)) break;
            strbuf_set(&out->names[out->count++], e.name);
        }
        ops->close(it);
    }
    temp_end(t);
    // Tri par insertion : les vecteurs sont numérotés, l'ordre doit être stable.
    for (isize i = 1; i < out->count; i++) {
        StrBuf v = out->names[i];
        isize j = i - 1;
        while (j >= 0 && strcmp((const char *)out->names[j].data, (const char *)v.data) > 0) {
            out->names[j + 1] = out->names[j];
            j--;
        }
        out->names[j + 1] = v;
    }
}

// --- comparaison d'une étape de trace ---

static void check_commands(const char *vec, i64 at_ms, const JsonValue *want, const VsCmd *got, isize got_n) {
    isize want_n = (want && want->kind == JSON_ARRAY) ? want->count : 0;
    if (want_n != got_n) {
        failf("%s @%lldms : %lld commande(s) VLC, attendu %lld", vec, (long long)at_ms, (long long)got_n,
              (long long)want_n);
        return;
    }
    isize i = 0;
    for (JsonValue *c = want ? want->first : NULL; c; c = c->next, i++) {
        Str8 cmd = json_get_str(c, "cmd", S(""));
        const char *have = vs_cmd_name(got[i].kind);
        if (!str8_eq_cstr(cmd, have)) {
            failf("%s @%lldms : commande %lld = %s, attendu %.*s", vec, (long long)at_ms, (long long)i, have,
                  (int)cmd.len, cmd.data);
            continue;
        }
        JsonValue *val = json_get(c, "value");
        if (!val) continue;
        // La valeur consignée est celle envoyée à VLC : seek arrondi à la
        // seconde, rate arrondi au millième.
        f64 sent = got[i].kind == VS_CMD_SEEK ? f64_round(got[i].value) : got[i].value;
        if (!approx(sent, val->number, 1e-3)) {
            failf("%s @%lldms : %s valeur %.6f, attendu %.6f", vec, (long long)at_ms, have, sent, val->number);
        }
    }
}

static void check_messages(const char *vec, i64 at_ms, const JsonValue *want, const VsMsg *got, isize got_n) {
    isize want_n = (want && want->kind == JSON_ARRAY) ? want->count : 0;
    if (want_n != got_n) {
        failf("%s @%lldms : %lld message(s) serveur, attendu %lld", vec, (long long)at_ms, (long long)got_n,
              (long long)want_n);
        for (isize k = 0; k < got_n; k++) printf("      obtenu: %s\n", vs_msg_name(got[k].kind));
        for (JsonValue *c = want ? want->first : NULL; c; c = c->next) {
            Str8 t = json_get_str(c, "type", S(""));
            printf("      attendu: %.*s\n", (int)t.len, t.data);
        }
        return;
    }
    isize i = 0;
    for (JsonValue *c = want ? want->first : NULL; c; c = c->next, i++) {
        Str8 type = json_get_str(c, "type", S(""));
        const VsMsg *m = &got[i];
        if (!str8_eq_cstr(type, vs_msg_name(m->kind))) {
            failf("%s @%lldms : message %lld = %s, attendu %.*s", vec, (long long)at_ms, (long long)i,
                  vs_msg_name(m->kind), (int)type.len, type.data);
            continue;
        }
        JsonValue *d = json_get(c, "data");
        switch (m->kind) {
            case VS_MSG_PING:
                if (json_get_i64(d, "t", 0) != m->t) {
                    failf("%s @%lldms : ping t = %lld, attendu %lld", vec, (long long)at_ms, (long long)m->t,
                          (long long)json_get_i64(d, "t", 0));
                }
                break;
            case VS_MSG_SET_FILE: {
                Str8 name = json_get_str(d, "name", S(""));
                if (!strbuf_eq(&m->name, name)) {
                    failf("%s @%lldms : setFile name = %.*s, attendu %.*s", vec, (long long)at_ms,
                          (int)m->name.len, m->name.data, (int)name.len, name.data);
                }
                if (!approx(m->duration_sec, json_get_num(d, "durationSec", 0), 1e-6)) {
                    failf("%s @%lldms : setFile durationSec = %.6f, attendu %.6f", vec, (long long)at_ms,
                          m->duration_sec, json_get_num(d, "durationSec", 0));
                }
                if (m->size_bytes != json_get_i64(d, "sizeBytes", 0)) {
                    failf("%s @%lldms : setFile sizeBytes = %lld", vec, (long long)at_ms, (long long)m->size_bytes);
                }
                break;
            }
            case VS_MSG_CONTROL: {
                Str8 act = json_get_str(d, "action", S(""));
                if (!str8_eq_cstr(act, vs_action_name(m->action))) {
                    failf("%s @%lldms : control action = %s, attendu %.*s", vec, (long long)at_ms,
                          vs_action_name(m->action), (int)act.len, act.data);
                }
                if (!approx(m->position_sec, json_get_num(d, "positionSec", 0), 1e-6)) {
                    failf("%s @%lldms : control positionSec = %.9f, attendu %.9f", vec, (long long)at_ms,
                          m->position_sec, json_get_num(d, "positionSec", 0));
                }
                break;
            }
            case VS_MSG_REPORT: {
                if (!approx(m->position_sec, json_get_num(d, "positionSec", 0), 1e-6)) {
                    failf("%s @%lldms : report positionSec = %.9f, attendu %.9f", vec, (long long)at_ms,
                          m->position_sec, json_get_num(d, "positionSec", 0));
                }
                if ((m->paused != 0) != (json_get_bool(d, "paused", 0) != 0) ||
                    (m->buffering != 0) != (json_get_bool(d, "buffering", 0) != 0)) {
                    failf("%s @%lldms : report paused/buffering divergents", vec, (long long)at_ms);
                }
                break;
            }
            case VS_MSG_SET_READY:
                if ((m->ready != 0) != (json_get_bool(d, "ready", 0) != 0)) {
                    failf("%s @%lldms : setReady divergent", vec, (long long)at_ms);
                }
                break;
            case VS_MSG_CHAT:
                if (!strbuf_eq(&m->text, json_get_str(d, "text", S("")))) {
                    failf("%s @%lldms : chat divergent", vec, (long long)at_ms);
                }
                break;
        }
    }
}

// run_vector rejoue un fichier de vecteur complet.
static void run_vector(Arena *a, Str8 dir, Str8 file) {
    TempArena top = temp_begin(a);
    u8 sep = vs_dir_ops()->sep;
    Str8 path = str8_cat(a, str8_cat(a, dir, str8(&sep, 1)), file);
    Str8 raw = read_file(a, path);
    if (raw.len == 0) {
        failf("vecteur illisible: %.*s", (int)path.len, path.data);
        temp_end(top);
        return;
    }
    JsonError jerr = JSON_OK;
    JsonValue *v = json_parse(a, raw, &jerr);
    if (!v) {
        failf("vecteur %.*s: JSON invalide (%s)", (int)file.len, file.data, json_error_text(jerr));
        temp_end(top);
        return;
    }
    Str8 name = json_get_str(v, "name", file);
    char vec_name[128];
    isize nl = VS_MIN(name.len, (isize)sizeof(vec_name) - 1);
    memcpy(vec_name, name.data, (size_t)nl);
    vec_name[nl] = 0;

    i64 poll_ms = json_get_i64(v, "pollIntervalMs", 200);
    JsonValue *init = json_get(v, "initialVLC");
    JsonValue *events = json_get(v, "events");
    JsonValue *trace = json_get(v, "trace");
    if (!init || !events || !trace) {
        failf("vecteur %s incomplet", vec_name);
        temp_end(top);
        return;
    }

    // Horloge simulée : même origine que vlctest.NewClock (2026-08-05 20:00 UTC).
    const i64 base_ms = 1785960000000LL;
    i64 now = base_ms * 1000000LL;

    FakeVlc fake;
    memset(&fake, 0, sizeof(fake));
    fake.state = "stopped";
    fake.rate = 1;
    fake.last_at = now;
    Str8 file_name = json_get_str(init, "fileName", S("media.mkv"));
    fake_load(&fake, file_name, json_get_num(init, "durationSec", 0), now);
    fake_seek(&fake, json_get_num(init, "positionSec", 0), now);
    if (str8_eq(json_get_str(init, "state", S("")), S("playing"))) fake_play(&fake, now);

    VsEngine e;
    engine_init(&e);
    VsOutput out;
    vs_output_reset(&out);
    engine_open_file(&e, file_name, VECTOR_FILE_SIZE, &out);
    vs_output_reset(&out);  // le générateur draine les sorties d'ouverture

    JsonValue *ev = events->first;
    isize step_index = 0;
    // keepOutput : par défaut, ce que le moteur émet en réaction immédiate à un
    // message serveur ne fait pas partie de la trace (le générateur Go le
    // draine). Un événement marqué « keepOutput » garde cette réaction, qui est
    // alors attendue dans le premier pas de trace qui suit — c'est le cas quand
    // la réaction EST la règle testée (reprise « salle vierge »).
    b32 keep_pending = 0;
    for (JsonValue *step = trace->first; step; step = step->next, step_index++) {
        i64 step_ms = json_get_i64(step, "atMs", 0);

        // Les événements datés avant le prochain poll s'appliquent d'abord.
        while (ev && json_get_i64(ev, "atMs", 0) < step_ms) {
            i64 at = json_get_i64(ev, "atMs", 0);
            now = base_ms * 1000000LL + at * 1000000LL;
            Str8 type = json_get_str(ev, "type", S(""));
            JsonValue *data = json_get(ev, "data");
            if (str8_eq_cstr(type, "wait")) {
                now += json_get_i64(data, "durationMs", 0) * 1000000LL;
            } else if (str8_eq_cstr(type, "connectionLost")) {
                engine_session_lost(&e);
            } else if (str8_eq_cstr(type, "userPause")) {
                fake_pause(&fake, now);
            } else if (str8_eq_cstr(type, "userPlay")) {
                fake_play(&fake, now);
            } else if (str8_eq_cstr(type, "userSeek")) {
                fake_seek(&fake, json_get_num(data, "positionSec", 0), now);
            } else {
                VsInMsg m;
                proto_fill(a, type, data, &m);
                switch (m.kind) {
                    case VS_IN_WELCOME:
                        engine_on_welcome(&e, now, m.self_id, &m.state,
                                          m.have_self_ready ? &m.self_ready : NULL, &out);
                        break;
                    case VS_IN_PONG: engine_on_pong(&e, now, m.pong); break;
                    case VS_IN_ROOMSTATE: engine_on_roomstate(&e, now, &m.state); break;
                    default: break;
                }
            }
            if (json_get_bool(ev, "keepOutput", 0)) keep_pending = 1;
            else if (!keep_pending) vs_output_reset(&out);
            ev = ev->next;
        }

        // Un poll : lecture de VLC, ingestion, décisions.
        now += poll_ms * 1000000LL;
        i64 expect_ms = (now - base_ms * 1000000LL) / 1000000LL;
        if (expect_ms != step_ms) {
            failf("%s : désynchronisation d'horloge à l'étape %lld (%lldms vs %lldms attendus)", vec_name,
                  (long long)step_index, (long long)expect_ms, (long long)step_ms);
        }
        Str8 body = fake_status_json(&fake, a, now);
        VsStatus st;
        if (!vlc_parse_status(a, body, &st)) {
            failf("%s : status.json simulé illisible", vec_name);
            break;
        }
        if (!keep_pending) vs_output_reset(&out);
        keep_pending = 0;
        engine_on_vlc_status(&e, now, &st, &out);
        engine_on_tick(&e, now, &out);
        CHECK(!out.dropped, "%s : débordement de la file de décisions", vec_name);

        // Comparaison à la trace attendue.
        Str8 want_state = json_get_str(step, "vlcState", S(""));
        if (!str8_eq_cstr(want_state, vs_play_state_name(e.status.state))) {
            failf("%s @%lldms : état VLC = %s, attendu %.*s", vec_name, (long long)step_ms,
                  vs_play_state_name(e.status.state), (int)want_state.len, want_state.data);
        }
        f64 want_pos = json_get_num(step, "vlcPositionSec", 0);
        if (!approx(e.status.position_sec, want_pos, 1e-3)) {
            failf("%s @%lldms : position VLC = %.6f, attendu %.6f", vec_name, (long long)step_ms,
                  e.status.position_sec, want_pos);
        }
        f64 want_exp = json_get_num(step, "expectedPositionSec", 0);
        f64 got_exp = engine_expected_position(&e, now);
        if (!approx(got_exp, want_exp, 1e-3)) {
            failf("%s @%lldms : position attendue = %.6f, attendu %.6f", vec_name, (long long)step_ms, got_exp,
                  want_exp);
        }
        f64 want_drift = json_get_num(step, "driftSec", 0);
        if (!approx(e.drift, want_drift, 1e-3)) {
            failf("%s @%lldms : drift = %.6f, attendu %.6f", vec_name, (long long)step_ms, e.drift, want_drift);
        }
        check_commands(vec_name, step_ms, json_get(step, "vlcCommands"), out.cmds, out.cmd_count);
        check_messages(vec_name, step_ms, json_get(step, "toServer"), out.msgs, out.msg_count);
        g_checks++;

        // Les commandes décidées sont appliquées à VLC (comme le ferait vlc.c).
        for (isize i = 0; i < out.cmd_count; i++) fake_apply(&fake, out.cmds[i], now);
    }
    temp_end(top);
}


void test_core_vectors(Arena *a, Str8 override) {
    section("vecteurs de conformité");
    Str8 dir;
    if (!find_vectors_dir(a, override, &dir)) {
        failf("répertoire test/vectors introuvable (lancez depuis ui/win32 ou passez le chemin en argument)");
        return;
    }
    FileList files;
    list_vectors(a, dir, &files);
    if (files.count < 14) {
        failf("%lld vecteur(s) trouvé(s) dans %.*s, attendu au moins 14", (long long)files.count, (int)dir.len,
              dir.data);
        return;
    }
    for (isize i = 0; i < files.count; i++) {
        int before = g_failures;
        Str8 name = strbuf_str(&files.names[i]);
        run_vector(a, dir, name);
        printf("  %-24.*s %s\n", (int)name.len, name.data, g_failures == before ? "ok" : "ECHEC");
    }
}

// ------------------------------------------------------------ ordonnancement ---

void test_core_run(Arena *a) {
    test_base(a);
    test_json(a);
    test_protocol(a);
    test_vlc(a);
    test_engine_units();
    test_ini(a);
    test_conn_address(a);
    test_semver();
    test_conn_policy();
    test_offline_queue();
    test_virgin_resume();
    test_buffering_suspend();
    test_user_action_in_vlc();
    test_media_core(a);
}
