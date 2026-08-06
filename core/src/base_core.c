// base_core.c — socle PORTABLE (ADR-010, VS-030).
//
// C11 pur : aucune API du système, aucun wchar_t, tout texte en UTF-8. Ce
// fichier part tel quel dans la couche commune win32/macOS. Les arènes,
// l'horloge, l'aléa, le journal et les conversions UTF-16 vivent dans le
// fichier plateforme (base_win32.c).
#include "base.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- chaînes ---

Str8 str8(u8 *data, isize len) {
    Str8 s = {data, len};
    return s;
}

Str8 str8_from_cstr(const char *s) {
    Str8 r = {(u8 *)s, 0};
    if (s) r.len = (isize)strlen(s);
    return r;
}

b32 str8_eq(Str8 a, Str8 b) {
    if (a.len != b.len) return 0;
    if (a.len == 0) return 1;
    return memcmp(a.data, b.data, (size_t)a.len) == 0;
}

b32 str8_eq_cstr(Str8 a, const char *b) { return str8_eq(a, str8_from_cstr(b)); }

Str8 str8_copy(Arena *a, Str8 s) {
    Str8 r;
    r.len = s.len;
    r.data = arena_push_array(a, u8, s.len + 1);
    if (s.len) memcpy(r.data, s.data, (size_t)s.len);
    r.data[s.len] = 0;
    return r;
}

Str8 str8_cat(Arena *a, Str8 x, Str8 y) {
    Str8 r;
    r.len = x.len + y.len;
    r.data = arena_push_array(a, u8, r.len + 1);
    if (x.len) memcpy(r.data, x.data, (size_t)x.len);
    if (y.len) memcpy(r.data + x.len, y.data, (size_t)y.len);
    r.data[r.len] = 0;
    return r;
}

char *str8_cstr(Arena *a, Str8 s) { return (char *)str8_copy(a, s).data; }

b32 str8_starts_with(Str8 s, Str8 prefix) {
    if (prefix.len > s.len) return 0;
    return memcmp(s.data, prefix.data, (size_t)prefix.len) == 0;
}

isize str8_find_char(Str8 s, u8 c, isize from) {
    for (isize i = VS_MAX(from, 0); i < s.len; i++) {
        if (s.data[i] == c) return i;
    }
    return -1;
}

Str8 str8_sub(Str8 s, isize from, isize len) {
    if (from < 0) from = 0;
    if (from > s.len) from = s.len;
    isize max = s.len - from;
    if (len < 0 || len > max) len = max;
    Str8 r = {s.data + from, len};
    return r;
}

Str8 str8_trim(Str8 s) {
    isize i = 0, j = s.len;
    while (i < j && (s.data[i] == ' ' || s.data[i] == '\t' || s.data[i] == '\r' || s.data[i] == '\n')) i++;
    while (j > i && (s.data[j - 1] == ' ' || s.data[j - 1] == '\t' || s.data[j - 1] == '\r' || s.data[j - 1] == '\n')) j--;
    Str8 r = {s.data + i, j - i};
    return r;
}

void strbuf_set(StrBuf *b, Str8 s) {
    isize n = s.len;
    if (n > VS_STRBUF_CAP - 1) n = VS_STRBUF_CAP - 1;
    if (n > 0) memcpy(b->data, s.data, (size_t)n);
    b->data[n] = 0;
    b->len = n;
}

Str8 strbuf_str(const StrBuf *b) {
    Str8 s = {(u8 *)b->data, b->len};
    return s;
}

b32 strbuf_eq(const StrBuf *b, Str8 s) { return str8_eq(strbuf_str(b), s); }

// ----------------------------------------------------------- constructeur ---

void builder_init(Builder *b, Arena *a, isize initial_cap) {
    if (initial_cap < 64) initial_cap = 64;
    b->arena = a;
    b->cap = initial_cap;
    b->len = 0;
    b->data = arena_push_array(a, u8, initial_cap);
}

