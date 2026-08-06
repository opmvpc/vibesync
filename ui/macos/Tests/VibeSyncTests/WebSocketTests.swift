// WebSocketTests.swift — filtrage des rappels délégués (revue terra n°2).
//
// Aucun serveur n'est nécessaire : les méthodes déléguées sont appelées à la
// main. Ce qui est vérifié, c'est le tri — un rappel qui n'appartient pas à la
// tâche courante ne doit ni annoncer une ouverture (hello en double) ni couper
// la connexion active.

import XCTest
@testable import VibeSync

final class WebSocketTests: XCTestCase {

    // MARK: Adresse du serveur (VS-033)
    //
    // La normalisation vient du C commun (conn_normalize_url) : le client macOS
    // accepte enfin ce que le client Windows accepte depuis toujours, à
    // commencer par un hôte nu.

    func testAddressNormalization() {
        let cases: [(String, String)] = [
            // Hôte nu : wss + /ws. C'est CE cas que macOS refusait.
            ("vibesync.exemple.fr", "wss://vibesync.exemple.fr/ws"),
            ("VibeSync.Exemple.FR", "wss://vibesync.exemple.fr/ws"),
            ("  vibesync.exemple.fr  ", "wss://vibesync.exemple.fr/ws"),
            // Local : pas de TLS.
            ("localhost:8080", "ws://localhost:8080/ws"),
            ("127.0.0.1:8080", "ws://127.0.0.1:8080/ws"),
            // Schémas admis, http/https traduits.
            ("ws://127.0.0.1:8080/ws", "ws://127.0.0.1:8080/ws"),
            ("wss://vibesync.exemple.fr/ws", "wss://vibesync.exemple.fr/ws"),
            ("https://vibesync.exemple.fr", "wss://vibesync.exemple.fr/ws"),
            ("http://localhost:8080", "ws://localhost:8080/ws"),
            // Chemin absent ou réduit à « / » → /ws ; un vrai chemin est gardé.
            ("wss://vibesync.exemple.fr/", "wss://vibesync.exemple.fr/ws"),
            ("wss://vibesync.exemple.fr/salon/ws", "wss://vibesync.exemple.fr/salon/ws"),
            // Fragment jeté, requête gardée.
            ("wss://vibesync.exemple.fr/ws?x=1#ancre", "wss://vibesync.exemple.fr/ws?x=1"),
        ]
        for (raw, want) in cases {
            let got = ServerAddress.normalize(raw)
            XCTAssertEqual(got.url?.absoluteString, want, "normalisation de « \(raw) »")
            XCTAssertTrue(got.error.isEmpty, "« \(raw) » : \(got.error)")
        }

        for bad in ["", "   ", "ftp://exemple.fr/ws", "wss://", "wss:///ws"] {
            let got = ServerAddress.normalize(bad)
            XCTAssertNil(got.url, "« \(bad) » accepté à tort")
            XCTAssertFalse(got.error.isEmpty, "« \(bad) » refusé sans message")
        }
    }

    /// La règle non négociable de core/src/conn.c : une panne réseau programme
    /// un réessai, un refus du serveur JAMAIS.
    func testRetryPolicy() {
        let second: Nanos = 1_000_000_000
        var conn = ConnPolicy()
        let now: Nanos = 1_000 * second

        XCTAssertFalse(conn.shouldAttempt(now), "tentative avant tout démarrage")
        conn.start(now)
        XCTAssertTrue(conn.shouldAttempt(now))
        conn.attemptStarted()

        // Panne : réessai dans 1 s, puis 2 s (doublement borné).
        conn.socketDown(now)
        XCTAssertFalse(conn.shouldAttempt(now))
        XCTAssertEqual(conn.secondsUntilRetry(now), 1)
        XCTAssertTrue(conn.shouldAttempt(now + second))
        conn.attemptStarted()
        conn.socketDown(now + second)
        XCTAssertEqual(conn.secondsUntilRetry(now + second), 2)

        // Session établie : le backoff repart de zéro.
        conn.opened()
        conn.socketDown(now + 10 * second)
        XCTAssertEqual(conn.secondsUntilRetry(now + 10 * second), 1)

        // Refus : plus jamais de tentative, même après une socket qui tombe.
        conn.refused()
        XCTAssertFalse(conn.shouldAttempt(now + 100 * second))
        conn.socketDown(now + 100 * second)
        XCTAssertFalse(conn.shouldAttempt(now + 1000 * second),
                       "un refus du serveur a relancé une tentative")
    }

