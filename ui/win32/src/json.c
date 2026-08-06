#include "json.h"

#include <string.h>

const char *json_error_text(JsonError e) {
    switch (e) {
        case JSON_OK: return "ok";
        case JSON_ERR_EMPTY: return "entrée vide";
        case JSON_ERR_TOO_BIG: return "entrée trop volumineuse";
        case JSON_ERR_DEPTH: return "profondeur maximale dépassée";
        case JSON_ERR_SYNTAX: return "syntaxe invalide";
        case JSON_ERR_STRING: return "chaîne invalide";
        case JSON_ERR_NUMBER: return "nombre invalide";
        case JSON_ERR_TRAILING: return "octets en trop après la valeur";
        case JSON_ERR_UTF8: return "UTF-8 invalide";
    }
    return "erreur inconnue";
}

typedef struct {
    Arena *arena;
    Str8 in;
    isize at;
    isize depth;
    JsonError err;
} Parser;

static void p_fail(Parser *p, JsonError e) {
    if (p->err == JSON_OK) p->err = e;
}

static void p_skip_ws(Parser *p) {
    while (p->at < p->in.len) {
        u8 c = p->in.data[p->at];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            p->at++;
        } else {
            break;
        }
    }
}

static u8 p_peek(const Parser *p) { return p->at < p->in.len ? p->in.data[p->at] : 0; }

static JsonValue *p_value(Parser *p);

static JsonValue *node(Parser *p, JsonKind kind) {
    JsonValue *v = arena_push_struct(p->arena, JsonValue);
    v->kind = kind;
    return v;
}

// p_string lit une chaîne JSON déjà positionnée sur son guillemet ouvrant et
// renvoie la valeur déséchappée (allouée dans l'arène).
static b32 p_string(Parser *p, Str8 *out) {
    if (p_peek(p) != '"') {
        p_fail(p, JSON_ERR_SYNTAX);
        return 0;
    }
    p->at++;
    isize start = p->at;
    // Première passe : trouver la fin et repérer la présence d'échappements.
    b32 escaped = 0;
    isize i = start;
    for (;;) {
        if (i >= p->in.len) {
            p_fail(p, JSON_ERR_STRING);
            return 0;
        }
        u8 c = p->in.data[i];
        if (c == '"') break;
        if (c == '\\') {
            escaped = 1;
            if (i + 1 >= p->in.len) {
                p_fail(p, JSON_ERR_STRING);
                return 0;
            }
            i += 2;
            continue;
        }
        if (c < 0x20) {  // caractère de contrôle brut : interdit par la RFC
            p_fail(p, JSON_ERR_STRING);
            return 0;
        }
        i++;
    }
    isize end = i;
    Str8 raw = str8_sub(p->in, start, end - start);
    p->at = end + 1;

    if (!escaped) {
        if (!utf8_validate(raw)) {
            p_fail(p, JSON_ERR_UTF8);
            return 0;
        }
        *out = raw;
        return 1;
    }

    // Deuxième passe : déséchappement (la sortie ne peut pas être plus longue).
    u8 *buf = arena_push_array(p->arena, u8, raw.len + 1);
    isize n = 0;
    isize k = 0;
    while (k < raw.len) {
        u8 c = raw.data[k];
        if (c != '\\') {
            buf[n++] = c;
            k++;
            continue;
        }
        k++;
        if (k >= raw.len) {
            p_fail(p, JSON_ERR_STRING);
            return 0;
        }
        u8 e = raw.data[k++];
        switch (e) {
            case '"': buf[n++] = '"'; break;
            case '\\': buf[n++] = '\\'; break;
            case '/': buf[n++] = '/'; break;
            case 'b': buf[n++] = '\b'; break;
            case 'f': buf[n++] = '\f'; break;
            case 'n': buf[n++] = '\n'; break;
            case 'r': buf[n++] = '\r'; break;
            case 't': buf[n++] = '\t'; break;
            case 'u': {
                u32 cp = 0;
                if (k + 4 > raw.len) {
                    p_fail(p, JSON_ERR_STRING);
                    return 0;
                }
                for (int d = 0; d < 4; d++) {
                    u8 h = raw.data[k + d];
                    u32 nib;
                    if (h >= '0' && h <= '9') nib = (u32)(h - '0');
                    else if (h >= 'a' && h <= 'f') nib = (u32)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') nib = (u32)(h - 'A' + 10);
                    else {
                        p_fail(p, JSON_ERR_STRING);
                        return 0;
                    }
                    cp = (cp << 4) | nib;
                }
                k += 4;
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    // Paire de substitution : la seconde moitié doit suivre.
                    if (k + 6 <= raw.len && raw.data[k] == '\\' && raw.data[k + 1] == 'u') {
                        u32 lo = 0;
                        b32 ok = 1;
                        for (int d = 0; d < 4; d++) {
                            u8 h = raw.data[k + 2 + d];
                            u32 nib;
                            if (h >= '0' && h <= '9') nib = (u32)(h - '0');
                            else if (h >= 'a' && h <= 'f') nib = (u32)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') nib = (u32)(h - 'A' + 10);
                            else {
                                ok = 0;
                                break;
                            }
                            lo = (lo << 4) | nib;
                        }
                        if (ok && lo >= 0xdc00 && lo <= 0xdfff) {
                            cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                            k += 6;
                        } else {
                            cp = 0xfffd;  // moitié haute orpheline
                        }
                    } else {
                        cp = 0xfffd;
                    }
                } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                    cp = 0xfffd;  // moitié basse orpheline
                }
                n += utf8_encode(cp, buf + n);
                break;
            }
            default:
                p_fail(p, JSON_ERR_STRING);
                return 0;
        }
    }
    buf[n] = 0;
    Str8 s = {buf, n};
    if (!utf8_validate(s)) {
        p_fail(p, JSON_ERR_UTF8);
        return 0;
    }
    *out = s;
    return 1;
}

