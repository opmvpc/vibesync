// conn.h — tout ce qui décide *comment* on se connecte, sans rien connaître du
// réseau : normalisation de l'adresse tapée par un humain, et politique de
// reconnexion (panne réseau → réessai, refus du serveur → arrêt net).
//
// Ces deux morceaux sont de purs calculs : main.c les pilote, test_main.c les
// exerce. C'est ce qui permet de figer par des tests la règle « un mauvais mot
// de passe ne relance JAMAIS de tentative ».
#ifndef VS_CONN_H
#define VS_CONN_H

#include "base.h"

// --- normalisation d'adresse (portage de internal/webui/address.go) ---
//
// « vibesync.exemple.fr » → « wss://vibesync.exemple.fr/ws »
// « localhost:8080 »      → « ws://localhost:8080/ws »   (local = pas de TLS)
// http→ws, https→wss ; chemin absent → /ws ; fragment et userinfo retirés.
// Renvoie 0 et remplit *err (message français prêt à afficher) si l'adresse
// est inutilisable. `err` peut être NULL.
b32 conn_normalize_url(Arena *a, Str8 raw, Str8 *out, const char **err);

// conn_is_local dit si un hôte désigne la machine locale (pas de TLS attendu).
b32 conn_is_local_host(Str8 host);

// --- politique de connexion ---

typedef enum {
    CONN_IDLE = 0,    // formulaire éditable, rien en cours
    CONN_TRYING,      // tentative en cours
    CONN_WAITING,     // panne réseau : attente avant le prochain essai
    CONN_OPEN,        // session établie
    CONN_REFUSED,     // le serveur a dit non : ARRÊT NET, on n'insiste pas
} ConnPhase;

typedef struct {
    ConnPhase phase;
    i64 backoff_ns;
    i64 next_attempt_ns;
    i32 attempts;  // tentatives depuis le dernier conn_start (diagnostic)
} Conn;

void conn_reset(Conn *c);
// conn_start : l'utilisateur a cliqué « Se connecter ». Remet le backoff à zéro.
void conn_start(Conn *c, i64 now_ns);
// conn_on_open : welcome reçu.
void conn_on_open(Conn *c);
// conn_on_socket_down : la socket est tombée (panne, DNS, TLS, coupure).
// Programme un réessai — SAUF si le serveur nous a déjà refusés ou si
// l'utilisateur a annulé : c'est là que se jouait la boucle « Nouvelle
// tentative… » après un mot de passe erroné.
void conn_on_socket_down(Conn *c, i64 now_ns);
// conn_on_refused : erreur fatale du protocole. Aucune reconnexion, jamais.
void conn_on_refused(Conn *c);
// conn_cancel : l'utilisateur reprend la main pendant une tentative.
void conn_cancel(Conn *c);

// conn_should_attempt dit si le moment est venu d'ouvrir une socket.
b32 conn_should_attempt(const Conn *c, i64 now_ns);
// conn_attempt_started note qu'une socket vient d'être ouverte.
void conn_attempt_started(Conn *c);
// conn_is_busy : vrai tant que l'utilisateur ne peut pas relancer lui-même
// (bouton « Connexion… » + bouton Annuler affichés).
b32 conn_is_busy(const Conn *c);
// conn_seconds_until_retry arrondit à la seconde supérieure, pour l'affichage.
i64 conn_seconds_until_retry(const Conn *c, i64 now_ns);

#endif // VS_CONN_H
