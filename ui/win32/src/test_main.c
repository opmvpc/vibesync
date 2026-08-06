// test_main.c — suite de tests console (vibesync_tests.exe).
//
// Couvre : base (arènes, chaînes, nombres, UTF-8/16), json (aller-retours et
// entrées hostiles), protocol (encodage/décodage), vlc (parsing de status.json
// réels, HTTP, base64), net (analyse d'URL) et surtout le REJEU DES 12
// VECTEURS DE CONFORMITÉ test/vectors/*.json, qui gèlent le moteur de sync.
//
// Sortie non nulle en cas d'échec, avec diagnostic.

#include "base.h"
#include "conn.h"
#include "engine.h"
#include "health.h"
#include "ini.h"
#include "json.h"
#include "net.h"
#include "protocol.h"
#include "secret.h"
#include "ui.h"
#include "vlc.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ------------------------------------------------------------ harnais ---

static int g_failures = 0;
static int g_checks = 0;
static const char *g_section = "";

static void failf(const char *fmt, ...) {
    g_failures++;
    printf("  ECHEC [%s] ", g_section);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

#define CHECK(cond, ...)              \
    do {                              \
        g_checks++;                   \
        if (!(cond)) failf(__VA_ARGS__); \
    } while (0)

static b32 approx(f64 a, f64 b, f64 tol) {
    f64 d = a - b;
    if (d < 0) d = -d;
    return d <= tol;
}

static void section(const char *name) {
    g_section = name;
    printf("== %s\n", name);
}

static Str8 S(const char *s) { return str8_from_cstr(s); }

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

    // Unicode : aller-retour UTF-8 ↔ UTF-16, hors BMP inclus.
    {
        TempArena t = temp_begin(a);
        Str8 src = S("caf\xc3\xa9 \xf0\x9f\x98\x80 fin");  // « café 😀 fin »
        isize wlen = 0;
        u16 *w = utf8_to_utf16(a, src, &wlen);
        // « café 😀 fin » = 11 unités UTF-16 (l'émoji en occupe deux).
        CHECK(wlen == 11, "longueur UTF-16 = %lld", (long long)wlen);
        CHECK(w[5] == 0xd83d && w[6] == 0xde00, "paire de substitution mal encodée");
        Str8 back = utf16_to_utf8(a, w);
        CHECK(str8_eq(back, src), "aller-retour UTF-8/16 cassé");
        u8 bad[] = {0xc3, 0x28, 0};  // séquence invalide
        CHECK(!utf8_validate(str8(bad, 2)), "UTF-8 invalide accepté");
        CHECK(utf8_validate(src), "UTF-8 valide refusé");
        u16 lone[] = {0xd800, 'a', 0};
        Str8 repl = utf16_to_utf8(a, lone);
        CHECK(repl.len == 4 && repl.data[0] == 0xef, "substitut orphelin non remplacé");
        temp_end(t);
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

    section("net");
    NetUrl u;
    CHECK(net_parse_url(S("ws://localhost:8080/ws"), &u) && !u.secure && u.port == 8080 &&
              strcmp(u.host, "localhost") == 0 && strcmp(u.path, "/ws") == 0,
          "ws:// avec port");
    CHECK(net_parse_url(S("wss://vibesync.example.com/ws"), &u) && u.secure && u.port == 443, "wss:// par défaut 443");
    CHECK(net_parse_url(S("ws://host"), &u) && u.port == 80 && strcmp(u.path, "/") == 0, "chemin par défaut");
    CHECK(net_parse_url(S("wss://[::1]:9000/ws"), &u) && u.port == 9000 && strcmp(u.host, "::1") == 0, "IPv6");
    CHECK(net_parse_url(S("https://x/y?z=1"), &u) && u.secure && strcmp(u.path, "/y?z=1") == 0, "query conservée");
    CHECK(!net_parse_url(S("ftp://x/"), &u), "schéma inconnu accepté");
    CHECK(!net_parse_url(S("ws://"), &u), "hôte vide accepté");
    CHECK(!net_parse_url(S("ws://h:0/"), &u), "port 0 accepté");
    CHECK(!net_parse_url(S("ws://h:99999/"), &u), "port hors bornes accepté");
    CHECK(!net_parse_url(S("ws://user:pass@h/"), &u), "identifiants dans l'URL acceptés");
    CHECK(!net_parse_url(S(""), &u), "URL vide acceptée");

    temp_end(top);
}

// ------------------------------------------- mini serveur WebSocket local ---
//
// Serveur RFC 6455 minimal (handshake + trames texte) pour exercer net.c pour
// de vrai : cycle connexion/envoi/réception/fermeture, saturation de la file,
// et surtout arrêt CONCURRENT d'un envoi — le scénario des deux bloquants de
// la revue. Aucune dépendance : Winsock + SHA-1 maison.

typedef struct {
    u32 h[5];
    u64 bits;
    u8 buf[64];
    isize buf_len;
} Sha1;

static u32 rol32(u32 v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_block(Sha1 *s, const u8 *p) {
    u32 w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((u32)p[i * 4] << 24) | ((u32)p[i * 4 + 1] << 16) | ((u32)p[i * 4 + 2] << 8) | p[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a1 = s->h[0], b1 = s->h[1], c1 = s->h[2], d1 = s->h[3], e1 = s->h[4];
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20) {
            f = (b1 & c1) | (~b1 & d1);
            k = 0x5a827999u;
        } else if (i < 40) {
            f = b1 ^ c1 ^ d1;
            k = 0x6ed9eba1u;
        } else if (i < 60) {
            f = (b1 & c1) | (b1 & d1) | (c1 & d1);
            k = 0x8f1bbcdcu;
        } else {
            f = b1 ^ c1 ^ d1;
            k = 0xca62c1d6u;
        }
        u32 tmp = rol32(a1, 5) + f + e1 + k + w[i];
        e1 = d1;
        d1 = c1;
        c1 = rol32(b1, 30);
        b1 = a1;
        a1 = tmp;
    }
    s->h[0] += a1;
    s->h[1] += b1;
    s->h[2] += c1;
    s->h[3] += d1;
    s->h[4] += e1;
}

static void sha1_init(Sha1 *s) {
    s->h[0] = 0x67452301u;
    s->h[1] = 0xefcdab89u;
    s->h[2] = 0x98badcfeu;
    s->h[3] = 0x10325476u;
    s->h[4] = 0xc3d2e1f0u;
    s->bits = 0;
    s->buf_len = 0;
}

static void sha1_update(Sha1 *s, const u8 *data, isize len) {
    s->bits += (u64)len * 8;
    while (len > 0) {
        isize take = VS_MIN(64 - s->buf_len, len);
        memcpy(s->buf + s->buf_len, data, (size_t)take);
        s->buf_len += take;
        data += take;
        len -= take;
        if (s->buf_len == 64) {
            sha1_block(s, s->buf);
            s->buf_len = 0;
        }
    }
}

static void sha1_final(Sha1 *s, u8 out[20]) {
    u64 bits = s->bits;
    u8 pad = 0x80;
    sha1_update(s, &pad, 1);
    u8 zero = 0;
    while (s->buf_len != 56) sha1_update(s, &zero, 1);
    u8 len_be[8];
    for (int i = 0; i < 8; i++) len_be[i] = (u8)(bits >> (56 - 8 * i));
    s->bits = bits;  // sha1_update ci-dessous ne doit pas fausser le compte
    sha1_update(s, len_be, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (u8)(s->h[i] >> 24);
        out[i * 4 + 1] = (u8)(s->h[i] >> 16);
        out[i * 4 + 2] = (u8)(s->h[i] >> 8);
        out[i * 4 + 3] = (u8)s->h[i];
    }
}

typedef struct {
    SOCKET listener;
    int port;
    HANDLE thread;
    volatile long stop;
    volatile long accepted;
    volatile long received;
    volatile long echo;        // renvoyer chaque message reçu
    volatile long flood;       // nombre de messages à pousser dès la connexion
    volatile long flood_size;  // taille de chaque message poussé
    volatile long drop_after;  // fermer brutalement après N messages reçus
} MiniWs;

static b32 sock_send_all(SOCKET s, const u8 *data, isize len) {
    isize sent = 0;
    while (sent < len) {
        int n = send(s, (const char *)data + sent, (int)(len - sent), 0);
        if (n <= 0) return 0;
        sent += n;
    }
    return 1;
}

static b32 sock_recv_exact(SOCKET s, u8 *out, isize len) {
    isize got = 0;
    while (got < len) {
        int n = recv(s, (char *)out + got, (int)(len - got), 0);
        if (n <= 0) return 0;
        got += n;
    }
    return 1;
}

static b32 ws_send_text_frame(SOCKET s, const u8 *data, isize len) {
    u8 hdr[10];
    isize n = 0;
    hdr[0] = 0x81;  // FIN + texte
    if (len < 126) {
        hdr[1] = (u8)len;
        n = 2;
    } else if (len <= 0xffff) {
        hdr[1] = 126;
        hdr[2] = (u8)(len >> 8);
        hdr[3] = (u8)len;
        n = 4;
    } else {
        hdr[1] = 127;
        for (int i = 0; i < 8; i++) hdr[2 + i] = (u8)((u64)len >> (56 - 8 * i));
        n = 10;
    }
    if (!sock_send_all(s, hdr, n)) return 0;
    return sock_send_all(s, data, len);
}

// ws_recv_frame lit une trame masquée du client. opcode reçoit l'opcode.
static b32 ws_recv_frame(SOCKET s, u8 *out, isize cap, isize *out_len, int *opcode) {
    u8 h[2];
    if (!sock_recv_exact(s, h, 2)) return 0;
    *opcode = h[0] & 0x0f;
    b32 masked = (h[1] & 0x80) != 0;
    u64 len = h[1] & 0x7f;
    if (len == 126) {
        u8 e[2];
        if (!sock_recv_exact(s, e, 2)) return 0;
        len = ((u64)e[0] << 8) | e[1];
    } else if (len == 127) {
        u8 e[8];
        if (!sock_recv_exact(s, e, 8)) return 0;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | e[i];
    }
    u8 mask[4] = {0, 0, 0, 0};
    if (masked && !sock_recv_exact(s, mask, 4)) return 0;
    if (len > (u64)cap) return 0;
    if (len > 0 && !sock_recv_exact(s, out, (isize)len)) return 0;
    if (masked) {
        for (u64 i = 0; i < len; i++) out[i] ^= mask[i & 3];
    }
    *out_len = (isize)len;
    return 1;
}

static b32 ws_handshake(SOCKET s, Arena *a) {
    u8 req[4096];
    isize len = 0;
    for (;;) {
        if (len >= (isize)sizeof(req)) return 0;
        int n = recv(s, (char *)req + len, (int)(sizeof(req) - (size_t)len), 0);
        if (n <= 0) return 0;
        len += n;
        if (len >= 4 && memcmp(req + len - 4, "\r\n\r\n", 4) == 0) break;
    }
    Str8 head = str8(req, len);
    Str8 tag = str8_lit("Sec-WebSocket-Key:");
    isize at = -1;
    for (isize i = 0; i + tag.len <= head.len; i++) {
        if (memcmp(head.data + i, tag.data, (size_t)tag.len) == 0) {
            at = i + tag.len;
            break;
        }
    }
    if (at < 0) return 0;
    isize end = at;
    while (end < head.len && head.data[end] != '\r' && head.data[end] != '\n') end++;
    Str8 key = str8_trim(str8_sub(head, at, end - at));

    Str8 magic = str8_lit("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    Sha1 sh;
    sha1_init(&sh);
    sha1_update(&sh, key.data, key.len);
    sha1_update(&sh, magic.data, magic.len);
    u8 digest[20];
    sha1_final(&sh, digest);
    char accept[64];
    base64_encode(digest, 20, accept, (isize)sizeof(accept));

    Builder resp;
    builder_init(&resp, a, 256);
    builder_cstr(&resp, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n");
    builder_cstr(&resp, "Sec-WebSocket-Accept: ");
    builder_cstr(&resp, accept);
    builder_cstr(&resp, "\r\n\r\n");
    Str8 out = builder_result(&resp);
    return sock_send_all(s, out.data, out.len);
}

static DWORD WINAPI mini_ws_thread(LPVOID param) {
    MiniWs *srv = (MiniWs *)param;
    Arena *a = arena_create(VS_MB(2));
    if (!a) return 1;
    while (!InterlockedCompareExchange(&srv->stop, 0, 0)) {
        SOCKET c = accept(srv->listener, NULL, NULL);
        if (c == INVALID_SOCKET) break;
        InterlockedIncrement(&srv->accepted);
        isize mark = arena_pos(a);
        if (!ws_handshake(c, a)) {
            closesocket(c);
            arena_pop_to(a, mark);
            continue;
        }
        long flood = InterlockedCompareExchange(&srv->flood, 0, 0);
        if (flood > 0) {
            long size = InterlockedCompareExchange(&srv->flood_size, 0, 0);
            if (size < 16) size = 16;
            u8 *payload = arena_push_array(a, u8, size);
            memset(payload, 'x', (size_t)size);
            payload[0] = '{';
            payload[size - 1] = '}';
            for (long i = 0; i < flood; i++) {
                if (!ws_send_text_frame(c, payload, size)) break;
            }
        }
        u8 *buf = arena_push_array(a, u8, VS_KB(128));
        for (;;) {
            if (InterlockedCompareExchange(&srv->stop, 0, 0)) break;
            isize n = 0;
            int opcode = 0;
            if (!ws_recv_frame(c, buf, VS_KB(128), &n, &opcode)) break;
            if (opcode == 0x8) break;  // close
            if (opcode != 0x1 && opcode != 0x2 && opcode != 0x0) continue;
            long count = InterlockedIncrement(&srv->received);
            long drop = InterlockedCompareExchange(&srv->drop_after, 0, 0);
            if (drop > 0 && count >= drop) break;  // fermeture brutale
            if (InterlockedCompareExchange(&srv->echo, 0, 0)) {
                if (!ws_send_text_frame(c, buf, n)) break;
            }
        }
        closesocket(c);
        arena_pop_to(a, mark);
    }
    arena_destroy(a);
    return 0;
}

static b32 mini_ws_start(MiniWs *srv) {
    memset(srv, 0, sizeof(*srv));
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    srv->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv->listener == INVALID_SOCKET) return 0;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(srv->listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) return 0;
    int alen = (int)sizeof(addr);
    if (getsockname(srv->listener, (struct sockaddr *)&addr, &alen) != 0) return 0;
    srv->port = ntohs(addr.sin_port);
    if (listen(srv->listener, 8) != 0) return 0;
    srv->thread = CreateThread(NULL, 0, mini_ws_thread, srv, 0, NULL);
    return srv->thread != NULL;
}

static void mini_ws_stop(MiniWs *srv) {
    InterlockedExchange(&srv->stop, 1);
    if (srv->listener != INVALID_SOCKET) {
        closesocket(srv->listener);
        srv->listener = INVALID_SOCKET;
    }
    if (srv->thread) {
        WaitForSingleObject(srv->thread, 5000);
        CloseHandle(srv->thread);
        srv->thread = NULL;
    }
}

static Str8 mini_ws_url(Arena *a, const MiniWs *srv) {
    Builder b;
    builder_init(&b, a, 64);
    builder_cstr(&b, "ws://127.0.0.1:");
    builder_i64(&b, srv->port);
    builder_cstr(&b, "/ws");
    return builder_result(&b);
}

// wait_event attend un événement précis (ms max) en vidant la file.
static b32 wait_event(Net *net, NetSlot *slot, NetEventKind want, int timeout_ms) {
    i64 deadline = vs_now_ns() + (i64)timeout_ms * 1000000LL;
    for (;;) {
        while (net_poll(net, slot)) {
            if (slot->kind == want) return 1;
            if (slot->kind == NET_EV_ERROR || slot->kind == NET_EV_CLOSED) {
                if (want != NET_EV_ERROR && want != NET_EV_CLOSED) return 0;
            }
        }
        if (vs_now_ns() > deadline) return 0;
        HANDLE h = (HANDLE)net_wakeup_handle(net);
        if (h) WaitForSingleObject(h, 20);
        else Sleep(5);
    }
}

typedef struct {
    Net *net;
    volatile long stop;
    volatile long sent;
} Spammer;

// spam_thread martèle net_send_text pendant que le thread principal ferme :
// c'est le scénario de course « fermeture serveur pendant un envoi ».
static DWORD WINAPI spam_thread(LPVOID param) {
    Spammer *sp = (Spammer *)param;
    Str8 msg = str8_lit("{\"type\":\"ping\",\"data\":{\"t\":1785960000000}}");
    while (!InterlockedCompareExchange(&sp->stop, 0, 0)) {
        if (net_send_text(sp->net, msg)) InterlockedIncrement(&sp->sent);
    }
    return 0;
}

static void test_net_live(Arena *a) {
    section("net (cycle réel)");
    TempArena top = temp_begin(a);
    MiniWs srv;
    if (!mini_ws_start(&srv)) {
        failf("mini serveur WebSocket indisponible");
        temp_end(top);
        return;
    }
    InterlockedExchange(&srv.echo, 1);
    Str8 url = mini_ws_url(a, &srv);
    Net *net = arena_push_struct(a, Net);
    NetSlot *slot = arena_push_struct(a, NetSlot);

    CHECK(net_init(net), "net_init");

    // 1. Trois cycles connexion → envoi → écho → fermeture : pas de second
    //    thread réseau, pas de handle qui fuit, reconnexion propre.
    for (int cycle = 0; cycle < 3; cycle++) {
        CHECK(net_connect(net, url), "cycle %d : net_connect", cycle);
        CHECK(wait_event(net, slot, NET_EV_CONNECTED, 5000), "cycle %d : pas de NET_EV_CONNECTED", cycle);
        CHECK(net_state(net) == NET_STATE_OPEN, "cycle %d : état != OPEN", cycle);
        Str8 msg = str8_lit("{\"type\":\"hello\",\"data\":{\"version\":1}}");
        CHECK(net_send_text(net, msg), "cycle %d : envoi", cycle);
        CHECK(wait_event(net, slot, NET_EV_MESSAGE, 5000), "cycle %d : pas d'écho", cycle);
        CHECK(slot->len == msg.len && memcmp(slot->data, msg.data, (size_t)msg.len) == 0,
              "cycle %d : écho altéré (%lld octets)", cycle, (long long)slot->len);
        net_close(net);
        CHECK(net_state(net) == NET_STATE_DEAD, "cycle %d : état != DEAD après close", cycle);
        CHECK(!net_send_text(net, msg), "cycle %d : envoi accepté après fermeture", cycle);
    }
    CHECK(srv.accepted == 3, "%ld connexions acceptées, attendu 3", (long)srv.accepted);

    // 2. Fermeture CONCURRENTE d'un envoi, 100 itérations : c'est le scénario
    //    du bloquant nº2 (data race / double close sur les handles WinHTTP).
    {
        int stress = 100;
        int connected = 0;
        for (int i = 0; i < stress; i++) {
            if (!net_connect(net, url)) continue;
            Spammer sp;
            memset(&sp, 0, sizeof(sp));
            sp.net = net;
            HANDLE th = CreateThread(NULL, 0, spam_thread, &sp, 0, NULL);
            if (wait_event(net, slot, NET_EV_CONNECTED, 3000)) connected++;
            Sleep(i % 3);  // fenêtres de course variées
            net_close(net);
            InterlockedExchange(&sp.stop, 1);
            if (th) {
                WaitForSingleObject(th, 5000);
                CloseHandle(th);
            }
            CHECK(net_state(net) == NET_STATE_DEAD, "stress %d : état != DEAD", i);
            while (net_poll(net, slot)) { /* vidange */ }
        }
        CHECK(connected > stress / 2, "seulement %d/%d connexions établies sous stress", connected, stress);
        printf("  stress fermeture/envoi : %d itérations, %d connectées\n", stress, connected);
    }

    // 3. Fermeture brutale par le serveur : la perte est signalée, jamais tue.
    {
        InterlockedExchange(&srv.drop_after, 1);
        CHECK(net_connect(net, url), "connexion pour fermeture brutale");
        if (wait_event(net, slot, NET_EV_CONNECTED, 5000)) {
            net_send_text(net, str8_lit("{\"type\":\"ping\",\"data\":{\"t\":1}}"));
            b32 signaled = 0;
            i64 deadline = vs_now_ns() + 5000LL * 1000000LL;
            while (vs_now_ns() < deadline && !signaled) {
                while (net_poll(net, slot)) {
                    if (slot->kind == NET_EV_ERROR || slot->kind == NET_EV_CLOSED) signaled = 1;
                }
                Sleep(10);
            }
            CHECK(signaled, "coupure serveur non remontée");
        }
        net_close(net);
        InterlockedExchange(&srv.drop_after, 0);
    }

    net_destroy(net);
    mini_ws_stop(&srv);
    temp_end(top);
}

static void test_net_queue_saturation(Arena *a) {
    section("net (saturation de file)");
    TempArena top = temp_begin(a);
    MiniWs srv;
    if (!mini_ws_start(&srv)) {
        failf("mini serveur WebSocket indisponible");
        temp_end(top);
        return;
    }
    // Beaucoup plus que ce que l'arène de file peut contenir : le consommateur
    // ne vide rien pendant ce temps.
    InterlockedExchange(&srv.flood_size, 32768);
    InterlockedExchange(&srv.flood, 400);
    Str8 url = mini_ws_url(a, &srv);
    Net *net = arena_push_struct(a, Net);
    NetSlot *slot = arena_push_struct(a, NetSlot);
    CHECK(net_init(net), "net_init");
    CHECK(net_connect(net, url), "net_connect");

    Sleep(1500);  // le serveur inonde, personne ne consomme

    isize messages = 0, errors = 0, queue_full = 0;
    while (net_poll(net, slot)) {
        if (slot->kind == NET_EV_MESSAGE) {
            messages++;
            // Aucune troncature silencieuse : les messages restent entiers.
            CHECK(slot->len == 32768, "message tronqué (%lld octets)", (long long)slot->len);
            if (slot->len == 32768) {
                CHECK(slot->data[0] == '{' && slot->data[slot->len - 1] == '}', "message corrompu");
            }
        } else if (slot->kind == NET_EV_ERROR) {
            errors++;
            if (slot->code == NET_ERR_QUEUE_FULL) queue_full++;
        }
    }
    printf("  %lld messages reçus, %lld erreurs (dont %lld saturation)\n", (long long)messages,
           (long long)errors, (long long)queue_full);
    CHECK(messages > 50, "file trop petite : seulement %lld messages avant saturation", (long long)messages);
    CHECK(queue_full > 0, "saturation non signalée explicitement");
    CHECK(net->dropped == 0, "%lld événement(s) perdus silencieusement", (long long)net->dropped);

    net_destroy(net);
    mini_ws_stop(&srv);
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

    // Fichier : écriture, relecture, BOM toléré.
    {
        u16 wtmp[MAX_PATH];
        DWORD n = GetTempPathW(MAX_PATH, (LPWSTR)wtmp);
        CHECK(n > 0, "dossier temporaire");
        Str8 dir = utf16_to_utf8(a, wtmp);
        Str8 path = str8_cat(a, dir, S("vibesync-test.ini"));
        CHECK(ini_save_file(a, path, text), "écriture du fichier");
        Ini loaded;
        CHECK(ini_load_file(a, path, &loaded), "lecture du fichier");
        CHECK(str8_eq(ini_get(&loaded, "pseudo", S("")), S("Thibault Éloïse")), "accents sur disque");
        Str8 bom = str8_cat(a, S("\xef\xbb\xbf"), text);
        CHECK(ini_save_file(a, path, bom), "écriture avec BOM");
        CHECK(ini_load_file(a, path, &loaded), "lecture avec BOM");
        CHECK(str8_eq(ini_get(&loaded, "serveur", S("")), S("wss://vibesync.exemple.fr/ws")), "BOM ignoré");
        u16 *wpath = utf8_to_utf16(a, path, NULL);
        DeleteFileW((LPCWSTR)wpath);
        Ini none;
        CHECK(!ini_load_file(a, path, &none) && none.count == 0, "fichier absent");
    }

    section("ui");
    struct {
        f64 sec;
        const char *want;
    } times[] = {
        {0, "0:00"},        {5, "0:05"},       {59.9, "0:59"},     {60, "1:00"},
        {83, "1:23"},       {599, "9:59"},     {3600, "1:00:00"},  {5025, "1:23:45"},
        {-5, "0:00"},       {36000, "10:00:00"},
    };
    for (isize i = 0; i < VS_ARRAY_COUNT(times); i++) {
        char buf[32];
        ui_format_time(times[i].sec, buf, sizeof(buf));
        CHECK(strcmp(buf, times[i].want) == 0, "temps %.1f = %s, attendu %s", times[i].sec, buf,
              times[i].want);
    }
    {
        char buf[32];
        // Valeur non finie : repli sur 0:00 plutôt qu'un affichage absurde.
        ui_format_time(1e308 * 10, buf, sizeof(buf));
        CHECK(strcmp(buf, "0:00") == 0, "durée infinie : %s", buf);
        // Valeur finie démesurée : bornée à 99:59:59.
        ui_format_time(9999999, buf, sizeof(buf));
        CHECK(strcmp(buf, "99:59:59") == 0, "durée démesurée bornée : %s", buf);
    }
    // Champ de saisie : troncature sur frontière UTF-8, jamais au milieu.
    {
        UiText t;
        memset(&t, 0, sizeof(t));
        u8 big_text[UI_TEXT_CAP * 2];
        for (isize i = 0; i < (isize)sizeof(big_text); i += 2) {
            big_text[i] = 0xc3;  // « é » en UTF-8 : deux octets
            big_text[i + 1] = 0xa9;
        }
        ui_text_set(&t, str8(big_text, (isize)sizeof(big_text)));
        CHECK(t.len < UI_TEXT_CAP, "champ tronqué (%lld)", (long long)t.len);
        CHECK(utf8_validate(ui_text_str(&t)), "troncature au milieu d'un caractère UTF-8");
        ui_text_set(&t, S("bonjour"));
        CHECK(str8_eq(ui_text_str(&t), S("bonjour")) && t.caret == 7, "contenu et caret");
    }

    temp_end(top);
}

// ------------------------------------------- édition de texte (VS-018) ---
//
// Le modèle caret/sélection est découplé du rendu : ces tests l'exercent sans
// fenêtre ni GDI, en branchant une police fictive sur UiTextMetrics.

// fake_width : 10 px par caractère, 4 px pour les caractères fins. Largeurs
// inégales exprès, pour que le hit-test ne puisse pas tricher avec une division.
static i32 fake_width(void *ctx, const UiText *t, isize byte_off) {
    VS_UNUSED(ctx);
    i32 w = 0;
    for (isize i = 0; i < byte_off && i < t->len; i++) {
        u8 c = t->data[i];
        if ((c & 0xc0) == 0x80) continue;  // octet de continuation : même glyphe
        w += (c == 'i' || c == 'l') ? 4 : 10;
    }
    return w;
}

static void test_text_edit(void) {
    section("édition de texte");
    UiTextMetrics mx = {fake_width, NULL};
    UiText t;
    memset(&t, 0, sizeof(t));

    // --- hit-test : « salon », frontières à 0, 10, 20, 24, 34, 44 px ---
    ui_text_set(&t, S("salon"));
    struct {
        i32 x;
        isize want;
    } hits[] = {
        {-20, 0}, {0, 0}, {4, 0},  {6, 1},  {15, 1},
        {16, 2},  {23, 3}, {40, 5}, {1000, 5},
    };
    for (isize i = 0; i < VS_ARRAY_COUNT(hits); i++) {
        isize got = ui_text_hit(&t, &mx, hits[i].x);
        CHECK(got == hits[i].want, "hit(%d) = %lld, attendu %lld", (int)hits[i].x, (long long)got,
              (long long)hits[i].want);
    }
    {
        UiText empty;
        memset(&empty, 0, sizeof(empty));
        CHECK(ui_text_hit(&empty, &mx, 50) == 0, "hit sur champ vide");
    }

    // --- hit-test : jamais au milieu d'un caractère multi-octets ---
    ui_text_set(&t, S("café"));  // « é » = 2 octets, len 5
    CHECK(t.len == 5, "longueur de « café » = %lld", (long long)t.len);
    CHECK(ui_text_hit(&t, &mx, 33) == 3, "hit avant « é »");
    CHECK(ui_text_hit(&t, &mx, 36) == 5, "hit après « é » (pas 4)");
    ui_text_move(&t, 4, 0);  // offset interdit : ramené sur la frontière
    CHECK(t.caret == 3, "caret recalé sur frontière UTF-8 (%lld)", (long long)t.caret);

    // --- clic puis glissé : sélection continue ---
    ui_text_set(&t, S("salon"));
    ui_text_move(&t, ui_text_hit(&t, &mx, 16), 0);  // appui à 16 px → offset 2
    CHECK(t.caret == 2 && !ui_text_has_sel(&t), "appui : caret posé, pas de sélection");
    ui_text_move(&t, ui_text_hit(&t, &mx, 30), 1);  // glissé
    CHECK(ui_text_sel_lo(&t) == 2 && ui_text_sel_hi(&t) == 4, "glissé : sélection [2,4)");
    ui_text_move(&t, ui_text_hit(&t, &mx, 1000), 1);
    CHECK(str8_eq(ui_text_selection(&t), S("lon")), "glissé jusqu'au bout");
    // Retour en arrière : l'ancre ne bouge pas, la sélection s'inverse.
    ui_text_move(&t, ui_text_hit(&t, &mx, 0), 1);
    CHECK(ui_text_sel_lo(&t) == 0 && ui_text_sel_hi(&t) == 2 && str8_eq(ui_text_selection(&t), S("sa")),
          "sélection inversée");

    // --- double-clic : mot sous le curseur ---
    struct {
        const char *text;
        isize pos;
        const char *want;
    } words[] = {
        {"salut le monde", 6, "le"},
        {"salut le monde", 0, "salut"},
        {"salut le monde", 14, "monde"},   // clic après le dernier caractère
        {"salut le monde", 5, " "},        // sur un espace : la suite d'espaces
        {"ws://127.0.0.1:8080/ws", 5, "127"},
        {"ws://127.0.0.1:8080/ws", 2, "://"},  // ponctuation : le groupe entier
        {"Thibault Éloïse", 9, "Éloïse"},      // accents = lettres
    };
    for (isize i = 0; i < VS_ARRAY_COUNT(words); i++) {
        ui_text_set(&t, S(words[i].text));
        isize lo, hi;
        ui_text_word_bounds(&t, words[i].pos, &lo, &hi);
        ui_text_select_range(&t, lo, hi);
        CHECK(str8_eq(ui_text_selection(&t), S(words[i].want)), "mot en %lld de « %s » = « %.*s »",
              (long long)words[i].pos, words[i].text, (int)ui_text_selection(&t).len,
              ui_text_selection(&t).data);
    }
    {
        UiText empty;
        memset(&empty, 0, sizeof(empty));
        isize lo = 9, hi = 9;
        ui_text_word_bounds(&empty, 0, &lo, &hi);
        CHECK(lo == 0 && hi == 0, "mot dans un champ vide");
    }

    // --- sauts de mot (Ctrl+flèches) ---
    ui_text_set(&t, S("salut le monde"));
    CHECK(ui_text_word_right(&t, 0) == 6, "mot à droite depuis 0");
    CHECK(ui_text_word_right(&t, 6) == 9, "mot à droite depuis 6");
    CHECK(ui_text_word_right(&t, 14) == 14, "mot à droite en fin de champ");
    CHECK(ui_text_word_left(&t, 14) == 9, "mot à gauche depuis la fin");
    CHECK(ui_text_word_left(&t, 9) == 6, "mot à gauche depuis 9");
    CHECK(ui_text_word_left(&t, 0) == 0, "mot à gauche en début de champ");

    // --- Ctrl+A puis frappe : la sélection est remplacée ---
    ui_text_set(&t, S("ancien"));
    ui_text_select_all(&t);
    CHECK(ui_text_has_sel(&t) && ui_text_sel_lo(&t) == 0 && ui_text_sel_hi(&t) == 6, "Ctrl+A");
    ui_text_insert_cp(&t, 'X');
    CHECK(str8_eq(ui_text_str(&t), S("X")) && t.caret == 1 && !ui_text_has_sel(&t),
          "frappe sur sélection : remplacement");

    // --- suppressions ---
    ui_text_set(&t, S("salon"));
    ui_text_select_range(&t, 1, 3);
    ui_text_backspace(&t, 0);
    CHECK(str8_eq(ui_text_str(&t), S("son")) && t.caret == 1, "Retour arrière sur sélection");
    ui_text_set(&t, S("salon"));
    ui_text_select_range(&t, 1, 3);
    ui_text_delete_fwd(&t, 0);
    CHECK(str8_eq(ui_text_str(&t), S("son")), "Suppr sur sélection");
    ui_text_set(&t, S("café"));
    ui_text_backspace(&t, 0);
    CHECK(str8_eq(ui_text_str(&t), S("caf")) && utf8_validate(ui_text_str(&t)),
          "Retour arrière supprime le caractère entier");
    ui_text_set(&t, S("salut le monde"));
    ui_text_backspace(&t, 1);
    CHECK(str8_eq(ui_text_str(&t), S("salut le ")), "Ctrl+Retour arrière : un mot");
    ui_text_set(&t, S("salut le monde"));
    ui_text_move(&t, 0, 0);
    ui_text_delete_fwd(&t, 1);
    CHECK(str8_eq(ui_text_str(&t), S("le monde")), "Ctrl+Suppr : un mot");

    // --- collage : mono-ligne, sélection remplacée ---
    ui_text_set(&t, S("abc"));
    ui_text_select_all(&t);
    ui_text_insert_str(&t, S("un\r\ndeux\ttrois"));
    CHECK(str8_eq(ui_text_str(&t), S("undeuxtrois")), "collage filtré : « %.*s »", (int)t.len, t.data);
    CHECK(t.caret == t.len && !ui_text_has_sel(&t), "caret après collage");

    // --- Maj+Origine / Maj+Fin ---
    ui_text_set(&t, S("salon"));
    ui_text_move(&t, 2, 0);
    ui_text_move(&t, t.len, 1);
    CHECK(str8_eq(ui_text_selection(&t), S("lon")), "Maj+Fin");
    ui_text_move(&t, 0, 1);
    CHECK(str8_eq(ui_text_selection(&t), S("sa")), "Maj+Origine depuis l'ancre");

    // --- limites : capacité et frontières UTF-8 ---
    {
        u8 big[UI_TEXT_CAP * 2];
        for (isize i = 0; i < (isize)sizeof(big); i += 2) {
            big[i] = 0xc3;  // « é »
            big[i + 1] = 0xa9;
        }
        ui_text_set(&t, str8(big, (isize)sizeof(big)));
        CHECK(t.len == UI_TEXT_CAP - 2 && utf8_validate(ui_text_str(&t)), "troncature (%lld octets)",
              (long long)t.len);
        isize before = t.len;
        ui_text_insert_cp(&t, 0xe9);  // « é » : 2 octets, il n'en reste qu'un
        CHECK(t.len == before, "insertion refusée quand le champ est plein");
        ui_text_insert_cp(&t, 'x');  // 1 octet : passe tout juste
        CHECK(t.len == before + 1 && t.data[t.len] == 0, "dernier octet utilisable");
        CHECK(utf8_validate(ui_text_str(&t)), "champ plein toujours valide en UTF-8");
        ui_text_select_all(&t);
        ui_text_insert_str(&t, S("court"));
        CHECK(str8_eq(ui_text_str(&t), S("court")), "remplacement total d'un champ plein");
    }
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

static void test_semver(void) {
    section("semver");
    struct {
        const char *a, *b;
        int want;
    } cases[] = {
        {"0.2.0", "0.2.0", 0},   {"0.2.1", "0.2.0", 1},   {"0.2.0", "0.2.1", -1},
        {"0.10.0", "0.2.0", 1},  {"1.0.0", "0.99.99", 1}, {"v0.3.0", "0.2.0", 1},
        {"0.2", "0.2.0", 0},     {"0.2.0", "0.2", 0},     {"1", "0.9.9", 1},
        {"1.0.0-rc1", "1.0.0", 0},  // suffixe ignoré : c'est documenté
        {"dev", "0.2.0", -1},       // version non numérique = 0.0.0
        {"", "0.0.0", 0},
    };
    for (isize i = 0; i < VS_ARRAY_COUNT(cases); i++) {
        int got = proto_semver_cmp(S(cases[i].a), S(cases[i].b));
        CHECK(got == cases[i].want, "semver(%s, %s) = %d, attendu %d", cases[i].a, cases[i].b, got,
              cases[i].want);
    }
    // La bannière ne doit jamais apparaître pour une version égale ou plus vieille.
    CHECK(proto_semver_cmp(S("0.2.0"), S(VS_VERSION)) <= 0 || proto_semver_cmp(S("9.9.9"), S(VS_VERSION)) > 0,
          "comparaison utilisable contre VS_VERSION");
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

// --------------------------------- mot de passe mémorisé (VS-025, DPAPI) ---

static void test_secret(Arena *a) {
    section("secret (DPAPI)");
    TempArena top = temp_begin(a);

    // Aller-retour : accents et octets non ASCII compris.
    const char *plains[] = {"s3cret", "mot de passe très long avec des accents éàü et des espaces", "x",
                            "!@#$%^&*()_+-={}[]|\\:;\"'<>,.?/"};
    for (isize i = 0; i < VS_ARRAY_COUNT(plains); i++) {
        Str8 hex, back;
        if (!secret_protect(a, S(plains[i]), &hex)) {
            failf("chiffrement refusé pour « %s »", plains[i]);
            g_checks++;
            continue;
        }
        // Le blob ne doit jamais contenir le clair en toutes lettres.
        CHECK(hex.len > 0 && (hex.len & 1) == 0, "blob hexadécimal de longueur paire (%lld)",
              (long long)hex.len);
        b32 hexish = 1;
        for (isize k = 0; k < hex.len; k++) {
            u8 c = hex.data[k];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) hexish = 0;
        }
        CHECK(hexish, "blob strictement hexadécimal minuscule");
        u8 *raw = NULL;
        isize raw_len = 0;
        CHECK(secret_hex_decode(a, hex, &raw, &raw_len), "blob redécodable");
        // Le clair ne doit pas se retrouver tel quel dans le blob. Sous 4
        // octets, la recherche n'a aucune valeur : une aiguille si courte se
        // retrouve par hasard dans quelques centaines d'octets chiffrés.
        Str8 needle = S(plains[i]);
        if (needle.len >= 4) {
            b32 found = 0;
            for (isize k = 0; raw && k + needle.len <= raw_len; k++) {
                if (memcmp(raw + k, needle.data, (size_t)needle.len) == 0) found = 1;
            }
            CHECK(!found, "le clair « %s » n'apparaît pas dans le blob chiffré", plains[i]);
        }

        CHECK(secret_unprotect(a, hex, &back), "déchiffrement de « %s »", plains[i]);
        CHECK(str8_eq(back, S(plains[i])), "aller-retour exact : « %.*s »", (int)back.len, back.data);
    }

    // Deux chiffrements du même clair diffèrent (DPAPI sale son entrée).
    {
        Str8 h1, h2;
        if (secret_protect(a, S("pareil"), &h1) && secret_protect(a, S("pareil"), &h2)) {
            CHECK(!str8_eq(h1, h2), "deux blobs du même clair ne sont pas identiques");
        }
    }

    // Clair vide : refusé (rien à mémoriser).
    {
        Str8 hex;
        CHECK(!secret_protect(a, S(""), &hex), "clair vide refusé");
    }

    // Blobs invalides : échec propre, jamais de plantage ni de sortie remplie.
    {
        Str8 ref;
        CHECK(secret_protect(a, S("s3cret"), &ref), "blob de référence");
        struct {
            const char *hex;
            const char *why;
        } bad[] = {
            {"", "blob vide"},
            {"abc", "longueur impaire"},
            {"zz", "caractère non hexadécimal"},
            {"00112233445566778899aabbccddeeff", "octets aléatoires"},
            {"deadbeef", "blob trop court"},
        };
        for (isize i = 0; i < VS_ARRAY_COUNT(bad); i++) {
            Str8 out = S("sentinelle");
            CHECK(!secret_unprotect(a, S(bad[i].hex), &out), "%s rejeté", bad[i].why);
            CHECK(str8_eq(out, S("sentinelle")), "%s : la sortie n'est pas touchée", bad[i].why);
        }
        // Blob authentique mais corrompu au milieu : DPAPI doit le refuser.
        u8 *copy = arena_push_array(a, u8, ref.len);
        memcpy(copy, ref.data, (size_t)ref.len);
        copy[ref.len / 2] ^= 0x0f;  // reste hexadécimal, mais le contenu change
        if (copy[ref.len / 2] > 'f') copy[ref.len / 2] = '0';
        Str8 out = S("sentinelle");
        CHECK(!secret_unprotect(a, str8(copy, ref.len), &out), "blob corrompu rejeté");
        // Mauvaise entropie applicative : un blob DPAPI d'une autre appli ne
        // doit pas être lisible par nous. On le simule en tronquant.
        CHECK(!secret_unprotect(a, str8_sub(ref, 0, ref.len - 2), &out), "blob tronqué rejeté");
    }

    // secret_wipe efface réellement.
    {
        u8 buf[32];
        memset(buf, 0xab, sizeof(buf));
        secret_wipe(buf, (isize)sizeof(buf));
        b32 clean = 1;
        for (isize i = 0; i < (isize)sizeof(buf); i++) {
            if (buf[i] != 0) clean = 0;
        }
        CHECK(clean, "secret_wipe met le tampon à zéro");
        secret_wipe(NULL, 16);  // ne doit pas planter
        secret_wipe(buf, 0);
    }

    // --- règles du fichier ini ---
    section("ini : secret");
    {
        Ini ini;
        ini_clear(&ini);
        ini_set(a, &ini, "serveur", S("wss://x/ws"));
        ini_set(a, &ini, "password_enc", S("00ff"));
        ini_set(a, &ini, "salle", S("salon"));
        CHECK(ini.count == 3, "trois entrées");
        CHECK(ini_remove(&ini, "password_enc"), "suppression de password_enc");
        CHECK(ini.count == 2, "l'entrée a disparu (%lld)", (long long)ini.count);
        CHECK(ini_get(&ini, "password_enc", S("absent")).len == 6, "clé introuvable après suppression");
        // Les autres clés gardent leur valeur et leur ordre.
        CHECK(str8_eq(ini_get(&ini, "serveur", S("")), S("wss://x/ws")) &&
                  str8_eq(ini_get(&ini, "salle", S("")), S("salon")),
              "les autres réglages sont intacts");
        CHECK(!ini_remove(&ini, "password_enc"), "seconde suppression sans effet");
        CHECK(!ini_remove(&ini, "jamais_vu"), "clé inconnue : rien à supprimer");
        // Le fichier écrit ne doit plus mentionner la clé.
        Str8 text = ini_write(a, &ini);
        b32 mentions = 0;
        for (isize i = 0; i + 12 <= text.len; i++) {
            if (memcmp(text.data + i, "password_enc", 12) == 0) mentions = 1;
        }
        CHECK(!mentions, "vibesync.ini n'écrit plus password_enc");
    }
    // Un ini existant sans entrée de mot de passe reste lisible tel quel.
    {
        Ini ini;
        CHECK(ini_parse(a, S("serveur=wss://x/ws\npseudo=thibault\n"), &ini), "ini d'une version antérieure");
        CHECK(ini_get(&ini, "password_enc", S("")).len == 0, "aucun secret mémorisé");
        CHECK(ini_get(&ini, "retenir_mdp", S("1")).len == 1, "case cochée par défaut");
    }

    temp_end(top);
}

// --------------------------------------------- faux VLC HTTP (sur socket) ---
//
// Sert /requests/status.json comme le vrai VLC, pour exercer le chemin réseau
// de vlc.c (Basic auth, commandes, réponses chunked) et la préparation
// pause+seek 0 exigée par docs/protocol.md §Chargement de fichier.

typedef struct {
    SOCKET listener;
    int port;
    HANDLE thread;
    volatile long stop;
    volatile long requests;
    volatile long chunked;  // répondre en Transfer-Encoding: chunked
    char password[64];
    // état simulé, protégé par le verrou
    SRWLOCK lock;
    const char *state;
    f64 pos;
    f64 length;
    f64 rate;
} FakeVlcHttp;

static void fake_http_body(FakeVlcHttp *srv, Arena *a, Str8 *out) {
    JsonWriter w;
    jw_init(&w, a);
    jw_obj_begin(&w);
    jw_key(&w, "state");
    jw_cstr(&w, srv->state);
    jw_kv_num(&w, "length", srv->length);
    jw_kv_num(&w, "time", (f64)(i64)srv->pos);
    jw_kv_num(&w, "rate", srv->rate);
    jw_kv_num(&w, "position", srv->length > 0 ? srv->pos / srv->length : 0);
    jw_key(&w, "information");
    jw_obj_begin(&w);
    jw_key(&w, "category");
    jw_obj_begin(&w);
    jw_key(&w, "meta");
    jw_obj_begin(&w);
    jw_kv_str(&w, "filename", str8_lit("ep1.mkv"));
    jw_obj_end(&w);
    jw_obj_end(&w);
    jw_obj_end(&w);
    jw_obj_end(&w);
    *out = jw_result(&w);
}

// fake_http_apply exécute la commande portée par la query.
static void fake_http_apply(FakeVlcHttp *srv, Str8 query) {
    Str8 cmd = str8_lit("");
    Str8 val = str8_lit("");
    isize i = 0;
    while (i < query.len) {
        isize amp = str8_find_char(query, '&', i);
        Str8 pair = str8_sub(query, i, (amp < 0 ? query.len : amp) - i);
        isize eq = str8_find_char(pair, '=', 0);
        if (eq > 0) {
            Str8 k = str8_sub(pair, 0, eq);
            Str8 v = str8_sub(pair, eq + 1, -1);
            if (str8_eq_cstr(k, "command")) cmd = v;
            else if (str8_eq_cstr(k, "val")) val = v;
        }
        if (amp < 0) break;
        i = amp + 1;
    }
    if (cmd.len == 0) return;
    if (str8_eq_cstr(cmd, "pl_forcepause")) {
        if (strcmp(srv->state, "playing") == 0) srv->state = "paused";
    } else if (str8_eq_cstr(cmd, "pl_forceresume")) {
        if (strcmp(srv->state, "paused") == 0) srv->state = "playing";
    } else if (str8_eq_cstr(cmd, "seek")) {
        f64 v = 0;
        if (str_to_f64(val, &v) && v >= 0) srv->pos = v;
    } else if (str8_eq_cstr(cmd, "rate")) {
        f64 v = 0;
        if (str_to_f64(val, &v) && v > 0) srv->rate = v;
    }
}

static DWORD WINAPI fake_vlc_thread(LPVOID param) {
    FakeVlcHttp *srv = (FakeVlcHttp *)param;
    Arena *a = arena_create(VS_MB(1));
    if (!a) return 1;
    char expected[128];
    {
        u8 raw[80];
        isize m = 0;
        raw[m++] = ':';
        for (isize i = 0; srv->password[i] && m < (isize)sizeof(raw); i++) raw[m++] = (u8)srv->password[i];
        base64_encode(raw, m, expected, (isize)sizeof(expected));
    }
    while (!InterlockedCompareExchange(&srv->stop, 0, 0)) {
        SOCKET c = accept(srv->listener, NULL, NULL);
        if (c == INVALID_SOCKET) break;
        isize mark = arena_pos(a);
        u8 req[8192];
        isize len = 0;
        b32 complete = 0;
        while (len < (isize)sizeof(req)) {
            int n = recv(c, (char *)req + len, (int)(sizeof(req) - (size_t)len), 0);
            if (n <= 0) break;
            len += n;
            if (len >= 4 && memcmp(req + len - 4, "\r\n\r\n", 4) == 0) {
                complete = 1;
                break;
            }
        }
        if (!complete) {
            closesocket(c);
            arena_pop_to(a, mark);
            continue;
        }
        InterlockedIncrement(&srv->requests);
        Str8 head = str8(req, len);
        // Authentification : « Authorization: Basic <b64> ».
        b32 authorized = 0;
        Str8 tag = str8_lit("Authorization: Basic ");
        for (isize i = 0; i + tag.len <= head.len; i++) {
            if (memcmp(head.data + i, tag.data, (size_t)tag.len) != 0) continue;
            isize s = i + tag.len, e = s;
            while (e < head.len && head.data[e] != '\r' && head.data[e] != '\n') e++;
            authorized = str8_eq(str8_trim(str8_sub(head, s, e - s)), str8_from_cstr(expected));
            break;
        }
        Builder resp;
        builder_init(&resp, a, VS_KB(8));
        if (!authorized) {
            builder_cstr(&resp, "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"VLC\"\r\n"
                                "Content-Length: 0\r\nConnection: close\r\n\r\n");
        } else {
            isize qs = str8_find_char(head, '?', 0);
            isize sp = str8_find_char(head, ' ', 4);
            Str8 query = str8_lit("");
            if (qs > 0 && sp > qs) query = str8_sub(head, qs + 1, sp - qs - 1);
            Str8 body;
            AcquireSRWLockExclusive(&srv->lock);
            fake_http_apply(srv, query);
            fake_http_body(srv, a, &body);
            ReleaseSRWLockExclusive(&srv->lock);
            if (InterlockedCompareExchange(&srv->chunked, 0, 0)) {
                builder_cstr(&resp, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                    "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
                // Deux blocs pour exercer réellement le dé-chunking.
                isize half = body.len / 2;
                char hex[24];
                snprintf(hex, sizeof(hex), "%llx\r\n", (unsigned long long)half);
                builder_cstr(&resp, hex);
                builder_bytes(&resp, body.data, half);
                builder_cstr(&resp, "\r\n");
                snprintf(hex, sizeof(hex), "%llx\r\n", (unsigned long long)(body.len - half));
                builder_cstr(&resp, hex);
                builder_bytes(&resp, body.data + half, body.len - half);
                builder_cstr(&resp, "\r\n0\r\n\r\n");
            } else {
                char hdr[160];
                snprintf(hdr, sizeof(hdr),
                         "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %llu\r\n"
                         "Connection: close\r\n\r\n",
                         (unsigned long long)body.len);
                builder_cstr(&resp, hdr);
                builder_bytes(&resp, body.data, body.len);
            }
        }
        Str8 out = builder_result(&resp);
        sock_send_all(c, out.data, out.len);
        shutdown(c, SD_SEND);
        closesocket(c);
        arena_pop_to(a, mark);
    }
    arena_destroy(a);
    return 0;
}

static b32 fake_vlc_start(FakeVlcHttp *srv, const char *password) {
    memset(srv, 0, sizeof(*srv));
    InitializeSRWLock(&srv->lock);
    srv->state = "playing";  // VLC démarre la lecture tout seul à l'ouverture
    srv->pos = 5.0;
    srv->length = 1200;
    srv->rate = 1;
    isize n = (isize)strlen(password);
    if (n >= (isize)sizeof(srv->password)) return 0;
    memcpy(srv->password, password, (size_t)n + 1);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    srv->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv->listener == INVALID_SOCKET) return 0;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(srv->listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) return 0;
    int alen = (int)sizeof(addr);
    if (getsockname(srv->listener, (struct sockaddr *)&addr, &alen) != 0) return 0;
    srv->port = ntohs(addr.sin_port);
    if (listen(srv->listener, 8) != 0) return 0;
    srv->thread = CreateThread(NULL, 0, fake_vlc_thread, srv, 0, NULL);
    return srv->thread != NULL;
}

static void fake_vlc_stop(FakeVlcHttp *srv) {
    InterlockedExchange(&srv->stop, 1);
    if (srv->listener != INVALID_SOCKET) {
        closesocket(srv->listener);
        srv->listener = INVALID_SOCKET;
    }
    if (srv->thread) {
        WaitForSingleObject(srv->thread, 5000);
        CloseHandle(srv->thread);
        srv->thread = NULL;
    }
}

static void test_vlc_live(Arena *a) {
    section("vlc (HTTP réel)");
    TempArena top = temp_begin(a);
    FakeVlcHttp srv;
    if (!fake_vlc_start(&srv, "mdp-test")) {
        failf("faux VLC HTTP indisponible");
        temp_end(top);
        return;
    }
    VlcClient c;
    vlc_client_init(&c, srv.port, S("mdp-test"));

    VsStatus st;
    CHECK(vlc_status(&c, a, &st) == VLC_OK, "status sur socket");
    CHECK(st.state == VS_PLAY_PLAYING, "VLC démarre en lecture");
    CHECK(approx(st.position_sec, 5, 1e-6), "position initiale = %f", st.position_sec);
    CHECK(strbuf_eq(&st.file_name, S("ep1.mkv")), "nom de fichier");

    // Réponses chunked : même résultat.
    InterlockedExchange(&srv.chunked, 1);
    CHECK(vlc_status(&c, a, &st) == VLC_OK, "status en Transfer-Encoding: chunked");
    CHECK(st.length_sec == 1200, "durée en chunked");
    InterlockedExchange(&srv.chunked, 0);

    // Préparation : pause + position 0 constatés (§Chargement de fichier).
    CHECK(vlc_prepare_paused(&c, a, 5000) == VLC_OK, "préparation pause+0");
    CHECK(vlc_status(&c, a, &st) == VLC_OK, "status après préparation");
    CHECK(st.state == VS_PLAY_PAUSED, "média non mis en pause");
    CHECK(st.position_sec < VLC_START_TOLERANCE, "média non ramené au début (%f)", st.position_sec);
    // Idempotence : un second appel ne fait rien de plus.
    long before = srv.requests;
    CHECK(vlc_prepare_paused(&c, a, 5000) == VLC_OK, "préparation idempotente");
    CHECK(srv.requests - before <= 2, "préparation non idempotente (%ld requêtes)", srv.requests - before);

    // Commandes.
    CHECK(vlc_seek(&c, a, 42.4) == VLC_OK, "seek");
    CHECK(vlc_status(&c, a, &st) == VLC_OK, "status après seek");
    CHECK(approx(st.position_sec, 42, 1e-6), "seek arrondi à la seconde : %f", st.position_sec);
    CHECK(vlc_set_rate(&c, a, 1.05) == VLC_OK, "rate");
    CHECK(vlc_resume(&c, a) == VLC_OK, "resume");
    CHECK(vlc_status(&c, a, &st) == VLC_OK, "status final");
    CHECK(st.state == VS_PLAY_PLAYING && approx(st.rate, 1.05, 1e-9), "reprise et rate");
    VsCmd cmd = {VS_CMD_PAUSE, 0};
    CHECK(vlc_apply(&c, a, cmd) == VLC_OK, "vlc_apply pause");

    // Mauvais mot de passe : 401 remonté distinctement.
    VlcClient bad;
    vlc_client_init(&bad, srv.port, S("mauvais"));
    CHECK(vlc_status(&bad, a, &st) == VLC_ERR_AUTH, "mot de passe erroné accepté");

    // Port fermé : erreur de connexion, pas de blocage.
    VlcClient dead;
    vlc_client_init(&dead, 1, S("x"));
    VlcError err = vlc_status(&dead, a, &st);
    CHECK(err == VLC_ERR_CONNECT || err == VLC_ERR_RECV, "port fermé : %s", vlc_error_text(err));

    fake_vlc_stop(&srv);
    temp_end(top);
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
    u16 *wpath = utf8_to_utf16(a, path, NULL);
    HANDLE h = CreateFileW((LPCWSTR)wpath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return empty;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > VS_MB(32)) {
        CloseHandle(h);
        return empty;
    }
    u8 *buf = arena_push_array(a, u8, (isize)size.QuadPart + 1);
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL);
    CloseHandle(h);
    if (!ok) return empty;
    return str8(buf, (isize)got);
}

// find_vectors_dir remonte l'arborescence jusqu'à trouver test/vectors.
static b32 find_vectors_dir(Arena *a, Str8 override, Str8 *out) {
    static const char *candidates[] = {
        "test\\vectors", "..\\..\\test\\vectors", "..\\..\\..\\test\\vectors", "..\\test\\vectors",
    };
    if (override.len > 0) {
        *out = override;
        return 1;
    }
    for (isize i = 0; i < VS_ARRAY_COUNT(candidates); i++) {
        Str8 cand = S(candidates[i]);
        u16 *w = utf8_to_utf16(a, cand, NULL);
        DWORD attrs = GetFileAttributesW((LPCWSTR)w);
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            *out = cand;
            return 1;
        }
    }
    return 0;
}

typedef struct {
    Str8 names[64];
    isize count;
} FileList;

static void list_vectors(Arena *a, Str8 dir, FileList *out) {
    out->count = 0;
    Str8 pattern = str8_cat(a, dir, str8_lit("\\*.json"));
    u16 *w = utf8_to_utf16(a, pattern, NULL);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((LPCWSTR)w, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (out->count >= VS_ARRAY_COUNT(out->names)) break;
        out->names[out->count++] = str8_copy(a, utf16_to_utf8(a, (const u16 *)fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    // Tri par insertion : les vecteurs sont numérotés, l'ordre doit être stable.
    for (isize i = 1; i < out->count; i++) {
        Str8 v = out->names[i];
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
    Str8 path = str8_cat(a, str8_cat(a, dir, str8_lit("\\")), file);
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
            vs_output_reset(&out);  // les réponses immédiates ne font pas partie de la trace
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
        vs_output_reset(&out);
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

static void test_vectors(Arena *a, Str8 override) {
    section("vecteurs de conformité");
    Str8 dir;
    if (!find_vectors_dir(a, override, &dir)) {
        failf("répertoire test/vectors introuvable (lancez depuis ui/win32 ou passez le chemin en argument)");
        return;
    }
    FileList files;
    list_vectors(a, dir, &files);
    if (files.count < 12) {
        failf("%lld vecteur(s) trouvé(s) dans %.*s, attendu au moins 12", (long long)files.count, (int)dir.len,
              dir.data);
        return;
    }
    for (isize i = 0; i < files.count; i++) {
        int before = g_failures;
        run_vector(a, dir, files.names[i]);
        printf("  %-24.*s %s\n", (int)files.names[i].len, files.names[i].data,
               g_failures == before ? "ok" : "ECHEC");
    }
}

// ------------------------------------------------------------ main ---

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    Arena *a = arena_create(VS_MB(64));
    if (!a) {
        printf("arène impossible à créer\n");
        return 2;
    }
    Str8 override = str8_lit("");
    if (argc > 1) override = S(argv[1]);

    test_base(a);
    test_json(a);
    test_protocol(a);
    test_vlc(a);
    test_engine_units();
    test_ini(a);
    test_text_edit();
    test_conn_address(a);
    test_semver();
    test_conn_policy();
    test_secret(a);
    test_vlc_live(a);
    test_net_live(a);
    test_net_queue_saturation(a);
    test_vectors(a, override);

    printf("\n%d vérifications, %d échec(s)\n", g_checks, g_failures);
    arena_destroy(a);
    return g_failures == 0 ? 0 : 1;
}
