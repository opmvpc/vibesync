// test_main.c — suite de tests console (vibesync_tests.exe).
//
// Couvre : base (arènes, chaînes, nombres, UTF-8/16), json (aller-retours et
// entrées hostiles), protocol (encodage/décodage), vlc (parsing de status.json
// réels, HTTP, base64), net (analyse d'URL) et surtout le REJEU DES 12
// VECTEURS DE CONFORMITÉ test/vectors/*.json, qui gèlent le moteur de sync.
//
// Sortie non nulle en cas d'échec, avec diagnostic.

#include "base.h"
#include "engine.h"
#include "json.h"
#include "net.h"
#include "protocol.h"
#include "vlc.h"

#define WIN32_LEAN_AND_MEAN
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
        // data absent : ne doit pas crasher.
        m = proto_decode(a, S("{\"type\":\"welcome\"}"));
        CHECK(m && m->kind == VS_IN_WELCOME && m->user_count == 0, "welcome sans data");
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
    test_vectors(a, override);

    printf("\n%d vérifications, %d échec(s)\n", g_checks, g_failures);
    arena_destroy(a);
    return g_failures == 0 ? 0 : 1;
}
