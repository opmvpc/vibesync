#include "protocol.h"

#include <string.h>

// --------------------------------------------------------------- encodage ---

// env_begin ouvre l'enveloppe {"type":…,"data":{…}}.
static void env_begin(JsonWriter *w, const char *type) {
    jw_obj_begin(w);
    jw_key(w, "type");
    jw_cstr(w, type);
    jw_key(w, "data");
    jw_obj_begin(w);
}

static Str8 env_end(JsonWriter *w) {
    jw_obj_end(w);  // data
    jw_obj_end(w);  // enveloppe
    return jw_result(w);
}

Str8 proto_encode_hello(Arena *a, Str8 name, Str8 room, Str8 password, Str8 session) {
    JsonWriter w;
    jw_init(&w, a);
    env_begin(&w, "hello");
    jw_kv_i64(&w, "version", VS_PROTOCOL_VERSION);
    jw_kv_str(&w, "name", name);
    jw_kv_str(&w, "room", room);
    if (password.len > 0) jw_kv_str(&w, "password", password);
    if (session.len > 0) jw_kv_str(&w, "session", session);
    return env_end(&w);
}

Str8 proto_encode_msg(Arena *a, const VsMsg *m) {
    JsonWriter w;
    jw_init(&w, a);
    switch (m->kind) {
        case VS_MSG_PING:
            env_begin(&w, "ping");
            jw_kv_i64(&w, "t", m->t);
            break;
        case VS_MSG_SET_READY:
            env_begin(&w, "setReady");
            jw_kv_bool(&w, "ready", m->ready);
            break;
        case VS_MSG_SET_FILE:
            env_begin(&w, "setFile");
            jw_kv_str(&w, "name", strbuf_str(&m->name));
            jw_kv_num(&w, "durationSec", m->duration_sec);
            jw_kv_i64(&w, "sizeBytes", m->size_bytes);
            break;
        case VS_MSG_CONTROL:
            env_begin(&w, "control");
            jw_key(&w, "action");
            jw_cstr(&w, vs_action_name(m->action));
            jw_kv_num(&w, "positionSec", m->position_sec);
            break;
        case VS_MSG_REPORT:
            env_begin(&w, "report");
            jw_kv_num(&w, "positionSec", m->position_sec);
            jw_kv_bool(&w, "paused", m->paused);
            jw_kv_bool(&w, "buffering", m->buffering);
            break;
        case VS_MSG_CHAT:
            env_begin(&w, "chat");
            jw_kv_str(&w, "text", strbuf_str(&m->text));
            break;
    }
    return env_end(&w);
}

void proto_hex(const u8 *bytes, isize n, char *out) { vs_hex_encode(bytes, n, out); }

b32 proto_session_token(char *out, isize cap) {
    if (cap < VS_SESSION_TOKEN_LEN + 1) return 0;
    u8 raw[VS_SESSION_TOKEN_BYTES];
    if (!vs_random_bytes(raw, (isize)sizeof(raw))) return 0;
    vs_hex_encode(raw, (isize)sizeof(raw), out);
    return 1;
}

b32 proto_session_token_valid(Str8 token) {
    if (token.len < VS_SESSION_TOKEN_LEN || token.len > VS_SESSION_TOKEN_MAX) return 0;
    if (token.len % 2 != 0) return 0;
    for (isize i = 0; i < token.len; i++) {
        u8 c = token.data[i];
        b32 hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) return 0;
    }
    return 1;
}

// --------------------------------------------------------------- décodage ---

// --- lecture stricte : présence ET type sont exigés ---
//
// Un champ obligatoire absent ou d'un type inattendu invalide le message : il
// est ignoré (docs/protocol.md §forward-compat) au lieu de se transformer en
// zéro silencieux (un `pong` vide deviendrait {t:0, serverMs:0} et empoisonnerait
// l'offset d'horloge).

static b32 need_num(const JsonValue *o, const char *key, f64 *out) {
    const JsonValue *v = json_get(o, key);
    if (!v || v->kind != JSON_NUMBER || !f64_is_finite(v->number)) return 0;
    *out = v->number;
    return 1;
}

