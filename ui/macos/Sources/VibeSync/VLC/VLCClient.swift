// VLCClient.swift — pilotage de l'interface HTTP locale de VLC.
//
// Authentification Basic avec un utilisateur vide (comme internal/vlc/http.go).
// Toutes les requêtes visent 127.0.0.1 ; les rappels reviennent sur la file
// principale.

import Foundation

public enum VLCError: Error {
    case notFound
    case spawn(String)
    case unreachable(String)
    case auth
    case http(Int)
    case badJSON
    case timeout
    /// Le média n'a pas pu être mis en pause au début (cf. `prepare`).
    case notReady(String)

    public var text: String {
        switch self {
        case .notFound:
            return "VLC introuvable (installez VLC dans /Applications ou renseignez VIBESYNC_VLC)"
        case .spawn(let m):
            return "lancement de VLC impossible : \(m)"
        case .unreachable(let m):
            return "interface HTTP de VLC injoignable : \(m)"
        case .auth:
            return "authentification refusée par l'interface HTTP de VLC"
        case .http(let code):
            return "VLC a répondu HTTP \(code)"
        case .badJSON:
            return "status.json illisible"
        case .timeout:
            return "interface HTTP de VLC muette"
        case .notReady(let m):
            return "média non arrêté au début : \(m)"
        }
    }
}

public final class VLCClient {

    public let port: Int
    public let password: String
    private let session: URLSession
    private let authHeader: String

    public init(port: Int, password: String) {
        self.port = port
        self.password = password
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 3
        config.timeoutIntervalForResource = 5
        config.httpMaximumConnectionsPerHost = 4
        config.requestCachePolicy = .reloadIgnoringLocalAndRemoteCacheData
        self.session = URLSession(configuration: config)
        let raw = ":" + password
        let encoded = Data(raw.utf8).base64EncodedString()
        self.authHeader = "Basic " + encoded
    }

    private func url(command: String?, value: String?) -> URL? {
        var comps = URLComponents()
        comps.scheme = "http"
        comps.host = "127.0.0.1"
        comps.port = port
        comps.path = "/requests/status.json"
        if let c = command {
            var items = [URLQueryItem(name: "command", value: c)]
            if let v = value {
                items.append(URLQueryItem(name: "val", value: v))
            }
            comps.queryItems = items
        }
        return comps.url
    }

    private func call(command: String?, value: String?, completion: @escaping (Result<VLCStatus, VLCError>) -> Void) {
        guard let u = url(command: command, value: value) else {
            DispatchQueue.main.async { completion(.failure(.unreachable("URL invalide"))) }
            return
        }
        var request = URLRequest(url: u)
        request.httpMethod = "GET"
        request.setValue(authHeader, forHTTPHeaderField: "Authorization")
        request.setValue("close", forHTTPHeaderField: "Connection")
        let task = session.dataTask(with: request) { data, response, error in
            var result: Result<VLCStatus, VLCError>
            if let e = error {
                result = .failure(.unreachable(e.localizedDescription))
            } else if let http = response as? HTTPURLResponse, http.statusCode == 401 {
                result = .failure(.auth)
            } else if let http = response as? HTTPURLResponse, http.statusCode != 200 {
                result = .failure(.http(http.statusCode))
            } else if let d = data, let root = JSON.object(JSON.parse(d)) {
                result = .success(VLCStatusParser.parse(object: root))
            } else {
                result = .failure(.badJSON)
            }
            DispatchQueue.main.async {
                completion(result)
            }
        }
        task.resume()
    }

    public func status(completion: @escaping (Result<VLCStatus, VLCError>) -> Void) {
        call(command: nil, value: nil, completion: completion)
    }

    /// Applique une décision du moteur.
    public func apply(_ cmd: VLCCommand, completion: @escaping (Result<VLCStatus, VLCError>) -> Void) {
        switch cmd.kind {
        case .pause:
            call(command: "pl_forcepause", value: nil, completion: completion)
        case .resume:
            call(command: "pl_forceresume", value: nil, completion: completion)
        case .seek:
            var pos = cmd.value
            if !pos.isFinite || pos < 0 {
                pos = 0
            }
            // L'API HTTP de VLC n'accepte qu'une seconde entière.
            call(command: "seek", value: String(Int(pos.rounded())), completion: completion)
        case .rate:
            var rate = cmd.value
            if !rate.isFinite || rate <= 0 {
                rate = 1
            }
            call(command: "rate", value: JSONVal.number(rate), completion: completion)
        }
    }