    /// Tâche étrangère, jamais démarrée : créer une tâche ne connecte rien.
    private func strayTask() -> URLSessionWebSocketTask {
        return URLSession.shared.webSocketTask(with: URL(string: "ws://127.0.0.1:9/ws")!)
    }

    func testDelegateIgnoresForeignTask() {
        let client = WebSocketClient()
        var events: [String] = []
        client.onEvent = { event in
            switch event {
            case .connected:
                events.append("connected")
            case .message:
                events.append("message")
            case .closed:
                events.append("closed")
            }
        }
        client.connect(url: URL(string: "ws://127.0.0.1:9/ws")!)
        guard let current = client.task else {
            XCTFail("aucune tâche courante après connect")
            return
        }
        let stale = strayTask()

        // Rappels d'une tâche abandonnée : ignorés.
        client.urlSession(URLSession.shared, webSocketTask: stale, didOpenWithProtocol: nil)
        XCTAssertTrue(events.isEmpty, "le didOpen d'une ancienne tâche a été pris pour argent comptant")
        XCTAssertFalse(client.isOpen)

        // Rappel de la tâche courante : traité.
        client.urlSession(URLSession.shared, webSocketTask: current, didOpenWithProtocol: nil)
        XCTAssertEqual(events, ["connected"])
        XCTAssertTrue(client.isOpen)

        // didClose d'une ancienne tâche : la connexion active survit.
        client.urlSession(URLSession.shared, webSocketTask: stale,
                          didCloseWith: .goingAway, reason: nil)
        XCTAssertEqual(events, ["connected"], "une ancienne tâche a coupé la connexion active")
        XCTAssertTrue(client.isOpen)

        // didClose de la tâche courante : la session est perdue.
        client.urlSession(URLSession.shared, webSocketTask: current,
                          didCloseWith: .goingAway, reason: nil)
        XCTAssertEqual(events, ["connected", "closed"])
        XCTAssertFalse(client.isOpen)

        client.close()
    }

    /// Régression : la boucle de reconnexion doit distinguer « handshake en
    /// cours » de « connecté ». Se fier à `isOpen` relançait le dial à chaque
    /// tour de 200 ms et aucune connexion distante n'aboutissait jamais.
    func testActiveDuringHandshake() {
        let client = WebSocketClient()
        XCTAssertFalse(client.isActive, "rien en vol avant connect")
        client.connect(url: URL(string: "ws://127.0.0.1:9/ws")!)
        XCTAssertTrue(client.isActive, "le handshake en cours doit compter comme une tentative vivante")
        XCTAssertFalse(client.isOpen, "…sans être ouverte pour autant")

        guard let current = client.task else {
            XCTFail("aucune tâche courante après connect")
            return
        }
        client.urlSession(URLSession.shared, webSocketTask: current, didOpenWithProtocol: nil)
        XCTAssertTrue(client.isActive)
        XCTAssertTrue(client.isOpen)

        // Tombée : la tentative est finie, la boucle peut en relancer une.
        client.urlSession(URLSession.shared, webSocketTask: current,
                          didCloseWith: .abnormalClosure, reason: nil)
        XCTAssertFalse(client.isActive)
        XCTAssertFalse(client.isOpen)
    }

    func testVoluntaryCloseWaitsForItsOwnAcknowledgement() {
        let client = WebSocketClient()
        var events: [String] = []
        client.onEvent = { event in
            if case .closed = event {
                events.append("closed")
            }
        }
        // Rien d'ouvert : il n'y a rien à attendre.
        XCTAssertTrue(client.closeAcknowledged)

        client.connect(url: URL(string: "ws://127.0.0.1:9/ws")!)
        guard let current = client.task else {
            XCTFail("aucune tâche courante après connect")
            return
        }
        client.urlSession(URLSession.shared, webSocketTask: current, didOpenWithProtocol: nil)

        client.close(normal: true)
        XCTAssertFalse(client.closeAcknowledged, "le départ volontaire n'attend pas son accusé")
        XCTAssertFalse(client.isOpen)

        // L'accusé d'une AUTRE tâche ne compte pas…
        client.urlSession(URLSession.shared, webSocketTask: strayTask(),
                          didCloseWith: .normalClosure, reason: nil)
        XCTAssertFalse(client.closeAcknowledged)

        // …celui de la tâche qu'on a fermée, si : et il ne remonte aucune perte
        // de session (le départ est volontaire).
        client.urlSession(URLSession.shared, webSocketTask: current,
                          didCloseWith: .normalClosure, reason: nil)
        XCTAssertTrue(client.closeAcknowledged)
        XCTAssertTrue(events.isEmpty, "un départ volontaire ne doit pas passer pour une coupure")
    }
}