static b32 need_ms(const JsonValue *o, const char *key, i64 *out) {
    f64 raw = 0;
    if (!need_num(o, key, &raw)) return 0;
    if (raw < (f64)VS_MS_MIN || raw > (f64)VS_MS_MAX) return 0;
    *out = (i64)raw;
    return vs_valid_epoch_ms(*out);
}

static b32 need_bool(const JsonValue *o, const char *key, b32 *out) {
    const JsonValue *v = json_get(o, key);
    if (!v || v->kind != JSON_BOOL) return 0;
    *out = v->boolean;
    return 1;
}

static b32 need_str(const JsonValue *o, const char *key, Str8 *out) {
    const JsonValue *v = json_get(o, key);
    if (!v || v->kind != JSON_STRING) return 0;
    *out = v->string;
    return 1;
}

// read_roomstate exige paused, positionSec, rate et refServerMs (sauf en
// pause, où une référence absente ou nulle est admise).
static b32 read_roomstate(const JsonValue *o, VsRoomState *rs) {
    memset(rs, 0, sizeof(*rs));
    if (!o || o->kind != JSON_OBJECT) return 0;
    if (!need_bool(o, "paused", &rs->paused)) return 0;
    if (!need_num(o, "positionSec", &rs->position_sec)) return 0;
    if (!need_num(o, "rate", &rs->rate)) return 0;
    if (!need_ms(o, "refServerMs", &rs->ref_server_ms)) {
        if (!rs->paused) return 0;  // en lecture, la référence est obligatoire
        rs->ref_server_ms = 0;
    }
    Str8 set_by;
    if (need_str(o, "setBy", &set_by)) strbuf_set(&rs->set_by, set_by);
    return 1;
}

static isize read_users(Arena *a, const JsonValue *arr, VsUser **out) {
    *out = NULL;
    if (!arr || arr->kind != JSON_ARRAY || arr->count <= 0) return 0;
    isize n = arr->count;
    if (n > VS_MAX_USERS) n = VS_MAX_USERS;
    VsUser *users = arena_push_array(a, VsUser, n);
    isize i = 0;
    for (JsonValue *c = arr->first; c && i < n; c = c->next) {
        if (c->kind != JSON_OBJECT) continue;
        Str8 id;
        if (!need_str(c, "id", &id) || id.len == 0) continue;  // entrée inexploitable
        VsUser *u = &users[i++];
        u->id = id;
        if (!need_str(c, "name", &u->name)) u->name = str8_lit("");
        u->ready = json_get_bool(c, "ready", 0);
        u->position_sec = json_get_num(c, "positionSec", 0);
        if (!f64_is_finite(u->position_sec)) u->position_sec = 0;
        u->latency_ms = json_get_i64(c, "latencyMs", 0);
        if (u->latency_ms < 0 || u->latency_ms > 600000) u->latency_ms = 0;
        JsonValue *f = json_get(c, "file");
        if (f && f->kind == JSON_OBJECT) {
            u->has_file = 1;
            u->file_name = json_get_str(f, "name", str8_lit(""));
            u->file_duration_sec = json_get_num(f, "durationSec", 0);
            u->file_size_bytes = json_get_i64(f, "sizeBytes", 0);
        }
    }
    *out = users;
    return i;
}

VsInMsg *proto_decode(Arena *a, Str8 raw) {
    JsonError err = JSON_OK;
    JsonValue *root = json_parse(a, raw, &err);
    if (!root || root->kind != JSON_OBJECT) return NULL;
    JsonValue *type = json_get(root, "type");
    if (!type || type->kind != JSON_STRING || type->string.len == 0) return NULL;
    JsonValue *data = json_get(root, "data");

    VsInMsg *m = arena_push_struct(a, VsInMsg);
    proto_fill(a, type->string, data, m);
    return m;
}

