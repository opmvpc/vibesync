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

// semver_part lit le composant numérique commençant en *i et avance *i au
// séparateur suivant. Tout ce qui n'est pas un chiffre arrête le composant ;
// un suffixe de pré-version fait tomber le reste à zéro.
static i64 semver_part(Str8 s, isize *i, b32 *stop) {
    i64 v = 0;
    b32 any = 0;
    while (*i < s.len) {
        u8 c = s.data[*i];
        if (c >= '0' && c <= '9') {
            if (v < 1000000) v = v * 10 + (c - '0');  // borne : pas de débordement
            any = 1;
            (*i)++;
            continue;
        }
        break;
    }
    if (*i < s.len && s.data[*i] == '.') (*i)++;
    else *stop = 1;  // fin, ou suffixe « -rc1 » / « +build » : on s'arrête là
    if (!any) v = 0;
    return v;
}

int proto_semver_cmp(Str8 a, Str8 b) {
    if (a.len > 0 && (a.data[0] == 'v' || a.data[0] == 'V')) a = str8_sub(a, 1, -1);
    if (b.len > 0 && (b.data[0] == 'v' || b.data[0] == 'V')) b = str8_sub(b, 1, -1);
    isize ia = 0, ib = 0;
    b32 stop_a = 0, stop_b = 0;
    for (int k = 0; k < 3; k++) {
        i64 va = stop_a ? 0 : semver_part(a, &ia, &stop_a);
        i64 vb = stop_b ? 0 : semver_part(b, &ib, &stop_b);
        if (va < vb) return -1;
        if (va > vb) return 1;
    }
    return 0;
}