static b32 p_number(Parser *p, f64 *out) {
    isize start = p->at;
    if (p_peek(p) == '-') p->at++;
    isize digits = 0;
    if (p_peek(p) == '0') {
        p->at++;
        digits = 1;
        if (p_peek(p) >= '0' && p_peek(p) <= '9') {  // zéro non significatif
            p_fail(p, JSON_ERR_NUMBER);
            return 0;
        }
    } else {
        while (p_peek(p) >= '0' && p_peek(p) <= '9') {
            p->at++;
            digits++;
        }
    }
    if (digits == 0) {
        p_fail(p, JSON_ERR_NUMBER);
        return 0;
    }
    if (p_peek(p) == '.') {
        p->at++;
        isize frac = 0;
        while (p_peek(p) >= '0' && p_peek(p) <= '9') {
            p->at++;
            frac++;
        }
        if (frac == 0) {
            p_fail(p, JSON_ERR_NUMBER);
            return 0;
        }
    }
    if (p_peek(p) == 'e' || p_peek(p) == 'E') {
        p->at++;
        if (p_peek(p) == '+' || p_peek(p) == '-') p->at++;
        isize exp = 0;
        while (p_peek(p) >= '0' && p_peek(p) <= '9') {
            p->at++;
            exp++;
        }
        if (exp == 0) {
            p_fail(p, JSON_ERR_NUMBER);
            return 0;
        }
    }
    Str8 text = str8_sub(p->in, start, p->at - start);
    f64 v = 0;
    if (!str_to_f64(text, &v)) {
        p_fail(p, JSON_ERR_NUMBER);
        return 0;
    }
    if (!f64_is_finite(v)) {  // débordement (1e400) : refus net
        p_fail(p, JSON_ERR_NUMBER);
        return 0;
    }
    *out = v;
    return 1;
}

static b32 p_literal(Parser *p, const char *lit) {
    Str8 l = str8_from_cstr(lit);
    if (p->in.len - p->at < l.len) return 0;
    if (memcmp(p->in.data + p->at, l.data, (size_t)l.len) != 0) return 0;
    p->at += l.len;
    return 1;
}

static void child_append(JsonValue *parent, JsonValue *child) {
    if (parent->last) {
        parent->last->next = child;
    } else {
        parent->first = child;
    }
    parent->last = child;
    parent->count++;
}