static void builder_grow(Builder *b, isize need) {
    if (b->len + need <= b->cap) return;
    isize new_cap = b->cap * 2;
    while (new_cap < b->len + need) new_cap *= 2;
    // Extension en place si le tampon est la dernière allocation de l'arène.
    isize pos = arena_pos(b->arena);
    u8 *arena_end = b->data + b->cap;
    u8 *cur_end = (u8 *)arena_push(b->arena, 0, 1);
    if (cur_end == arena_end) {
        arena_push(b->arena, new_cap - b->cap, 1);
        b->cap = new_cap;
        return;
    }
    arena_pop_to(b->arena, pos);
    u8 *fresh = arena_push_array(b->arena, u8, new_cap);
    memcpy(fresh, b->data, (size_t)b->len);
    b->data = fresh;
    b->cap = new_cap;
}

void builder_bytes(Builder *b, const void *data, isize len) {
    if (len <= 0) return;
    builder_grow(b, len);
    memcpy(b->data + b->len, data, (size_t)len);
    b->len += len;
}

void builder_str(Builder *b, Str8 s) { builder_bytes(b, s.data, s.len); }
void builder_cstr(Builder *b, const char *s) { builder_str(b, str8_from_cstr(s)); }

void builder_byte(Builder *b, u8 c) {
    builder_grow(b, 1);
    b->data[b->len++] = c;
}

void builder_i64(Builder *b, i64 v) {
    char tmp[24];
    isize n = i64_to_str(v, tmp, sizeof(tmp));
    builder_bytes(b, tmp, n);
}

void builder_f64(Builder *b, f64 v) {
    char tmp[40];
    isize n = f64_to_str(v, tmp, sizeof(tmp));
    if (n == 0) {
        builder_cstr(b, "0");
        return;
    }
    builder_bytes(b, tmp, n);
}

Str8 builder_result(const Builder *b) {
    Str8 s = {b->data, b->len};
    return s;
}

// ---------------------------------------------------------------- nombres ---

b32 f64_is_finite(f64 v) {
    // Test par les bits : exposant tout à 1 = NaN ou infini.
    u64 bits;
    memcpy(&bits, &v, sizeof(bits));
    return ((bits >> 52) & 0x7ff) != 0x7ff;
}

f64 f64_abs(f64 v) { return v < 0 ? -v : v; }

f64 f64_round(f64 v) {
    if (!f64_is_finite(v)) return v;
    // Au-delà de 2^52, tout double est déjà entier.
    if (v >= 4503599627370496.0 || v <= -4503599627370496.0) return v;
    // Arrondi au plus proche, moitiés éloignées de zéro (comme math.Round).
    if (v >= 0) {
        f64 f = (f64)(i64)v;
        if (v - f >= 0.5) f += 1;
        return f;
    }
    f64 f = (f64)(i64)v;
    if (f - v >= 0.5) f -= 1;
    return f;
}

isize f64_to_str(f64 v, char *buf, isize cap) {
    if (!f64_is_finite(v)) return 0;
    for (int prec = 15; prec <= 17; prec++) {
        int n = snprintf(buf, (size_t)cap, "%.*g", prec, v);
        if (n < 0 || n >= cap) continue;
        if (strtod(buf, NULL) == v) return (isize)n;
    }
    int n = snprintf(buf, (size_t)cap, "%.17g", v);
    if (n < 0) return 0;
    return (isize)VS_MIN((isize)n, cap - 1);
}

b32 str_to_f64(Str8 s, f64 *out) {
    char tmp[512];
    if (s.len <= 0 || s.len >= (isize)sizeof(tmp)) return 0;
    memcpy(tmp, s.data, (size_t)s.len);
    tmp[s.len] = 0;
    char *end = NULL;
    f64 v = strtod(tmp, &end);
    if (end != tmp + s.len) return 0;
    *out = v;
    return 1;
}

