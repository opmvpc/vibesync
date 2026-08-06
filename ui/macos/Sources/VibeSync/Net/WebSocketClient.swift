// WebSocketClient.swift — client WebSocket sur URLSessionWebSocketTask.
//
// ws:// et wss:// (TLS géré par le système). Une seule connexion à la fois ;
// la reconnexion (backoff 1 s → 10 s) et le re-hello sont pilotés par AppModel.
// Tous les événements sont republiés sur la file principale : le moteur et
// l'interface vivent sur le thread principal, sans verrou.

import Foundation

public final class WebSocketClient: NSObject, URLSessionWebSocketDelegate {

    public enum Event {
        case connected
        case message(String)
        /// Fermeture ou erreur : la session est perdue.
        case closed(reason: String)
    }

    /// Appelé sur la file principale.
    public var onEvent: ((Event) -> Void)?

    private var session: URLSession?
    private var task: URLSessionWebSocketTask?
    /// Génération : les rappels tardifs d'une connexion abandonnée sont ignorés.
    private var generation: Int = 0
    private var live: Bool = false

    public override init() {
        super.init()
    }

    /// Vrai entre l'ouverture du handshake et la fermeture.
    public var isOpen: Bool {
        return live
    }

    public static func parseURL(_ text: String) -> URL? {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty {
            return nil
        }
        guard let url = URL(string: trimmed), let scheme = url.scheme?.lowercased() else {
            return nil
        }
        if scheme != "ws" && scheme != "wss" {
            return nil
        }
        if url.host == nil || url.host?.isEmpty == true {
            return nil
        }
        return url
    }

    public func connect(url: URL) {
        close()
        generation += 1
        let gen = generation
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 30
        config.waitsForConnectivity = false
        let s = URLSession(configuration: config, delegate: self, delegateQueue: OperationQueue.main)
        var request = URLRequest(url: url)
        request.timeoutInterval = 15
        let t = s.webSocketTask(with: request)
        session = s
        task = t
        t.resume()
        receive(gen)
    }

    public func send(_ text: String, completion: @escaping (Bool) -> Void) {
        guard let t = task, live else {
            completion(false)
            return
        }
        t.send(.string(text)) { error in
            DispatchQueue.main.async {
                completion(error == nil)
            }
        }
    }

    public func close() {
        live = false
        generation += 1
        task?.cancel(with: .goingAway, reason: nil)
        task = nil
        session?.invalidateAndCancel()
        session = nil
    }

    private func receive(_ gen: Int) {
        guard let t = task else {
            return
        }
        t.receive { [weak self] result in
            DispatchQueue.main.async {
                guard let self = self, gen == self.generation else {
                    return
                }
                switch result {
                case .failure(let err):
                    self.fail(err.localizedDescription)
                case .success(let message):
                    switch message {
                    case .string(let text):
                        self.onEvent?(.message(text))
                    case .data(let data):
                        // Le protocole est texte ; on tolère un cadre binaire
                        // porteur d'UTF-8 plutôt que de couper la session.
                        if let text = String(data: data, encoding: .utf8) {
                            self.onEvent?(.message(text))
                        }
                    @unknown default:
                        break
                    }
                    self.receive(gen)
                }
            }
        }
    }

    private func fail(_ reason: String) {
        if !live && task == nil {
            return
        }
        live = false
        task?.cancel(with: .abnormalClosure, reason: nil)
        task = nil
        session?.invalidateAndCancel()
        session = nil
        onEvent?(.closed(reason: reason))
    }

    // MARK: - URLSessionWebSocketDelegate

    public func urlSession(_ session: URLSession,
                           webSocketTask: URLSessionWebSocketTask,
                           didOpenWithProtocol protocolName: String?) {
        live = true
        onEvent?(.connected)
    }

    public func urlSession(_ session: URLSession,
                           webSocketTask: URLSessionWebSocketTask,
                           didCloseWith closeCode: URLSessionWebSocketTask.CloseCode,
                           reason: Data?) {
        fail("connexion fermée par le serveur (code \(closeCode.rawValue))")
    }
}
