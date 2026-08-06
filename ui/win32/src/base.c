#include "base.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------ fatal ---

void vs_write_stderr(Str8 s) {
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h == NULL || h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, s.data, (DWORD)s.len, &written, NULL);
}

void vs_fatal(const char *file, int line, const char *fmt, ...) {
    char msg[1024];
    int n = snprintf(msg, sizeof(msg), "vibesync: %s:%d: ", file, line);
    if (n < 0) n = 0;
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(msg + n, sizeof(msg) - (size_t)n, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    isize len = (isize)n + (isize)m;
    if (len > (isize)sizeof(msg) - 2) len = (isize)sizeof(msg) - 2;
    msg[len++] = '\n';
    vs_write_stderr(str8((u8 *)msg, len));
    ExitProcess(3);
}

// ----------------------------------------------------------------- arènes ---

// Sous AddressSanitizer, l'arène marque elle-même sa mémoire : ASan ne voit
// pas VirtualAlloc, donc sans ça un dépassement d'une allocation d'arène sur
// la suivante passerait inaperçu. Les octets non alloués (padding
// d'alignement et zone libérée par temp_end) sont empoisonnés.
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define VS_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define VS_ASAN 1
#endif

#ifdef VS_ASAN
void __asan_poison_memory_region(void const volatile *addr, size_t size);
void __asan_unpoison_memory_region(void const volatile *addr, size_t size);
#define VS_POISON(p, n) __asan_poison_memory_region((p), (size_t)(n))
#define VS_UNPOISON(p, n) __asan_unpoison_memory_region((p), (size_t)(n))
#else
#define VS_POISON(p, n) ((void)(p), (void)(n))
#define VS_UNPOISON(p, n) ((void)(p), (void)(n))
#endif

#define ARENA_COMMIT_CHUNK VS_KB(64)

struct Arena {
    u8 *base;      // début de la zone utilisable (après l'en-tête)
    isize cap;     // octets réservés utilisables
    isize commit;  // octets déjà engagés
    isize used;
};

static isize align_up(isize v, isize align) {
    VS_ASSERT(align > 0 && (align & (align - 1)) == 0);
    return (v + align - 1) & ~(align - 1);
}

Arena *arena_create(isize reserve_bytes) {
    if (reserve_bytes < VS_KB(64)) reserve_bytes = VS_KB(64);
    isize header = align_up((isize)sizeof(Arena), 64);
    isize total = align_up(reserve_bytes + header, VS_KB(64));
    u8 *mem = (u8 *)VirtualAlloc(NULL, (SIZE_T)total, MEM_RESERVE, PAGE_READWRITE);
    if (!mem) return NULL;
    // Engager la première page pour y loger l'en-tête.
    if (!VirtualAlloc(mem, (SIZE_T)ARENA_COMMIT_CHUNK, MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(mem, 0, MEM_RELEASE);
        return NULL;
    }
    Arena *a = (Arena *)mem;
    a->base = mem + header;
    a->cap = total - header;
    a->commit = ARENA_COMMIT_CHUNK - header;
    a->used = 0;
    VS_POISON(a->base, a->commit);
    return a;
}

void arena_destroy(Arena *a) {
    if (!a) return;
    VirtualFree(a, 0, MEM_RELEASE);
}

void *arena_push(Arena *a, isize size, isize align) {
    VS_ASSERT(a != NULL);
    VS_ASSERT(size >= 0);
    isize start = align_up(a->used, align);
    isize end = start + size;
    if (end > a->cap) {
        vs_fatal(__FILE__, __LINE__, "arène pleine (%lld demandés, %lld libres)",
                 (long long)size, (long long)(a->cap - start));
    }
    if (end > a->commit) {
        isize want = align_up(end, ARENA_COMMIT_CHUNK);
        if (want > a->cap) want = a->cap;
        u8 *from = a->base + a->commit;
        SIZE_T bytes = (SIZE_T)(want - a->commit);
        if (!VirtualAlloc(from, bytes, MEM_COMMIT, PAGE_READWRITE)) {
            vs_fatal(__FILE__, __LINE__, "engagement mémoire refusé (%lld octets)", (long long)bytes);
        }
        VS_POISON(from, bytes);
        a->commit = want;
    }
    u8 *p = a->base + start;
    VS_UNPOISON(p, size);
    memset(p, 0, (size_t)size);
    a->used = end;
    return p;
}

isize arena_pos(const Arena *a) { return a->used; }

void arena_pop_to(Arena *a, isize pos) {
    VS_ASSERT(pos >= 0 && pos <= a->used);
    VS_POISON(a->base + pos, a->commit - pos);
    a->used = pos;
}

void arena_reset(Arena *a) { arena_pop_to(a, 0); }

isize arena_capacity(const Arena *a) { return a->cap; }

TempArena temp_begin(Arena *a) {
    TempArena t = {a, a->used};
    return t;
}

void temp_end(TempArena t) { arena_pop_to(t.arena, t.pos); }

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
// 0 si la séquence est invalide (`*cp` reçoit alors U+FFFD).
static isize utf8_decode(Str8 s, isize at, u32 *cp) {
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

u16 *utf8_to_utf16(Arena *a, Str8 s, isize *out_len) {
    // Majorant : chaque octet donne au plus une unité UTF-16 (et un point de
    // code hors BMP consomme 4 octets pour 2 unités).
    u16 *out = arena_push_array(a, u16, s.len + 1);
    isize n = 0;
    isize i = 0;
    while (i < s.len) {
        u32 cp;
        isize used = utf8_decode(s, i, &cp);
        if (used == 0) {
            cp = 0xfffd;
            used = 1;
        }
        i += used;
        if (cp < 0x10000) {
            out[n++] = (u16)cp;
        } else {
            cp -= 0x10000;
            out[n++] = (u16)(0xd800 + (cp >> 10));
            out[n++] = (u16)(0xdc00 + (cp & 0x3ff));
        }
    }
    out[n] = 0;
    if (out_len) *out_len = n;
    return out;
}

Str8 utf16_to_utf8(Arena *a, const u16 *w) {
    isize wlen = 0;
    while (w[wlen]) wlen++;
    u8 *out = arena_push_array(a, u8, wlen * 3 + 4);
    isize n = 0;
    for (isize i = 0; i < wlen; i++) {
        u32 cp = w[i];
        if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < wlen && w[i + 1] >= 0xdc00 && w[i + 1] <= 0xdfff) {
            cp = 0x10000 + ((cp - 0xd800) << 10) + (w[i + 1] - 0xdc00);
            i++;
        } else if (cp >= 0xd800 && cp <= 0xdfff) {
            cp = 0xfffd;
        }
        n += utf8_encode(cp, out + n);
    }
    out[n] = 0;
    Str8 s = {out, n};
    return s;
}

// ------------------------------------------------------------------ temps ---

i64 vs_now_ns(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    u64 ticks = ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;  // 100 ns depuis 1601
    const u64 epoch_delta = 116444736000000000ULL;
    return (i64)(ticks - epoch_delta) * 100;
}

i64 vs_ns_to_unix_ms(i64 ns) {
    i64 ms = ns / 1000000;
    if (ns < 0 && ns % 1000000 != 0) ms -= 1;
    return ms;
}

// ----------------------------------------------------------------- hasard ---

b32 vs_random_bytes(u8 *out, isize n) {
    if (n <= 0) return 1;
    NTSTATUS st = BCryptGenRandom(NULL, (PUCHAR)out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return st >= 0;
}

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