isize i64_to_str(i64 v, char *buf, isize cap) {
    char tmp[24];
    isize n = 0;
    b32 neg = v < 0;
    u64 u = neg ? (u64)(-(v + 1)) + 1 : (u64)v;
    do {
        tmp[n++] = (char)('0' + (u % 10));
        u /= 10;
    } while (u);
    if (neg) tmp[n++] = '-';
    if (n >= cap) return 0;
    for (isize i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
    return n;
}

b32 str_to_i64(Str8 s, i64 *out) {
    s = str8_trim(s);
    if (s.len == 0) return 0;
    isize i = 0;
    b32 neg = 0;
    if (s.data[0] == '-' || s.data[0] == '+') {
        neg = s.data[0] == '-';
        i = 1;
    }
    if (i >= s.len) return 0;
    u64 acc = 0;
    for (; i < s.len; i++) {
        u8 c = s.data[i];
        if (c < '0' || c > '9') return 0;
        if (acc > (u64)0x7fffffffffffffffULL / 10) return 0;
        acc = acc * 10 + (u64)(c - '0');
        if (acc > (u64)0x8000000000000000ULL) return 0;
    }
    if (!neg && acc > (u64)0x7fffffffffffffffULL) return 0;
    *out = neg ? -(i64)acc : (i64)acc;
    return 1;
}

// ---------------------------------------------------------------- unicode ---

isize utf8_encode(u32 cp, u8 *out) {
    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
    if (cp < 0x80) {
        out[0] = (u8)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (u8)(0xc0 | (cp >> 6));
        out[1] = (u8)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (u8)(0xe0 | (cp >> 12));
        out[1] = (u8)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (u8)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (u8)(0xf0 | (cp >> 18));
    out[1] = (u8)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (u8)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (u8)(0x80 | (cp & 0x3f));
    return 4;
}

// utf8_decode lit un point de code ; renvoie le nombre d'octets consommés,
// 0 si la séquence est invalide (`*cp` reçoit alors U+FFFD). Exposé (base.h)
// parce que la conversion UTF-16 de la plateforme s'appuie dessus.
isize utf8_decode(Str8 s, isize at, u32 *cp) {
    *cp = 0xfffd;
    if (at >= s.len) return 0;
    u8 c = s.data[at];
    isize need;
    u32 v;
    u32 lo;
    if (c < 0x80) {
        *cp = c;
        return 1;
    } else if ((c & 0xe0) == 0xc0) {
        need = 2;
        v = c & 0x1fu;
        lo = 0x80;
    } else if ((c & 0xf0) == 0xe0) {
        need = 3;
        v = c & 0x0fu;
        lo = 0x800;
    } else if ((c & 0xf8) == 0xf0) {
        need = 4;
        v = c & 0x07u;
        lo = 0x10000;
    } else {
        return 0;
    }
    if (at + need > s.len) return 0;
    for (isize i = 1; i < need; i++) {
        u8 cc = s.data[at + i];
        if ((cc & 0xc0) != 0x80) return 0;
        v = (v << 6) | (cc & 0x3fu);
    }
    if (v < lo) return 0;                        // surlong
    if (v > 0x10ffff) return 0;                  // hors plage
    if (v >= 0xd800 && v <= 0xdfff) return 0;    // surrogate encodé
    *cp = v;
    return need;
}

b32 utf8_validate(Str8 s) {
    isize i = 0;
    while (i < s.len) {
        u32 cp;
        isize n = utf8_decode(s, i, &cp);
        if (n == 0) return 0;
        i += n;
    }
    return 1;
}

// ------------------------------------------------------------------ temps ---

i64 vs_ns_to_unix_ms(i64 ns) {
    i64 ms = ns / 1000000;
    if (ns < 0 && ns % 1000000 != 0) ms -= 1;
    return ms;
}

// ----------------------------------------------------------------- hasard ---

void vs_hex_encode(const u8 *bytes, isize n, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (isize i = 0; i < n; i++) {
        out[i * 2] = hex[(bytes[i] >> 4) & 0xf];
        out[i * 2 + 1] = hex[bytes[i] & 0xf];
    }
    out[n * 2] = 0;
}

f64 vs_ns_seconds(i64 dur_ns) {
    i64 sec = dur_ns / 1000000000;
    i64 nsec = dur_ns % 1000000000;
    return (f64)sec + (f64)nsec / 1e9;
}
