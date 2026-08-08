// ini_win32.c — moitié PLATEFORME des réglages (ADR-010, VS-030).
//
// Emplacement du fichier (%APPDATA%, en UTF-16) et E/S disque. Le format et
// toute la logique d'édition sont dans ini_core.c.
#include "ini.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string.h>

Str8 ini_path(Arena *a) {
    u16 wname[] = {'A', 'P', 'P', 'D', 'A', 'T', 'A', 0};
    DWORD n = GetEnvironmentVariableW((LPCWSTR)wname, NULL, 0);
    if (n == 0) return str8_lit("");
    u16 *buf = arena_push_array(a, u16, (isize)n + 1);
    DWORD got = GetEnvironmentVariableW((LPCWSTR)wname, (LPWSTR)buf, n);
    if (got == 0 || got > n) return str8_lit("");
    return str8_cat(a, utf16_to_utf8(a, buf), str8_lit("\\vibesync.ini"));
}

b32 ini_load_file(Arena *a, Str8 path, Ini *out) {
    ini_clear(out);
    if (path.len == 0) return 0;
    TempArena t = temp_begin(a);
    u16 *w = utf8_to_utf16(a, path, NULL);
    HANDLE h = CreateFileW((LPCWSTR)w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    temp_end(t);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > VS_MB(1)) {
        CloseHandle(h);
        return 0;
    }
    u8 *buf = arena_push_array(a, u8, (isize)size.QuadPart + 1);
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL);
    CloseHandle(h);
    if (!ok) return 0;
    // Un BOM UTF-8 éventuel (fichier édité au Bloc-notes) est sauté.
    Str8 text = str8(buf, (isize)got);
    if (text.len >= 3 && text.data[0] == 0xef && text.data[1] == 0xbb && text.data[2] == 0xbf) {
        text = str8_sub(text, 3, -1);
    }
    return ini_parse(a, text, out);
}

// ini_save_file écrit le fichier de réglages. Un échec est RARE mais réel chez
// l'utilisateur — disque plein (112), profil aux droits bricolés (5), antivirus
// ou éditeur qui garde le fichier ouvert (32) — et il faisait perdre réglages
// et jeton de session sans laisser de trace. Le code Win32 est journalisé ici,
// au seul endroit qui le connaisse ; la décision de prévenir l'utilisateur
// appartient à l'appelant (ini_flush_notify dans main.c).
//
// L'écriture est ATOMIQUE (VS-037) : temporaire du même répertoire, puis
// bascule par MoveFileExW — même pattern que auto_write_atomic(). Un
// CreateFileW(CREATE_ALWAYS) direct tronque le fichier AVANT d'écrire : une
// coupure au milieu emportait réglages, jeton de session et mot de passe
// chiffré d'un coup. Ici, tant que la bascule n'a pas eu lieu, l'ancien
// vibesync.ini est intact ; c'est tout l'intérêt.
b32 ini_save_file(Arena *scratch, Str8 path, Str8 content) {
    if (path.len == 0) {
        vs_log("ini: chemin de vibesync.ini inconnu (%%APPDATA%% introuvable), réglages non écrits");
        return 0;
    }
    TempArena t = temp_begin(scratch);
    // Le temporaire vit dans le MÊME répertoire que la cible : MoveFileExW
    // n'est atomique qu'à l'intérieur d'un volume (entre volumes il copie).
    // Suffixe = pid, pour que deux vibesync lancés en parallèle (ou un test qui
    // tourne pendant que l'appli tourne) n'écrivent pas dans le même fichier.
    Builder tmp;
    builder_init(&tmp, scratch, path.len + 24);
    builder_str(&tmp, path);
    builder_cstr(&tmp, ".tmp-");
    builder_i64(&tmp, (i64)GetCurrentProcessId());
    Str8 tmp_path = builder_result(&tmp);
    u16 *wtmp = utf8_to_utf16(scratch, tmp_path, NULL);
    u16 *wdst = utf8_to_utf16(scratch, path, NULL);

    HANDLE h = CreateFileW((LPCWSTR)wtmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();  // lu AVANT tout autre appel : rien ne doit s'intercaler
        vs_log("ini: création du fichier temporaire refusée (erreur Win32 %lu) — \"%.*s\"",
               (unsigned long)err, (int)tmp_path.len, (const char *)tmp_path.data);
        temp_end(t);
        return 0;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(h, content.data, (DWORD)content.len, &written, NULL);
    DWORD err = GetLastError();
    if (!ok || written != (DWORD)content.len) {
        // Écriture partielle (disque plein) : elle ne concerne QUE le
        // temporaire, l'ancien fichier n'a pas été touché. On jette le déchet.
        CloseHandle(h);
        DeleteFileW((LPCWSTR)wtmp);
        vs_log("ini: écriture incomplète du temporaire (%lu/%lld octets, erreur Win32 %lu) — "
               "\"%.*s\" laissé intact",
               (unsigned long)written, (long long)content.len, (unsigned long)err, (int)path.len,
               (const char *)path.data);
        temp_end(t);
        return 0;
    }
    // FlushFileBuffers avant la bascule. Coût : un aller-retour disque (~ms) à
    // chaque geste de l'utilisateur — négligeable pour un fichier de 2 Ko écrit
    // à la connexion ou au changement de dossier, jamais en boucle. Gain : sans
    // lui, le renommage peut être validé par le système de fichiers alors que
    // le CONTENU du temporaire est encore en cache ; une coupure de courant
    // laisse alors un vibesync.ini de zéros, exactement le désastre que ce
    // ticket supprime. Un échec ici est traité comme un échec d'écriture (le
    // disque plein se manifeste souvent à ce moment-là) : on ne bascule pas.
    if (!FlushFileBuffers(h)) {
        err = GetLastError();
        CloseHandle(h);
        DeleteFileW((LPCWSTR)wtmp);
        vs_log("ini: vidage sur disque du temporaire refusé (erreur Win32 %lu) — \"%.*s\" laissé intact",
               (unsigned long)err, (int)path.len, (const char *)path.data);
        temp_end(t);
        return 0;
    }
    CloseHandle(h);

    // La bascule échoue si quelqu'un tient l'ancien fichier ouvert au même
    // instant — antivirus qui l'analyse, Bloc-notes resté ouvert (Windows ne
    // remplace pas un fichier ouvert sans FILE_SHARE_DELETE). Ce n'est pas
    // forcément une erreur, c'est souvent une course : on réessaie brièvement,
    // comme auto_write_atomic(), plutôt que de perdre l'écriture.
    b32 moved = 0;
    DWORD move_err = 0;
    for (int attempt = 0; attempt < 5; attempt++) {
        if (MoveFileExW((LPCWSTR)wtmp, (LPCWSTR)wdst, MOVEFILE_REPLACE_EXISTING)) {
            moved = 1;
            break;
        }
        move_err = GetLastError();
        if (attempt < 4) Sleep(20);
    }
    if (!moved) {
        // Le temporaire orphelin ne doit pas s'accumuler dans %APPDATA%.
        DeleteFileW((LPCWSTR)wtmp);
        vs_log("ini: bascule du temporaire refusée (erreur Win32 %lu) — \"%.*s\" conservé intact",
               (unsigned long)move_err, (int)path.len, (const char *)path.data);
    }
    temp_end(t);
    return moved;
}
