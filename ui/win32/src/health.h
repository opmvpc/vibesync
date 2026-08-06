// health.h — sonde de joignabilité du serveur : un GET /healthz en WinHTTP.
//
// Bloquant (DNS + TCP + TLS) : appelé depuis un thread dédié, jamais depuis le
// thread UI. La raison d'un échec est traduite en français, parce qu'un
// « Injoignable » sans motif ne dit pas quoi corriger.
#ifndef VS_HEALTH_H
#define VS_HEALTH_H

#include "base.h"

typedef enum {
    HEALTH_UNKNOWN = 0,
    HEALTH_OK,        // 200 sur /healthz
    HEALTH_HTTP,      // a répondu, mais pas 200 (proxy, mauvais chemin…)
    HEALTH_DNS,       // nom introuvable
    HEALTH_REFUSED,   // port fermé / injoignable
    HEALTH_TLS,       // échec TLS (certificat, version)
    HEALTH_TIMEOUT,
    HEALTH_OTHER,
} HealthKind;

typedef struct {
    HealthKind kind;
    i64 latency_ms;
    int status;          // code HTTP si la réponse est arrivée
    // Le serveur ne répond pas en clair mais répond en TLS : l'utilisateur a
    // tapé ws:// alors qu'il faut wss://. C'est LE piège rencontré sur le
    // terrain, on le détecte au lieu de laisser tourner le réessai.
    b32 tls_available;
    char detail[128];    // motif court, déjà en français
} HealthResult;

// health_probe interroge http(s)://host:port/healthz. `timeout_ms` couvre
// chaque étape (résolution, connexion, envoi, réception).
void health_probe(Arena *scratch, Str8 host, int port, b32 secure, i64 timeout_ms, HealthResult *out);

// health_text rend le résultat affichable (« en ligne (113 ms) », « nom
// introuvable (DNS) »…).
const char *health_text(const HealthResult *r);

#endif // VS_HEALTH_H
