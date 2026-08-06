// base_posix.c — moitié PLATEFORME du socle pour macOS/POSIX (ADR-010, VS-031).
//
// Pendant exact de base_win32.c : tout ce que base.h déclare sous « frontière
// plateforme », implémenté avec la seule libc du système (aucune dépendance,
// ADR-008) :
//
//   vs_write_stderr / vs_fatal   write(2), _exit(3)
//   vs_log                       ~/Library/Logs/vibesync.log, rotation à 1 Mo
//   arènes                       mmap PROT_NONE (réserve) + mprotect (engagement)
//   vs_now_ns                    clock_gettime(CLOCK_REALTIME)
//   vs_random_bytes              arc4random_buf / getentropy
//
// Ce qui n'y est VOLONTAIREMENT pas : utf8_to_utf16 / utf16_to_utf8. Ces deux
// prototypes de base.h sont marqués « frontière plateforme » précisément parce
// que la couche commune ne parle qu'UTF-8 (risque n°1 d'ADR-010) : côté macOS,
// rien — ni le C portable, ni Swift — n'a de raison d'appeler de l'UTF-16. Les
// implémenter ici ne ferait que dupliquer du code mort.
#include "base.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// ------------------------------------------------------------------ fatal ---

void vs_write_stderr(Str8 s) {
    isize off = 0;
    while (off < s.len) {
        ssize_t n = write(2, s.data + off, (size_t)(s.len - off));
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return;
        }
        off += (isize)n;
    }
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
    // _exit et non exit : vs_fatal est appelable depuis n'importe quel thread,
    // y compris avec une arène à moitié écrite ; dérouler les atexit d'un
    // processus déjà incohérent n'apporte rien.
    _exit(3);
}

// ----------------------------------------------------------------- journal ---

// Même contrat que sous Windows : « lire la taille puis décider de tronquer ou
// d'ajouter » doit être indivisible, plusieurs threads journalisent.
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

// log_path écrit le chemin du journal, terminé par 0. 0 si $HOME est absent :
// le journal reste alors purement console.
static b32 log_path(char *out, isize cap) {
    const char *home = getenv("HOME");
    if (!home || !*home) return 0;
#if defined(__APPLE__)
    // Emplacement attendu par la Console d'Apple ; le dossier existe toujours,
    // on le crée quand même (compte fraîchement provisionné, sandbox de test).
    int n = snprintf(out, (size_t)cap, "%s/Library/Logs", home);
    if (n < 0 || n >= (int)cap) return 0;
    mkdir(out, 0755);
    n = snprintf(out, (size_t)cap, "%s/Library/Logs/vibesync.log", home);
#else
    int n = snprintf(out, (size_t)cap, "%s/.vibesync.log", home);
#endif
    return n > 0 && n < (int)cap;
}

void vs_log(const char *fmt, ...) {
    char line[1024];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm utc;
    time_t secs = (time_t)ts.tv_sec;
    gmtime_r(&secs, &utc);
    int n = snprintf(line, sizeof(line), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ vibesync[%ld] ",
                     utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
                     (int)(ts.tv_nsec / 1000000), (long)getpid());
    if (n < 0 || n >= (int)sizeof(line)) n = 0;
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    isize len = (isize)n + (isize)m;
    if (len > (isize)sizeof(line) - 2) len = (isize)sizeof(line) - 2;
    // Fin de ligne Unix : le journal se lit avec tail(1), pas avec Bloc-notes.
    line[len++] = '\n';
    line[len] = 0;

    vs_write_stderr(str8((u8 *)line, len));

    char path[1024];
    if (!log_path(path, (isize)sizeof(path))) return;

    pthread_mutex_lock(&g_log_lock);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        struct stat st;
        // Plafond STRICT : on tourne AVANT d'écrire la ligne qui ferait
        // déborder, pas après avoir dépassé.
        if (fstat(fd, &st) == 0 && (i64)st.st_size + (i64)len > VS_LOG_MAX) {
            close(fd);
            fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        }
    }
    if (fd >= 0) {
        isize off = 0;
        while (off < len) {
            ssize_t w = write(fd, line + off, (size_t)(len - off));
            if (w <= 0) {
                if (w < 0 && errno == EINTR) continue;
                break;
            }
            off += (isize)w;
        }
        close(fd);
    }
    pthread_mutex_unlock(&g_log_lock);
}

