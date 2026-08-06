// ConnPolicy.swift — frontière Swift ↔ core/src/conn.c (ADR-010, phase 4).
//
// Deux morceaux que le client macOS n'avait JAMAIS eus, et que le client
// Windows utilise depuis toujours :
//
//   1. La normalisation de l'adresse tapée par un humain. « vibesync.exemple.fr »
//      devient « wss://vibesync.exemple.fr/ws » ; « localhost:8080 » devient
//      « ws://localhost:8080/ws » (pas de TLS en local) ; http→ws, https→wss ;
//      chemin absent → /ws ; fragment et userinfo retirés. Avant, macOS exigeait
//      une URL complète et répondait « Adresse invalide » à un hôte nu.
//   2. La politique de réessai. La règle non négociable — « un refus du serveur
//      ne relance JAMAIS de tentative, une panne réseau si » — est désormais
//      tenue par le même code des deux côtés, avec la même courbe de backoff
//      (engine_next_backoff, 1 s → 10 s).
//
// Rien n'est décidé ici : `Conn` est le struct du C, manipulé par ses propres
// fonctions.

import Foundation
import VSCore

/// Politique de connexion : quand ouvrir une socket, quand renoncer.
public struct ConnPolicy {

    private var conn = Conn()

    public init() {
        conn_reset(&conn)
    }

    /// L'utilisateur a cliqué « Se connecter » : le backoff repart de zéro.
    public mutating func start(_ now: Nanos) {
        conn_start(&conn, now)
    }

    /// Welcome reçu.
    public mutating func opened() {
        conn_on_open(&conn)
    }

    /// La socket est tombée (panne, DNS, TLS, coupure) : un réessai est
    /// programmé — sauf si le serveur nous a déjà refusés ou si l'utilisateur a
    /// repris la main.
    public mutating func socketDown(_ now: Nanos) {
        conn_on_socket_down(&conn, now)
    }

    /// Erreur fatale du protocole (mauvais mot de passe, pseudo pris, version) :
    /// aucune reconnexion, jamais.
    public mutating func refused() {
        conn_on_refused(&conn)
    }

    /// L'utilisateur reprend la main (quitter la salle, annuler).
    public mutating func cancel() {
        conn_cancel(&conn)
    }

    /// Le moment est-il venu d'ouvrir une socket ?
    public func shouldAttempt(_ now: Nanos) -> Bool {
        var copy = conn
        return conn_should_attempt(&copy, now) != 0
    }

    public mutating func attemptStarted() {
        conn_attempt_started(&conn)
    }

    /// Secondes restantes avant le prochain essai, arrondies au supérieur.
    public func secondsUntilRetry(_ now: Nanos) -> Int64 {
        var copy = conn
        return conn_seconds_until_retry(&copy, now)
    }
}

/// Normalisation de l'adresse du serveur (conn_normalize_url).
public enum ServerAddress {

    /// Rend l'URL normalisée, ou le message d'erreur français prêt à afficher.
    public static func normalize(_ raw: String) -> (url: URL?, error: String) {
        let normalized: (text: String, error: String) = Scratch.shared.use { arena in
            withStr8(raw) { input -> (String, String) in
                var out = Str8()
                var err: UnsafePointer<CChar>?
                if conn_normalize_url(arena, input, &out, &err) == 0 {
                    return ("", err.map { String(cString: $0) } ?? "Adresse de serveur invalide.")
                }
                return (coreString(out), "")
            }
        }
        if !normalized.error.isEmpty {
            return (nil, normalized.error)
        }
        // Ceinture : le C garantit la forme, URLSession exige un URL analysable
        // (un nom d'hôte avec des caractères qu'il refuse, par exemple).
        guard let url = URL(string: normalized.text), let host = url.host, !host.isEmpty else {
            return (nil, "Adresse de serveur invalide.")
        }
        return (url, "")
    }
}