    // MARK: - Préparation du média

    /// Position en deçà de laquelle le média est « au début » (le seek HTTP est
    /// arrondi à la seconde : viser mieux n'aurait pas de sens).
    public static let startTolerance: Double = 0.5
    /// Pas de scrutation pendant la préparation, comme internal/vlc/prepare.go.
    static let preparePoll: Double = 0.02
    public static let prepareTimeoutSec: Double = 15

    /// Met un média fraîchement ouvert en pause à la position 0, et ne rend la
    /// main qu'une fois cet état **observé** (port de internal/vlc/prepare.go,
    /// docs/protocol.md §Chargement de fichier).
    ///
    /// Indispensable : VLC démarre la lecture tout seul à l'ouverture. Sans
    /// cette étape, deux clients qui ouvrent leur média à quelques centaines de
    /// millisecondes d'écart démarrent déjà désynchronisés, et le rattrapage au
    /// rate (5 %/s) mettrait une dizaine de secondes.
    ///
    /// La boucle est volontairement idempotente : on redemande pause et seek 0
    /// tant que l'état visé n'est pas constaté, ce qui absorbe aussi bien le
    /// délai d'ouverture du média que les commandes perdues. L'échec d'une
    /// commande n'interrompt rien mais devient l'erreur retenue : si l'état
    /// visé n'est jamais atteint, c'est elle que voit l'appelant.
    public func prepare(timeoutSec: Double = VLCClient.prepareTimeoutSec,
                        completion: @escaping (Result<Void, VLCError>) -> Void) {
        prepareStep(deadline: Date().addingTimeInterval(timeoutSec), completion: completion)
    }

    private func prepareStep(deadline: Date, completion: @escaping (Result<Void, VLCError>) -> Void) {
        status { [weak self] result in
            guard let self = self else {
                return
            }
            guard case .success(let st) = result else {
                if case .failure(let err) = result {
                    self.prepareRetry(deadline, err, completion)
                }
                return
            }
            if !st.loaded {
                // Le média n'est pas encore ouvert : rien à commander.
                self.prepareRetry(deadline, .notReady("aucun média chargé (état \(st.state.rawValue))"),
                                  completion)
                return
            }
            let atStart = st.positionSec < VLCClient.startTolerance
            if st.state == .paused && atStart {
                completion(.success(()))
                return
            }
            let observed = VLCError.notReady(String(format: "média %@ à %.2f s",
                                                    st.state.rawValue, st.positionSec))
            // pause puis seek 0, en séquence comme Prepare.
            let afterPause: (VLCError) -> Void = { pending in
                if !atStart {
                    self.apply(VLCCommand(.seek, 0)) { seeked in
                        if case .failure(let err) = seeked {
                            self.prepareRetry(deadline, err, completion)
                        } else {
                            self.prepareRetry(deadline, pending, completion)
                        }
                    }
                } else {
                    self.prepareRetry(deadline, pending, completion)
                }
            }
            if st.state == .playing {
                self.apply(VLCCommand(.pause)) { paused in
                    if case .failure(let err) = paused {
                        afterPause(err)
                    } else {
                        afterPause(observed)
                    }
                }
            } else {
                afterPause(observed)
            }
        }
    }

    private func prepareRetry(_ deadline: Date,
                              _ last: VLCError,
                              _ completion: @escaping (Result<Void, VLCError>) -> Void) {
        if Date() >= deadline {
            completion(.failure(last))
            return
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + VLCClient.preparePoll) { [weak self] in
            guard let self = self else {
                return
            }
            self.prepareStep(deadline: deadline, completion: completion)
        }
    }

    /// Attend que l'interface HTTP réponde (VLC met un instant à démarrer).
    public func waitReady(timeoutSec: Double, completion: @escaping (Result<Void, VLCError>) -> Void) {
        let deadline = Date().addingTimeInterval(timeoutSec)
        attemptReady(deadline: deadline, completion: completion)
    }

    private func attemptReady(deadline: Date, completion: @escaping (Result<Void, VLCError>) -> Void) {
        status { [weak self] result in
            switch result {
            case .success:
                completion(.success(()))
            case .failure(let err):
                if Date() >= deadline {
                    if case .auth = err {
                        completion(.failure(.auth))
                    } else {
                        completion(.failure(.timeout))
                    }
                    return
                }
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) {
                    self?.attemptReady(deadline: deadline, completion: completion)
                }
            }
        }
    }
}
