// AppModel.swift — assemblage : moteur + WebSocket + VLC + état de l'interface.
//
// Tout vit sur la file principale (le moteur est un struct, l'interface
// l'observe) : aucun verrou, aucune course. La boucle est un timer de 200 ms
// qui interroge VLC puis fait tourner le moteur, exactement comme la boucle du
// client C (ui/win32/src/main.c).

import AppKit
import Combine
import Foundation

public struct ChatLine: Identifiable {
    public let id = UUID()
    public let from: String
    public let text: String
    public let time: String
}

public struct ToastLine: Identifiable {
    public let id = UUID()
    public let level: String
    public let text: String
    let until: Date
}

public final class AppModel: ObservableObject {

    public enum Screen {
        case connect
        case room
    }

    // MARK: État publié

    @Published public var screen: Screen = .connect

    // Formulaire de connexion (mémorisé dans UserDefaults).
    @Published public var serverURL: String = ""
    @Published public var pseudo: String = ""
    @Published public var room: String = ""
    @Published public var password: String = ""
    @Published public var formError: String = ""

    // Session.
    @Published public var connected: Bool = false
    @Published public var connectionLabel: String = "hors ligne"
    @Published public var users: [ServerUser] = []
    @Published public var chatLines: [ChatLine] = []
    @Published public var toasts: [ToastLine] = []
    @Published public var draft: String = ""

    // Lecture.
    @Published public var ready: Bool = false
    @Published public var mediaName: String = ""
    @Published public var mediaLabel: String = "Aucun fichier ouvert"
    @Published public var durationSec: Double = 0
    @Published public var positionSec: Double = 0
    @Published public var roomPaused: Bool = true
    @Published public var driftSec: Double = 0
    @Published public var latencyMs: Int64 = 0
    @Published public var correctionLabel: String = ""
    @Published public var buffering: Bool = false
    @Published public var vlcRunning: Bool = false

    // MARK: Interne

    private var engine = Engine()
    private let ws = WebSocketClient()
    private var vlc: VLCProcess?
    private var timer: Timer?
    private var sessionToken: String = ""
    private var backoff: Nanos = 0
    private var nextAttempt: Nanos = 0
    private var wantConnection: Bool = false
    private var statusInFlight: Bool = false
    private var serverURLValue: URL?

    private static let keyServer = "vibesync.server"
    private static let keyName = "vibesync.name"
    private static let keyRoom = "vibesync.room"

    public init() {
        let defaults = UserDefaults.standard
        serverURL = defaults.string(forKey: AppModel.keyServer) ?? "ws://127.0.0.1:8080/ws"
        pseudo = defaults.string(forKey: AppModel.keyName) ?? NSFullUserName()
        room = defaults.string(forKey: AppModel.keyRoom) ?? "salon"
        sessionToken = Proto.sessionToken()

        ws.onEvent = { [weak self] event in
            self?.handle(event)
        }
        let t = Timer.scheduledTimer(withTimeInterval: 0.2, repeats: true) { [weak self] _ in
            self?.tick()
        }
        RunLoop.main.add(t, forMode: .common)
        timer = t
    }

    // MARK: - Connexion

    public func connect() {
        let trimmedName = pseudo.trimmingCharacters(in: .whitespacesAndNewlines)
        let trimmedRoom = room.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let url = WebSocketClient.parseURL(serverURL) else {
            formError = "Adresse invalide : attendu ws://hôte:port/ws ou wss://hôte/ws"
            return
        }
        if trimmedName.isEmpty || trimmedRoom.isEmpty {
            formError = "Pseudo et salle sont obligatoires"
            return
        }
        formError = ""
        pseudo = trimmedName
        room = trimmedRoom
        serverURLValue = url

        let defaults = UserDefaults.standard
        defaults.set(serverURL, forKey: AppModel.keyServer)
        defaults.set(trimmedName, forKey: AppModel.keyName)
        defaults.set(trimmedRoom, forKey: AppModel.keyRoom)

        wantConnection = true
        backoff = 0
        nextAttempt = VSTime.now()
        screen = .room
        startConnection()
    }

    public func leave() {
        wantConnection = false
        ws.close()
        engine.disconnected()
        connected = false
        connectionLabel = "hors ligne"
        users = []
        screen = .connect
        refresh()
    }

