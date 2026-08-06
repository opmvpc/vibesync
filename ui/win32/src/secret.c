#include "secret.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>

#include <string.h>

// Entropie applicative : elle ne remplace pas la clé de l'utilisateur (DPAPI
// s'en charge), elle cloisonne nos blobs de ceux d'une autre application qui
// utiliserait DPAPI sur le même compte. Constante, versionnée pour pouvoir
// changer de format un jour sans confusion.
static const char VS_ENTROPY[] = "vibesync.v1";

void secret_wipe(void *p, isize n) {
    if (p && n > 0) SecureZeroMemory(p, (SIZE_T)n);
}

static void entropy_blob(DATA_BLOB *b) {
    b->pbData = (BYTE *)VS_ENTROPY;
    b->cbData = (DWORD)(sizeof(VS_ENTROPY) - 1);
}

b32 secret_protect(Arena *a, Str8 plain, Str8 *out_hex) {
    if (plain.len <= 0) return 0;
    DATA_BLOB in, ent, out;
    in.pbData = plain.data;
    in.cbData = (DWORD)plain.len;
    entropy_blob(&ent);
    memset(&out, 0, sizeof(out));
    // CRYPTPROTECT_UI_FORBIDDEN : jamais d'invite, on est peut-être hors session
    // interactive (et une invite surprise serait une mauvaise UX).
    if (!CryptProtectData(&in, L"vibesync", &ent, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) return 0;

    char *hex = arena_push_array(a, char, (isize)out.cbData * 2 + 1);
    vs_hex_encode(out.pbData, (isize)out.cbData, hex);
    // Le blob chiffré n'est pas un secret, mais rien ne coûte de le rendre.
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    *out_hex = str8((u8 *)hex, (isize)out.cbData * 2);
    return 1;
}

static int hex_digit(u8 c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

b32 secret_hex_decode(Arena *a, Str8 hex, u8 **out, isize *out_len) {
    if (hex.len <= 0 || (hex.len & 1)) return 0;
    isize n = hex.len / 2;
    u8 *buf = arena_push_array(a, u8, n);
    for (isize i = 0; i < n; i++) {
        int hi = hex_digit(hex.data[i * 2]), lo = hex_digit(hex.data[i * 2 + 1]);
        if (hi < 0 || lo < 0) return 0;
        buf[i] = (u8)((hi << 4) | lo);
    }
    *out = buf;
    *out_len = n;
    return 1;
}

b32 secret_unprotect(Arena *a, Str8 hex, Str8 *out_plain) {
    u8 *raw = NULL;
    isize raw_len = 0;
    if (!secret_hex_decode(a, hex, &raw, &raw_len)) return 0;

    DATA_BLOB in, ent, out;
    in.pbData = raw;
    in.cbData = (DWORD)raw_len;
    entropy_blob(&ent);
    memset(&out, 0, sizeof(out));
    // Échec = blob d'un autre compte, d'une autre machine, ou corrompu. On ne
    // distingue pas : dans tous les cas « aucun mot de passe mémorisé ».
    if (!CryptUnprotectData(&in, NULL, &ent, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) return 0;

    u8 *copy = arena_push_array(a, u8, (isize)out.cbData + 1);
    if (out.cbData > 0) memcpy(copy, out.pbData, out.cbData);
    copy[out.cbData] = 0;
    // Le tampon rendu par DPAPI contient le clair : on l'efface avant de le
    // libérer, sinon il traîne dans le tas jusqu'à réutilisation.
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    *out_plain = str8(copy, (isize)out.cbData);
    return 1;
}
