#include "health.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <stdio.h>
#include <string.h>

// probe_once fait un aller-retour et renvoie le code d'erreur Win32 (0 = ok).
static DWORD probe_once(Arena *scratch, Str8 host, int port, b32 secure, i64 timeout_ms, int *status) {
    *status = 0;
    TempArena t = temp_begin(scratch);
    u16 *whost = utf8_to_utf16(scratch, host, NULL);
    DWORD err = 0;
    HINTERNET session = NULL, connect = NULL, request = NULL;

    session = WinHttpOpen(L"vibesync", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        err = GetLastError();
        goto done;
    }
    int ms = (int)(timeout_ms > 0 ? timeout_ms : 4000);
    WinHttpSetTimeouts(session, ms, ms, ms, ms);
    connect = WinHttpConnect(session, (LPCWSTR)whost, (INTERNET_PORT)port, 0);
    if (!connect) {
        err = GetLastError();
        goto done;
    }
    request = WinHttpOpenRequest(connect, L"GET", L"/healthz", NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        err = GetLastError();
        goto done;
    }
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL)) {
        err = GetLastError();
        goto done;
    }
    DWORD code = 0, size = sizeof(code);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &code, &size,
                            NULL)) {
        *status = (int)code;
    }

done:
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    temp_end(t);
    return err;
}

// classify traduit un code WinHTTP en motif utilisable par un humain.
static HealthKind classify(DWORD err, const char **detail) {
    switch (err) {
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
            *detail = "nom introuvable (DNS)";
            return HEALTH_DNS;
        case ERROR_WINHTTP_CANNOT_CONNECT:
            *detail = "connexion refusée";
            return HEALTH_REFUSED;
        case ERROR_WINHTTP_TIMEOUT:
            *detail = "délai dépassé";
            return HEALTH_TIMEOUT;
        case ERROR_WINHTTP_SECURE_FAILURE:
            *detail = "certificat TLS refusé";
            return HEALTH_TLS;
        case ERROR_WINHTTP_CONNECTION_ERROR:
            *detail = "connexion interrompue";
            return HEALTH_REFUSED;
        case ERROR_WINHTTP_INVALID_URL:
        case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
            *detail = "adresse invalide";
            return HEALTH_OTHER;
        default: break;
    }
    *detail = NULL;
    return HEALTH_OTHER;
}

void health_probe(Arena *scratch, Str8 host, int port, b32 secure, i64 timeout_ms, HealthResult *out) {
    memset(out, 0, sizeof(*out));
    if (host.len == 0) {
        out->kind = HEALTH_OTHER;
        snprintf(out->detail, sizeof(out->detail), "adresse vide");
        return;
    }
    i64 start = (i64)GetTickCount64();
    int status = 0;
    DWORD err = probe_once(scratch, host, port, secure, timeout_ms, &status);
    out->latency_ms = (i64)GetTickCount64() - start;
    out->status = status;

    if (err == 0) {
        out->kind = (status == 200) ? HEALTH_OK : HEALTH_HTTP;
        if (out->kind == HEALTH_HTTP) snprintf(out->detail, sizeof(out->detail), "réponse HTTP %d", status);
        return;
    }
    const char *detail = NULL;
    out->kind = classify(err, &detail);
    if (detail) snprintf(out->detail, sizeof(out->detail), "%s", detail);
    else snprintf(out->detail, sizeof(out->detail), "erreur réseau %lu", (unsigned long)err);

    // Échec en clair : le même hôte répond-il en TLS ? C'est le cas « j'ai tapé
    // ws:// alors que le serveur est derrière Traefik ».
    if (!secure && out->kind != HEALTH_DNS) {
        int tls_status = 0;
        int tls_port = (port == 80 || port == 0) ? 443 : port;
        if (probe_once(scratch, host, tls_port, 1, timeout_ms, &tls_status) == 0) {
            out->tls_available = 1;
        }
    }
}

const char *health_text(const HealthResult *r) {
    switch (r->kind) {
        case HEALTH_OK: return "en ligne";
        case HEALTH_HTTP: return r->detail;
        case HEALTH_DNS: return "nom introuvable (DNS)";
        case HEALTH_REFUSED: return "injoignable";
        case HEALTH_TLS: return "TLS refusé";
        case HEALTH_TIMEOUT: return "délai dépassé";
        case HEALTH_UNKNOWN: return "non testé";
        case HEALTH_OTHER:
        default: return r->detail[0] ? r->detail : "erreur réseau";
    }
}