    public func shutdown() {
        wantConnection = false
        timer?.invalidate()
        timer = nil
        ws.close()
        vlc?.terminate()
        vlc = nil
    }

    private func startConnection() {
        guard let url = serverURLValue else {
            return
        }
        engine.connecting()
        connected = false
        connectionLabel = "connexion…"
        ws.connect(url: url)
    }

    private func scheduleReconnect() {
        backoff = Sync.nextBackoff(backoff)
        nextAttempt = VSTime.now() + backoff
        connectionLabel = "reconnexion dans \(Int(VSTime.seconds(backoff))) s…"
    }

    private func dropSession(_ reason: String) {
        ws.close()
        connected = false
        engine.sessionLost()
        if wantConnection {
            scheduleReconnect()
        } else {
            connectionLabel = "hors ligne"
        }
        if !reason.isEmpty {
            pushToast(level: "warn", text: reason)
        }
    }

    private func handle(_ event: WebSocketClient.Event) {
        switch event {
        case .connected:
            let hello = Proto.encodeHello(name: pseudo,
                                          room: room,
                                          password: password,
                                          session: sessionToken)
            ws.send(hello) { [weak self] ok in
                if !ok {
                    self?.dropSession("envoi du hello impossible")
                }
            }
        case .message(let text):
            handleServer(text)
        case .closed(let reason):
            dropSession(reason)
        }
    }

    private func handleServer(_ raw: String) {
        guard let msg = Proto.decode(raw) else {
            return  // message illisible : ignoré (forward-compat)
        }
        let now = VSTime.now()
        var out: [Decision] = []

        switch msg {
        case .welcome(let selfId, let roomName, let state, let userList, let selfReady):
            backoff = 0
            connected = true
            connectionLabel = "connecté à « \(roomName) »"
            users = userList
            out += engine.onWelcome(now: now, selfId: selfId, state: state, selfReady: selfReady)

        case .pong(let p):
            engine.onPong(now: now, p)

        case .roomState(let rs):
            engine.onRoomState(now: now, rs)

        case .users(let list):
            users = list
            for u in list where u.id == engine.selfId {
                engine.onSelfReady(u.ready)
                break
            }

        case .chatEvent(let from, let text, _):
            appendChat(from: from, text: text)

        case .toast(let level, let text):
            pushToast(level: level, text: text)

        case .error(let code, let text):
            pushToast(level: "error", text: text.isEmpty ? code : text)
            if Proto.isFatal(code) {
                wantConnection = false
                ws.close()
                engine.disconnected()
                connected = false
                connectionLabel = "refusé par le serveur (\(code))"
                screen = .connect
                formError = text.isEmpty ? code : text
            }

        case .unknown:
            break
        }

        apply(out)
        refresh()
    }

    // MARK: - Boucle

    private func tick() {
        let now = VSTime.now()
        if wantConnection && !ws.isOpen && now >= nextAttempt {
            startConnection()
            return
        }
        guard let player = vlc, !statusInFlight else {
            apply(engine.onTick(now: VSTime.now()))
            refresh()
            return
        }
        statusInFlight = true
        player.client.status { [weak self] result in
            guard let self = self else {
                return
            }
            self.statusInFlight = false
            let t = VSTime.now()
            var out: [Decision] = []
            switch result {
            case .success(let st):
                out += self.engine.onVLCStatus(now: t, st)
            case .failure(let err):
                self.engine.onVLCError()
                self.mediaLabel = err.text
            }
            out += self.engine.onTick(now: t)
            self.apply(out)
            self.refresh()
        }
    }

    /// Pousse les décisions du moteur : messages au serveur, commandes à VLC.
    /// Une erreur d'écriture ferme la connexion (pas de perte silencieuse
    /// d'un `control`) — docs/protocol.md §Assainissement.
    private func apply(_ decisions: [Decision]) {
        for d in decisions {
            switch d {
            case .server(let message):
                guard ws.isOpen else {
                    continue
                }
                ws.send(Proto.encode(message)) { [weak self] ok in
                    if !ok {
                        self?.dropSession("écriture vers le serveur impossible")
                    }
                }
            case .vlc(let command):
                guard let player = vlc else {
                    continue
                }
                player.client.apply(command) { [weak self] result in
                    if case .failure(let err) = result {
                        self?.mediaLabel = err.text
                    }
                }
            }
        }
    }