void proto_fill(Arena *a, Str8 type, const JsonValue *data, VsInMsg *m) {
    memset(m, 0, sizeof(*m));
    m->type = type;
    m->kind = VS_IN_UNKNOWN;

    if (str8_eq_cstr(m->type, "welcome")) {
        // selfId et state sont obligatoires : sans eux le welcome ne peut pas
        // servir de référence de session.
        if (!need_str(data, "selfId", &m->self_id) || m->self_id.len == 0) {
            m->invalid = 1;
            return;
        }
        if (!read_roomstate(json_get(data, "state"), &m->state)) {
            m->invalid = 1;
            return;
        }
        m->have_state = 1;
        m->kind = VS_IN_WELCOME;
        Str8 room;
        if (need_str(data, "room", &room)) m->room = room;
        // Champs additifs : un serveur qui ne les envoie pas reste valide.
        Str8 sv;
        if (need_str(data, "serverVersion", &sv)) m->server_version = sv;
        if (need_str(data, "downloadUrl", &sv)) m->download_url = sv;
        m->user_count = read_users(a, json_get(data, "users"), &m->users);
        for (isize i = 0; i < m->user_count; i++) {
            if (str8_eq(m->users[i].id, m->self_id)) {
                m->have_self_ready = 1;
                m->self_ready = m->users[i].ready;
                break;
            }
        }
    } else if (str8_eq_cstr(m->type, "pong")) {
        if (!need_ms(data, "t", &m->pong.t) || !need_ms(data, "serverMs", &m->pong.server_ms)) {
            m->invalid = 1;
            return;
        }
        m->kind = VS_IN_PONG;
    } else if (str8_eq_cstr(m->type, "roomState")) {
        if (!read_roomstate(data, &m->state)) {
            m->invalid = 1;
            return;
        }
        m->have_state = 1;
        m->kind = VS_IN_ROOMSTATE;
    } else if (str8_eq_cstr(m->type, "users")) {
        m->kind = VS_IN_USERS;
        m->user_count = read_users(a, json_get(data, "users"), &m->users);
    } else if (str8_eq_cstr(m->type, "chatEvent") || str8_eq_cstr(m->type, "chat")) {
        if (!need_str(data, "from", &m->from) || !need_str(data, "text", &m->text)) {
            m->invalid = 1;
            return;
        }
        m->kind = VS_IN_CHATEVENT;
        i64 ms = 0;
        if (need_ms(data, "serverMs", &ms)) m->server_ms = ms;
    } else if (str8_eq_cstr(m->type, "toast")) {
        if (!need_str(data, "text", &m->text)) {
            m->invalid = 1;
            return;
        }
        m->kind = VS_IN_TOAST;
        m->level = str8_lit("info");
        Str8 level;
        if (need_str(data, "level", &level)) m->level = level;
    } else if (str8_eq_cstr(m->type, "error")) {
        if (!need_str(data, "code", &m->code) || m->code.len == 0) {
            m->invalid = 1;
            return;
        }
        m->kind = VS_IN_ERROR;
        Str8 text;
        if (need_str(data, "text", &text)) m->text = text;
    }
}

b32 proto_error_is_fatal(Str8 code) {
    return str8_eq_cstr(code, "version_mismatch") || str8_eq_cstr(code, "bad_password") ||
           str8_eq_cstr(code, "name_taken");
}

// ---------------------------------------------------------------- versions ---
//
// Portage EXACT de `NewerVersion` / `parseVersion` (internal/client/version.go),
// implémentation de référence (VS-036). Ce qui précédait ici, `proto_semver_cmp`,
// était plus laxiste — pas de rognage, pas de notion d'illisibilité, suffixes de
// pré-version ignorés — et faisait diverger la bannière de mise à jour de neuf
// cas sur trente-cinq, dans les deux sens.
//
// La règle tient en une phrase : une version illisible (« dev », vide, texte)
// ne se compare à RIEN. Dans le doute, pas de bannière ; le garde-fou dur de
// compatibilité reste la version de PROTOCOLE, refusée par le serveur au hello.
//
// Format accepté : `major[.minor[.patch]]`, chiffres seulement, avec un « v »
// initial optionnel, un suffixe de pré-version (`-rc1`) et des métadonnées de
// build (`+sha`). Les composants absents valent 0. À triplet égal, une
// pré-version est ANTÉRIEURE à la version nue (1.2.3-rc1 < 1.2.3) ; deux
// pré-versions du même triplet ne sont pas départagées (l'ordre alphabétique
// mentirait sur rc10 vs rc2).

