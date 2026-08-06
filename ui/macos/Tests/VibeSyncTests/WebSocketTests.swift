// WebSocketTests.swift — filtrage des rappels délégués (revue terra n°2).
//
// Aucun serveur n'est nécessaire : les méthodes déléguées sont appelées à la
// main. Ce qui est vérifié, c'est le tri — un rappel qui n'appartient pas à la
// tâche courante ne doit ni annoncer une ouverture (hello en double) ni couper
// la connexion active.

import XCTest
@testable import VibeSync

final class WebSocketTests: XCTestCase {

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