static JsonValue *p_value(Parser *p) {
    if (p->err != JSON_OK) return NULL;
    if (p->depth >= JSON_MAX_DEPTH) {
        p_fail(p, JSON_ERR_DEPTH);
        return NULL;
    }
    p_skip_ws(p);
    u8 c = p_peek(p);
    switch (c) {
        case '{': {
            p->at++;
            p->depth++;
            JsonValue *obj = node(p, JSON_OBJECT);
            p_skip_ws(p);
            if (p_peek(p) == '}') {
                p->at++;
                p->depth--;
                return obj;
            }
            for (;;) {
                p_skip_ws(p);
                Str8 key;
                if (!p_string(p, &key)) return NULL;
                p_skip_ws(p);
                if (p_peek(p) != ':') {
                    p_fail(p, JSON_ERR_SYNTAX);
                    return NULL;
                }
                p->at++;
                JsonValue *v = p_value(p);
                if (!v) return NULL;
                v->key = key;
                child_append(obj, v);
                p_skip_ws(p);
                if (p_peek(p) == ',') {
                    p->at++;
                    continue;
                }
                if (p_peek(p) == '}') {
                    p->at++;
                    p->depth--;
                    return obj;
                }
                p_fail(p, JSON_ERR_SYNTAX);
                return NULL;
            }
        }
        case '[': {
            p->at++;
            p->depth++;
            JsonValue *arr = node(p, JSON_ARRAY);
            p_skip_ws(p);
            if (p_peek(p) == ']') {
                p->at++;
                p->depth--;
                return arr;
            }
            for (;;) {
                JsonValue *v = p_value(p);
                if (!v) return NULL;
                child_append(arr, v);
                p_skip_ws(p);
                if (p_peek(p) == ',') {
                    p->at++;
                    continue;
                }
                if (p_peek(p) == ']') {
                    p->at++;
                    p->depth--;
                    return arr;
                }
                p_fail(p, JSON_ERR_SYNTAX);
                return NULL;
            }
        }
        case '"': {
            JsonValue *v = node(p, JSON_STRING);
            if (!p_string(p, &v->string)) return NULL;
            return v;
        }
        case 't':
            if (p_literal(p, "true")) {
                JsonValue *v = node(p, JSON_BOOL);
                v->boolean = 1;
                return v;
            }
            p_fail(p, JSON_ERR_SYNTAX);
            return NULL;
        case 'f':
            if (p_literal(p, "false")) {
                JsonValue *v = node(p, JSON_BOOL);
                v->boolean = 0;
                return v;
            }
            p_fail(p, JSON_ERR_SYNTAX);
            return NULL;
        case 'n':
            if (p_literal(p, "null")) return node(p, JSON_NULL);
            p_fail(p, JSON_ERR_SYNTAX);
            return NULL;
        default: {
            if (c == '-' || (c >= '0' && c <= '9')) {
                JsonValue *v = node(p, JSON_NUMBER);
                if (!p_number(p, &v->number)) return NULL;
                return v;
            }
            p_fail(p, JSON_ERR_SYNTAX);
            return NULL;
        }
    }
}

JsonValue *json_parse(Arena *a, Str8 text, JsonError *err) {
    JsonError local = JSON_OK;
    if (!err) err = &local;
    *err = JSON_OK;
    if (text.len <= 0) {
        *err = JSON_ERR_EMPTY;
        return NULL;
    }
    if (text.len > JSON_MAX_INPUT) {
        *err = JSON_ERR_TOO_BIG;
        return NULL;
    }
    Parser p = {a, text, 0, 0, JSON_OK};
    JsonValue *v = p_value(&p);
    if (!v) {
        *err = p.err == JSON_OK ? JSON_ERR_SYNTAX : p.err;
        return NULL;
    }
    p_skip_ws(&p);
    if (p.at != p.in.len) {
        *err = JSON_ERR_TRAILING;
        return NULL;
    }
    return v;
}

// ------------------------------------------------------------------ accès ---

JsonValue *json_get(const JsonValue *obj, const char *key) {
    if (!obj || obj->kind != JSON_OBJECT) return NULL;
    Str8 k = str8_from_cstr(key);
    for (JsonValue *c = obj->first; c; c = c->next) {
        if (str8_eq(c->key, k)) return c;
    }
    return NULL;
}

JsonValue *json_at(const JsonValue *arr, isize index) {
    if (!arr || arr->kind != JSON_ARRAY || index < 0) return NULL;
    isize i = 0;
    for (JsonValue *c = arr->first; c; c = c->next, i++) {
        if (i == index) return c;
    }
    return NULL;
}

f64 json_num(const JsonValue *v, f64 def) {
    if (!v || v->kind != JSON_NUMBER) return def;
    return v->number;
}

i64 json_i64(const JsonValue *v, i64 def) {
    if (!v || v->kind != JSON_NUMBER) return def;
    f64 n = v->number;
    if (!f64_is_finite(n) || n > 9.2233720368547738e18 || n < -9.2233720368547738e18) return def;
    return (i64)n;
}

b32 json_bool(const JsonValue *v, b32 def) {
    if (!v || v->kind != JSON_BOOL) return def;
    return v->boolean;
}

Str8 json_str(const JsonValue *v, Str8 def) {
    if (!v || v->kind != JSON_STRING) return def;
    return v->string;
}

f64 json_get_num(const JsonValue *obj, const char *key, f64 def) { return json_num(json_get(obj, key), def); }
i64 json_get_i64(const JsonValue *obj, const char *key, i64 def) { return json_i64(json_get(obj, key), def); }
b32 json_get_bool(const JsonValue *obj, const char *key, b32 def) { return json_bool(json_get(obj, key), def); }
Str8 json_get_str(const JsonValue *obj, const char *key, Str8 def) { return json_str(json_get(obj, key), def); }