// VS_VERSION_MAX_PART borne chaque composant : au-delà, c'est une saisie
// absurde plutôt qu'une version — et le calcul ne peut pas déborder.
#define VS_VERSION_MAX_PART 1000000

typedef struct {
    i64 parts[3];
    b32 pre;
} SemVer;

// version_trim rogne comme `strings.TrimSpace` : str8_trim ne connaît pas la
// tabulation verticale ni le saut de page, que Go coupe aussi. Les espaces
// Unicode exotiques (U+00A0, U+2028…) ne sont, eux, pas rognés : la version
// reste illisible, donc sans bannière — l'écart tombe du côté prudent.
static b32 version_is_space(u8 c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static Str8 version_trim(Str8 s) {
    isize i = 0, j = s.len;
    while (i < j && version_is_space(s.data[i])) i++;
    while (j > i && version_is_space(s.data[j - 1])) j--;
    return str8_sub(s, i, j - i);
}

// version_num lit un composant comme `strconv.Atoi` suivi des bornes de la
// référence : chiffres seulement, un signe toléré (« +1 »), négatif refusé,
// au-delà de la borne refusé. Aucun signe ne peut en pratique survivre aux
// coupes sur « + » et « - » faites plus haut ; on colle à Atoi quand même.
static b32 version_num(Str8 p, i64 *out) {
    if (p.len == 0) return 0;
    isize i = 0;
    b32 neg = 0;
    if (p.data[0] == '+' || p.data[0] == '-') {
        neg = p.data[0] == '-';
        i = 1;
    }
    if (i >= p.len) return 0;
    i64 v = 0;
    for (; i < p.len; i++) {
        u8 c = p.data[i];
        if (c < '0' || c > '9') return 0;
        v = v * 10 + (c - '0');
        if (v > VS_VERSION_MAX_PART) return 0;  // hors borne : Atoi déborderait
    }
    if (neg && v != 0) return 0;
    *out = v;
    return 1;
}

// version_parse découpe une version. Renvoie 0 si elle est illisible.
static b32 version_parse(Str8 s, SemVer *out) {
    out->parts[0] = out->parts[1] = out->parts[2] = 0;
    out->pre = 0;
    s = version_trim(s);
    // TrimPrefix « v » : minuscule seulement, comme la référence.
    if (s.len > 0 && s.data[0] == 'v') s = str8_sub(s, 1, -1);
    // Métadonnées de build : hors de l'ordre, on les coupe d'abord.
    isize plus = str8_find_char(s, '+', 0);
    if (plus >= 0) s = str8_sub(s, 0, plus);
    // Pré-version : elle ne change pas le triplet mais le déclasse.
    isize dash = str8_find_char(s, '-', 0);
    if (dash >= 0) {
        out->pre = s.len > dash + 1;
        s = str8_sub(s, 0, dash);
    }
    if (s.len == 0) return 0;
    // Découpe sur « . ». Plus de trois composants : illisible. Espaces et
    // tabulations internes, que la référence écarte explicitement, ne passent
    // pas la lecture chiffre par chiffre de version_num.
    int n = 0;
    isize start = 0;
    for (isize i = 0; i <= s.len; i++) {
        if (i < s.len && s.data[i] != '.') continue;
        if (n >= 3) return 0;
        if (!version_num(str8_sub(s, start, i - start), &out->parts[n])) return 0;
        n++;
        start = i + 1;
    }
    return 1;
}

b32 proto_newer_version(Str8 server, Str8 client) {
    SemVer r, l;
    if (!version_parse(server, &r) || !version_parse(client, &l)) return 0;
    for (int i = 0; i < 3; i++) {
        if (r.parts[i] != l.parts[i]) return r.parts[i] > l.parts[i];
    }
    // Même triplet : seule une version nue dépasse une pré-version.
    return !r.pre && l.pre;
}
