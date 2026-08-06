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

static void read_roomstate(const JsonValue *o, VsRoomState *rs) {
    memset(rs, 0, sizeof(*rs));
    rs->paused = json_get_bool(o, "paused", 0);
    rs->position_sec = json_get_num(o, "positionSec", 0);
    rs->rate = json_get_num(o, "rate", 0);
    rs->ref_server_ms = json_get_i64(o, "refServerMs", 0);
    strbuf_set(&rs->set_by, json_get_str(o, "setBy", str8_lit("")));
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
        VsUser *u = &users[i++];
        u->id = json_get_str(c, "id", str8_lit(""));
        u->name = json_get_str(c, "name", str8_lit(""));
        u->ready = json_get_bool(c, "ready", 0);
        u->position_sec = json_get_num(c, "positionSec", 0);
        u->latency_ms = json_get_i64(c, "latencyMs", 0);
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
        m->kind = VS_IN_WELCOME;
        m->self_id = json_get_str(data, "selfId", str8_lit(""));
        m->room = json_get_str(data, "room", str8_lit(""));
        JsonValue *st = json_get(data, "state");
        if (st && st->kind == JSON_OBJECT) {
            read_roomstate(st, &m->state);
            m->have_state = 1;
        }
        m->user_count = read_users(a, json_get(data, "users"), &m->users);
        for (isize i = 0; i < m->user_count; i++) {
            if (m->self_id.len > 0 && str8_eq(m->users[i].id, m->self_id)) {
                m->have_self_ready = 1;
                m->self_ready = m->users[i].ready;
                break;
            }
        }
    } else if (str8_eq_cstr(m->type, "pong")) {
        m->kind = VS_IN_PONG;
        m->pong.t = json_get_i64(data, "t", 0);
        m->pong.server_ms = json_get_i64(data, "serverMs", 0);
    } else if (str8_eq_cstr(m->type, "roomState")) {
        m->kind = VS_IN_ROOMSTATE;
        if (data && data->kind == JSON_OBJECT) {
            read_roomstate(data, &m->state);
            m->have_state = 1;
        }
    } else if (str8_eq_cstr(m->type, "users")) {
        m->kind = VS_IN_USERS;
        m->user_count = read_users(a, json_get(data, "users"), &m->users);
    } else if (str8_eq_cstr(m->type, "chatEvent") || str8_eq_cstr(m->type, "chat")) {
        m->kind = VS_IN_CHATEVENT;
        m->from = json_get_str(data, "from", str8_lit(""));
        m->text = json_get_str(data, "text", str8_lit(""));
        m->server_ms = json_get_i64(data, "serverMs", 0);
    } else if (str8_eq_cstr(m->type, "toast")) {
        m->kind = VS_IN_TOAST;
        m->level = json_get_str(data, "level", str8_lit("info"));
        m->text = json_get_str(data, "text", str8_lit(""));
    } else if (str8_eq_cstr(m->type, "error")) {
        m->kind = VS_IN_ERROR;
        m->code = json_get_str(data, "code", str8_lit(""));
        m->text = json_get_str(data, "text", str8_lit(""));
    }
}

b32 proto_error_is_fatal(Str8 code) {
    return str8_eq_cstr(code, "version_mismatch") || str8_eq_cstr(code, "bad_password") ||
           str8_eq_cstr(code, "name_taken");
}