// --------------------------------------------------------------- écriture ---

void jw_init(JsonWriter *w, Arena *a) {
    memset(w, 0, sizeof(*w));
    builder_init(&w->out, a, 256);
}

// jw_pre place la virgule de séparation si nécessaire.
static void jw_pre(JsonWriter *w) {
    if (w->pending_key) {
        w->pending_key = 0;
        return;
    }
    if (w->depth > 0) {
        if (w->has_item[w->depth]) builder_byte(&w->out, ',');
        w->has_item[w->depth] = 1;
    }
}

static void jw_open(JsonWriter *w, u8 c) {
    jw_pre(w);
    if (w->depth >= JSON_MAX_DEPTH) {
        w->overflow = 1;
        w->depth++;  // suivi symétrique pour que la fermeture reste cohérente
        return;
    }
    builder_byte(&w->out, c);
    w->depth++;
    w->has_item[w->depth] = 0;
}

static void jw_close(JsonWriter *w, u8 c) {
    if (w->depth <= 0) {
        w->overflow = 1;
        return;
    }
    w->depth--;
    if (w->depth >= JSON_MAX_DEPTH) return;
    builder_byte(&w->out, c);
}

void jw_obj_begin(JsonWriter *w) { jw_open(w, '{'); }
void jw_obj_end(JsonWriter *w) { jw_close(w, '}'); }
void jw_arr_begin(JsonWriter *w) { jw_open(w, '['); }
void jw_arr_end(JsonWriter *w) { jw_close(w, ']'); }

static void jw_raw_string(JsonWriter *w, Str8 v) {
    builder_byte(&w->out, '"');
    for (isize i = 0; i < v.len; i++) {
        u8 c = v.data[i];
        switch (c) {
            case '"': builder_cstr(&w->out, "\\\""); break;
            case '\\': builder_cstr(&w->out, "\\\\"); break;
            case '\n': builder_cstr(&w->out, "\\n"); break;
            case '\r': builder_cstr(&w->out, "\\r"); break;
            case '\t': builder_cstr(&w->out, "\\t"); break;
            case '\b': builder_cstr(&w->out, "\\b"); break;
            case '\f': builder_cstr(&w->out, "\\f"); break;
            default:
                if (c < 0x20) {
                    static const char hex[] = "0123456789abcdef";
                    char esc[6] = {'\\', 'u', '0', '0', hex[(c >> 4) & 0xf], hex[c & 0xf]};
                    builder_bytes(&w->out, esc, 6);
                } else {
                    builder_byte(&w->out, c);
                }
        }
    }
    builder_byte(&w->out, '"');
}

void jw_key(JsonWriter *w, const char *key) {
    if (w->depth > 0) {
        if (w->has_item[w->depth]) builder_byte(&w->out, ',');
        w->has_item[w->depth] = 1;
    }
    jw_raw_string(w, str8_from_cstr(key));
    builder_byte(&w->out, ':');
    // La valeur qui suit ne doit pas poser une seconde virgule.
    w->pending_key = 1;
}

void jw_str(JsonWriter *w, Str8 v) {
    jw_pre(w);
    jw_raw_string(w, v);
}

void jw_cstr(JsonWriter *w, const char *v) { jw_str(w, str8_from_cstr(v)); }

void jw_num(JsonWriter *w, f64 v) {
    jw_pre(w);
    if (!f64_is_finite(v)) {
        builder_byte(&w->out, '0');
        return;
    }
    builder_f64(&w->out, v);
}

void jw_i64(JsonWriter *w, i64 v) {
    jw_pre(w);
    builder_i64(&w->out, v);
}

void jw_bool(JsonWriter *w, b32 v) {
    jw_pre(w);
    builder_cstr(&w->out, v ? "true" : "false");
}

void jw_null(JsonWriter *w) {
    jw_pre(w);
    builder_cstr(&w->out, "null");
}

void jw_kv_str(JsonWriter *w, const char *key, Str8 v) {
    jw_key(w, key);
    jw_str(w, v);
}
void jw_kv_num(JsonWriter *w, const char *key, f64 v) {
    jw_key(w, key);
    jw_num(w, v);
}
void jw_kv_i64(JsonWriter *w, const char *key, i64 v) {
    jw_key(w, key);
    jw_i64(w, v);
}
void jw_kv_bool(JsonWriter *w, const char *key, b32 v) {
    jw_key(w, key);
    jw_bool(w, v);
}

Str8 jw_result(const JsonWriter *w) { return builder_result(&w->out); }
