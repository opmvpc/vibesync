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
    /// Tâche courante. Interne (et non privée) pour que les tests puissent
    /// distinguer un rappel délégué légitime d'un rappel tardif.
    private(set) var task: URLSessionWebSocketTask?
    /// Génération : les rappels tardifs d'une connexion abandonnée sont ignorés.
    private var generation: Int = 0
    private var live: Bool = false
    /// Tâche en cours de fermeture volontaire : on n'en attend plus que
    /// l'accusé de réception du serveur (VS-028).
    private var closingTask: URLSessionWebSocketTask?
    private var closeAcked: Bool = true

    /// Vrai quand la dernière fermeture volontaire a été confirmée (ou qu'il
    /// n'y avait rien à confirmer).
    public var closeAcknowledged: Bool {
        return closeAcked
    }

    public override init() {
        super.init()
    }

    /// Vrai entre l'ouverture du handshake et la fermeture.
    public var isOpen: Bool {
        return live
    }

    /// Vrai tant qu'une tâche vit : handshake en cours OU session ouverte.
    ///
    /// C'est ce qu'il faut regarder avant de relancer une connexion. Se fier à
    /// `isOpen` seul relance le dial à chaque tour de boucle (200 ms) : le
    /// handshake est alors annulé avant d'aboutir et la connexion n'a lieu
    /// JAMAIS dès que le serveur est à plus de 200 ms (TLS + aller-retour vers
    /// un serveur distant). Une tâche qui n'aboutit pas est bornée par le
    /// délai de la requête, qui la fait tomber en `.closed`.
    public var isActive: Bool {
        return task != nil
    }

    /// Adresse du serveur telle qu'on la tape, normalisée par le C commun
    /// (ServerAddress → conn_normalize_url) : un hôte nu suffit désormais,
    /// comme sur le client Windows. `nil` s'accompagne toujours d'un message
    /// prêt à afficher.
    public static func parseURL(_ text: String) -> URL? {
        return ServerAddress.normalize(text).url
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

    /// Ferme la connexion. `normal` = départ volontaire (bouton « Quitter la
    /// salle », fermeture de l'app) : on envoie une close 1000 pour que le
    /// serveur retire le membre immédiatement et libère le pseudo, au lieu de
    /// laisser une connexion zombie expirer (VS-028, docs/protocol.md §Départ
    /// volontaire). Autrement 1001 « going away », qui décrit une coupure
    /// technique ou un simple redémarrage de la boucle de connexion.
    public func close(normal: Bool = false) {
        let wasLive = live
        live = false
        generation += 1
        let code: URLSessionWebSocketTask.CloseCode = normal ? .normalClosure : .goingAway
        let closing = task
        closing?.cancel(with: code, reason: nil)
        task = nil
        if normal && wasLive && closing != nil {
            // On garde la tâche sous la main : son didClose est le seul accusé
            // de réception du départ (l'appelant peut l'attendre brièvement).
            closingTask = closing
            closeAcked = false
        } else {
            closingTask = nil
            closeAcked = true
        }
        // invalidateAndCancel() couperait la socket avant que la trame de
        // fermeture ne parte : on laisse la session se terminer d'elle-même
        // après avoir vidé ce qui est en vol.
        session?.finishTasksAndInvalidate()
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

    // Les rappels délégués arrivent aussi pour des tâches abandonnées (une
    // connexion remplacée met un instant à se dénouer). Sans ce filtrage, le
    // didOpen d'une ancienne tâche renverrait un hello en double et son
    // didClose couperait la connexion active à sa place.

    public func urlSession(_ session: URLSession,
                           webSocketTask: URLSessionWebSocketTask,
                           didOpenWithProtocol protocolName: String?) {
        guard webSocketTask === task else {
            return
        }
        live = true
        onEvent?(.connected)
    }

    public func urlSession(_ session: URLSession,
                           webSocketTask: URLSessionWebSocketTask,
                           didCloseWith closeCode: URLSessionWebSocketTask.CloseCode,
                           reason: Data?) {
        if webSocketTask === closingTask {
            // Accusé de réception de notre propre départ : rien à signaler.
            closingTask = nil
            closeAcked = true
            return
        }
        guard webSocketTask === task else {
            return
        }
        fail("connexion fermée par le serveur (code \(closeCode.rawValue))")
    }
}