    /// Recopie l'état du moteur vers l'interface.
    private func refresh() {
        ready = engine.ready
        driftSec = engine.drift
        latencyMs = engine.latencyMs
        roomPaused = engine.roomState.paused
        buffering = engine.buffering
        vlcRunning = vlc != nil
        switch engine.correcting {
        case .none:
            correctionLabel = engine.nudging ? "ajustement" : ""
        case .nudge:
            correctionLabel = "ajustement de vitesse"
        case .seek:
            correctionLabel = "resynchronisation"
        }
        if engine.haveStatus {
            positionSec = engine.status.positionSec
            durationSec = engine.status.lengthSec > 0 ? engine.status.lengthSec : engine.fileDurationSec
        } else if engine.haveState {
            positionSec = engine.expectedPosition(VSTime.now())
            durationSec = engine.fileDurationSec
        }
        if !engine.fileName.isEmpty {
            mediaName = engine.fileName
            if vlc != nil {
                mediaLabel = engine.fileDeclared ? engine.fileName : "chargement de « \(engine.fileName) »…"
            }
        }
        let now = Date()
        let live = toasts.filter { $0.until > now }
        if live.count != toasts.count {
            toasts = live
        }
    }

    // MARK: - Actions de l'utilisateur

    public func toggleReady() {
        apply(engine.setReady(!engine.ready))
        refresh()
    }

    public func togglePlayback() {
        let action: ControlAction = roomPaused ? .play : .pause
        apply(engine.userControl(now: VSTime.now(), action: action, positionSec: nil))
        refresh()
    }

    public func seek(to seconds: Double) {
        apply(engine.userControl(now: VSTime.now(), action: .seek, positionSec: seconds))
        refresh()
    }

    public func sendDraft() {
        let text = draft.trimmingCharacters(in: .whitespacesAndNewlines)
        draft = ""
        if text.isEmpty {
            return
        }
        apply(engine.chat(text))
    }

    /// Sélection du fichier puis lancement de VLC dessus.
    public func chooseFile() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        panel.title = "Choisir la vidéo"
        panel.message = "Chacun ouvre sa propre copie du fichier."
        panel.prompt = "Ouvrir"
        if panel.runModal() == NSApplication.ModalResponse.OK, let url = panel.url {
            open(url)
        }
    }

    private func open(_ url: URL) {
        vlc?.terminate()
        vlc = nil
        vlcRunning = false

        var size: Int64 = 0
        if let attrs = try? FileManager.default.attributesOfItem(atPath: url.path),
           let n = attrs[FileAttributeKey.size] as? NSNumber {
            size = n.int64Value
        }
        apply(engine.openFile(name: url.lastPathComponent, sizeBytes: size))
        mediaName = url.lastPathComponent
        mediaLabel = "lancement de VLC…"

        VLCLauncher.launch(filePath: url.path) { [weak self] result in
            guard let self = self else {
                return
            }
            switch result {
            case .success(let process):
                self.vlc = process
                self.vlcRunning = true
                self.mediaLabel = "chargement de « \(url.lastPathComponent) »…"
            case .failure(let err):
                self.mediaLabel = err.text
                self.pushToast(level: "error", text: err.text)
            }
        }
    }

    // MARK: - Journal

    private func appendChat(from: String, text: String) {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm"
        chatLines.append(ChatLine(from: from, text: text, time: formatter.string(from: Date())))
        if chatLines.count > 200 {
            chatLines.removeFirst(chatLines.count - 200)
        }
    }

    private func pushToast(level: String, text: String) {
        if text.isEmpty {
            return
        }
        toasts.append(ToastLine(level: level, text: text, until: Date().addingTimeInterval(6)))
        if toasts.count > 4 {
            toasts.removeFirst(toasts.count - 4)
        }
    }

    // MARK: - Mise en forme

    public static func timeLabel(_ seconds: Double) -> String {
        if !seconds.isFinite || seconds < 0 {
            return "0:00"
        }
        let total = Int(seconds.rounded(.down))
        let h = total / 3600
        let m = (total % 3600) / 60
        let s = total % 60
        if h > 0 {
            return String(format: "%d:%02d:%02d", h, m, s)
        }
        return String(format: "%d:%02d", m, s)
    }

    public var driftLabel: String {
        let ms = Int((driftSec * 1000).rounded())
        if ms == 0 {
            return "aligné"
        }
        return ms > 0 ? "+\(ms) ms" : "\(ms) ms"
    }
}