// ----------------------------------------------------------------- arènes ---
//
// Même modèle qu'avec VirtualAlloc : une RÉSERVE d'espace d'adressage (mmap
// PROT_NONE, aucune page réellement engagée) puis des ENGAGEMENTS par tranches
// de 64 Ko (mprotect en lecture/écriture). Les pointeurs restent donc stables,
// et un dépassement de la réserve tombe sur une page PROT_NONE plutôt que sur
// de la mémoire d'autrui.

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

// page_size : mprotect exige des bornes alignées sur la page (16 Ko sur
// Apple Silicon, 4 Ko ailleurs), là où MEM_COMMIT arrondissait pour nous.
static isize page_size(void) {
    static isize cached;
    if (cached == 0) {
        long p = sysconf(_SC_PAGESIZE);
        cached = p > 0 ? (isize)p : 4096;
    }
    return cached;
}

Arena *arena_create(isize reserve_bytes) {
    if (reserve_bytes < VS_KB(64)) reserve_bytes = VS_KB(64);
    isize header = align_up((isize)sizeof(Arena), 64);
    isize total = align_up(reserve_bytes + header, VS_KB(64));
    void *mem = mmap(NULL, (size_t)total, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED) return NULL;
    // Engager la première tranche pour y loger l'en-tête.
    if (mprotect(mem, (size_t)ARENA_COMMIT_CHUNK, PROT_READ | PROT_WRITE) != 0) {
        munmap(mem, (size_t)total);
        return NULL;
    }
    Arena *a = (Arena *)mem;
    a->base = (u8 *)mem + header;
    a->cap = total - header;
    a->commit = ARENA_COMMIT_CHUNK - header;
    a->used = 0;
    VS_POISON(a->base, a->commit);
    return a;
}

void arena_destroy(Arena *a) {
    if (!a) return;
    isize header = align_up((isize)sizeof(Arena), 64);
    isize total = a->cap + header;
    VS_UNPOISON(a, total);
    munmap(a, (size_t)total);
}

void *arena_push(Arena *a, isize size, isize align) {
    VS_ASSERT(a != NULL);
    VS_ASSERT(size >= 0);
    isize start = align_up(a->used, align);
    isize end = start + size;
    if (end > a->cap) {
        vs_fatal(__FILE__, __LINE__, "arène pleine (%lld demandés, %lld libres)", (long long)size,
                 (long long)(a->cap - start));
    }
    if (end > a->commit) {
        isize want = align_up(end, ARENA_COMMIT_CHUNK);
        if (want > a->cap) want = a->cap;
        u8 *from = a->base + a->commit;
        u8 *to = a->base + want;
        isize page = page_size();
        uintptr_t lo = (uintptr_t)from & ~(uintptr_t)(page - 1);
        uintptr_t hi = ((uintptr_t)to + (uintptr_t)page - 1) & ~(uintptr_t)(page - 1);
        if (mprotect((void *)lo, (size_t)(hi - lo), PROT_READ | PROT_WRITE) != 0) {
            vs_fatal(__FILE__, __LINE__, "engagement mémoire refusé (%lld octets)", (long long)(hi - lo));
        }
        VS_POISON(from, want - a->commit);
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

// ------------------------------------------------------------------ temps ---

i64 vs_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (i64)ts.tv_sec * 1000000000LL + (i64)ts.tv_nsec;
}

// ----------------------------------------------------------------- hasard ---

b32 vs_random_bytes(u8 *out, isize n) {
    if (n <= 0) return 1;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    // arc4random_buf ne peut pas échouer et ne consomme pas de descripteur.
    arc4random_buf(out, (size_t)n);
    return 1;
#else
    // getentropy plafonne à 256 octets par appel.
    isize off = 0;
    while (off < n) {
        size_t chunk = (size_t)(n - off);
        if (chunk > 256) chunk = 256;
        if (getentropy(out + off, chunk) != 0) return 0;
        off += (isize)chunk;
    }
    return 1;
#endif
}
