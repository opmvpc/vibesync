// base_win32.c — moitié PLATEFORME du socle (ADR-010, VS-030).
//
// Tout ce que base.h déclare sous « frontière plateforme » : sortie d'erreur,
// journal %APPDATA%, arènes (VirtualAlloc), horloge, aléa, et les conversions
// UTF-8 ↔ UTF-16 — qui ne doivent JAMAIS remonter dans le code commun.
#include "base.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include <stdarg.h>
#include <stdio.h>
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

// ----------------------------------------------------------------- journal ---

// Toutes les écritures du journal passent par ce verrou : au moins deux threads
// appellent vs_log (le thread UI et le thread VLC), et « lire la taille puis
// décider de tronquer ou d'ajouter » doit être indivisible, sans quoi deux
// rotations concurrentes peuvent se perdre mutuellement une ligne. Un SRWLOCK
// statique vaut SRWLOCK_INIT à zéro : aucune initialisation à ordonnancer,
// contrairement à une CRITICAL_SECTION.
static SRWLOCK g_log_lock;

// log_path écrit %APPDATA%\vibesync.log dans `out` (UTF-16 terminé par 0).
// 0 si %APPDATA% n'existe pas : le journal reste alors purement console.
static b32 log_path(u16 *out, isize cap) {
    DWORD n = GetEnvironmentVariableW(L"APPDATA", (LPWSTR)out, (DWORD)cap);
    if (n == 0 || (isize)n >= cap) return 0;
    static const u16 name[] = {'\\', 'v', 'i', 'b', 'e', 's', 'y', 'n', 'c', '.', 'l', 'o', 'g', 0};
    isize k = (isize)n;
    for (isize i = 0; name[i]; i++) {
        if (k + 1 >= cap) return 0;
        out[k++] = name[i];
    }
    out[k] = 0;
    return 1;
}

void vs_log(const char *fmt, ...) {
    char line[1024];
    SYSTEMTIME t;
    GetSystemTime(&t);
    int n = snprintf(line, sizeof(line), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ vibesync[%lu] ", t.wYear,
                     t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                     (unsigned long)GetCurrentProcessId());
    if (n < 0 || n >= (int)sizeof(line)) n = 0;
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    isize len = (isize)n + (isize)m;
    if (len > (isize)sizeof(line) - 3) len = (isize)sizeof(line) - 3;
    line[len++] = '\r';
    line[len++] = '\n';
    line[len] = 0;

    OutputDebugStringA(line);
    vs_write_stderr(str8((u8 *)line, len));

    u16 path[MAX_PATH + 32];
    if (!log_path(path, VS_ARRAY_COUNT(path))) return;

    // Tout le cycle « ouvrir, mesurer, tourner, écrire » est sous verrou : la
    // décision de rotation dépend de la taille lue juste avant.
    AcquireSRWLockExclusive(&g_log_lock);
    // FILE_APPEND_DATA : l'écriture se fait toujours en fin de fichier, même si
    // une autre instance du client écrit en même temps. FILE_READ_ATTRIBUTES
    // est exigé par GetFileSizeEx — sans lui, la taille n'est pas fiable.
    HANDLE h = CreateFileW((LPCWSTR)path, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           NULL);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size;
        // Plafond STRICT : on tourne AVANT d'écrire la ligne qui ferait
        // déborder, pas après avoir dépassé.
        if (GetFileSizeEx(h, &size) && size.QuadPart + (i64)len > VS_LOG_MAX) {
            CloseHandle(h);
            h = CreateFileW((LPCWSTR)path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        }
    }
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, line, (DWORD)len, &written, NULL);
        CloseHandle(h);
    }
    ReleaseSRWLockExclusive(&g_log_lock);
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

// ---------------------------------------------------------------- unicode ---

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

// ----------------------------------------------------------------- hasard ---

b32 vs_random_bytes(u8 *out, isize n) {
    if (n <= 0) return 1;
    NTSTATUS st = BCryptGenRandom(NULL, (PUCHAR)out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return st >= 0;
}
