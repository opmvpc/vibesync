// AppModel.swift — assemblage : moteur + WebSocket + VLC + état de l'interface.
//
// Tout vit sur la file principale (le moteur est un struct, l'interface
// l'observe) : aucun verrou, aucune course. La boucle est un timer de 200 ms
// qui interroge VLC puis fait tourner le moteur, exactement comme la boucle du
// client C (ui/win32/src/main.c).

import AppKit
import Combine
import Foundation
import UniformTypeIdentifiers
import VSCore

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

    // Formulaire de connexion (mémorisé dans UserDefaults ; le mot de passe va
    // au trousseau, cf. Keychain.swift).
    @Published public var serverURL: String = ""
    @Published public var pseudo: String = ""
    @Published public var room: String = ""
    @Published public var password: String = ""
    @Published public var rememberPassword: Bool = true
    @Published public var formError: String = ""

    // Versions (VS-023). La bannière n'est qu'une invitation : la compatibilité
    // dure reste celle du protocole, tranchée par le serveur au hello.
    public let clientVersion: String = AppVersion.current
    @Published public var serverVersion: String = ""
    @Published public var downloadURL: String = ""
    @Published public var showUpdateBanner: Bool = false

    // Dossiers médias et bandeaux associés (VS-026).
    @Published public var showSettings: Bool = false
    @Published public var mediaDirs: [String] = []
    /// Chemin de VLC réglé à la main (vide = détection automatique) et l'état
    /// affiché sous le champ. Parité avec le champ du panneau Windows.
    @Published public var vlcPath: String = ""
    @Published public var vlcStatus: VLCPathStatus = .undetected
    /// « X regarde <fichier> — cliquer pour l'ouvrir chez vous ».
    @Published public var watchWho: String = ""
    @Published public var watchFile: String = ""
    @Published public var showWatchBanner: Bool = false
    /// « <fichier> introuvable » + raccourci vers les Réglages.
    @Published public var mediaNotice: String = ""
    @Published public var showMediaNotice: Bool = false
    @Published public var mediaSearching: Bool = false

    // Session.
    @Published public var connected: Bool = false
    @Published public var connectionLabel: String = "hors ligne"
    /// Dernière raison de perte de session (le libellé de connexion, lui, est
    /// écrasé par le décompte de reconnexion).
    @Published public var lastError: String = ""
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

    /// Le moteur de synchronisation : la couche C commune (VSCore) vue à
    /// travers CoreEngine.swift. Une classe, donc une référence — mais la même
    /// discipline qu'avant : tous les appels partent de la file principale.
    private let engine = CoreEngine()
    private let ws = WebSocketClient()
    private var vlc: VLCProcess?
    private var timer: Timer?
    private let store: PrefStore
    private var sessionToken: String = ""
    /// Quand ouvrir une socket, quand renoncer : la politique commune du C
    /// (core/src/conn.c), la même que le client Windows depuis VS-033.
    private var conn = ConnPolicy()
    private var wantConnection: Bool = false
    private var statusInFlight: Bool = false
    private var serverURLValue: URL?
    /// Fichier d'un participant dont le bandeau a été écarté : on ne le
    /// ressuscite pas pour le même fichier.
    private var dismissedWatchFile: String = ""
    /// Génération de la recherche de média : une recherche est longue et n'est
    /// pas annulable, mais son résultat devient caduc dès qu'on quitte la
    /// salle, qu'on écarte le bandeau ou qu'on ouvre un autre fichier. Sans ce
    /// jeton, un résultat périmé lancerait VLC après coup.
    private var mediaSearchGen: Int = 0

    /// Jeton d'activité : sans lui, macOS met l'application en App Nap dès
    /// qu'elle n'est plus au premier plan — c'est-à-dire exactement pendant
    /// qu'on regarde le film dans VLC. App Nap ralentit les tâches périodiques
    /// et diffère le réseau : le moteur de sync ne peut pas travailler dans ces
    /// conditions. On le tient pour toute la vie de l'application.
    private var activity: NSObjectProtocol?

    // Pilote du harnais de test réel (nil = application normale).
    private let auto: AutoPilot?
    private var autoLastStatus: Nanos = 0
    /// Nombre de lignes déjà consommées dans le fichier de commandes.
    private var autoCommandsDone: Int = 0

    public init(store: PrefStore = UserDefaults.standard, auto: AutoPilot? = nil) {
        self.store = store
        self.auto = auto
        serverURL = store.string(forKey: Preferences.keyServer) ?? "ws://127.0.0.1:8080/ws"
        pseudo = store.string(forKey: Preferences.keyName) ?? NSFullUserName()
        room = store.string(forKey: Preferences.keyRoom) ?? "salon"
        // Jeton persisté (VS-028) : un relancement de l'app récupère le pseudo
        // immédiatement, sans attendre l'expiration de la connexion zombie.
        sessionToken = Preferences.sessionToken(store)
        mediaDirs = Preferences.mediaDirs(store)
        vlcPath = Preferences.vlcPath(store)
        vlcStatus = VLCLauncher.pathStatus(setting: vlcPath)
        rememberPassword = Preferences.rememberPassword(store)
        if let auto = auto {
            // Pilote : tout vient de l'environnement, et surtout PAS du
            // trousseau — deux instances de test ne doivent ni le lire ni
            // l'écrire (le mot de passe du harnais n'a rien à y faire).
            serverURL = auto.url
            pseudo = auto.name
            room = auto.room
            password = auto.password
            rememberPassword = false
        } else if rememberPassword {
            password = Keychain.read(account: Keychain.serverPasswordAccount) ?? ""
        }

        activity = ProcessInfo.processInfo.beginActivity(
            options: [.userInitiated, .automaticTerminationDisabled],
            reason: "synchronisation de lecture")

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
        // Un hôte nu suffit (« vibesync.exemple.fr » → wss://…/ws) et le
        // message d'erreur est celui du C commun, en français.
        let address = ServerAddress.normalize(serverURL)
        guard let url = address.url else {
            formError = address.error
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

        store.set(serverURL, forKey: Preferences.keyServer)
        store.set(trimmedName, forKey: Preferences.keyName)
        store.set(trimmedRoom, forKey: Preferences.keyRoom)
        savePassword()

        wantConnection = true
        conn.start(VSTime.now())
        screen = .room
        startConnection()
    }

    /// Départ volontaire : close 1000 pour que le serveur retire le membre tout
    /// de suite et libère le pseudo (VS-028).
    public func leave() {
        wantConnection = false
        conn.cancel()
        ws.close(normal: true)
        engine.disconnected()
        connected = false
        connectionLabel = "hors ligne"
        users = []
        clearBanners()
        screen = .connect
        refresh()
    }

    /// Fermeture de l'application : même départ volontaire, plus un court délai
    /// pour laisser partir la trame de fermeture avant que le processus meure.
    public func shutdown() {
        wantConnection = false
        conn.cancel()
        timer?.invalidate()
        timer = nil
        ws.close(normal: true)
        vlc?.terminate()
        vlc = nil
        // On attend l'accusé de réception du serveur, pas un délai aveugle :
        // la trame de fermeture doit être partie avant que le processus meure,
        // sinon le pseudo reste bloqué (VS-028). Repli à 250 ms, comme le
        // NET_CLOSE_GRACE_MS du client Windows.
        let deadline = Date().addingTimeInterval(AppModel.closeGraceSec)
        while !ws.closeAcknowledged && Date() < deadline {
            RunLoop.current.run(mode: RunLoop.Mode.default, before: Date().addingTimeInterval(0.02))
        }
        if let token = activity {
            ProcessInfo.processInfo.endActivity(token)
            activity = nil
        }
    }

    /// Délai maximal accordé à la close 1000 pour partir et être accusée.
    public static let closeGraceSec: Double = 0.25

    private func clearBanners() {
        invalidateMediaSearch()
        showWatchBanner = false
        showMediaNotice = false
        showUpdateBanner = false
        watchWho = ""
        watchFile = ""
        dismissedWatchFile = ""
    }

    /// Mémorise (ou oublie) le mot de passe du serveur — VS-025. Le clair ne
    /// touche jamais le disque : c'est le trousseau qui le chiffre.
    ///
    /// Le trousseau peut refuser (session verrouillée, ACL d'un binaire
    /// re-signé…). Dans ce cas on le dit et on décoche : laisser croire que le
    /// mot de passe est mémorisé serait pire que de le retaper.
    private func savePassword() {
        if auto != nil {
            return  // mode pilote : le trousseau n'est jamais touché
        }
        if rememberPassword && !password.isEmpty {
            if !Keychain.write(password, account: Keychain.serverPasswordAccount) {
                rememberPassword = false
                Preferences.setRememberPassword(false, store)
                pushToast(level: "warn",
                          text: "Le trousseau a refusé d'enregistrer le mot de passe : il ne sera pas mémorisé.")
                return
            }
        } else if !Keychain.delete(account: Keychain.serverPasswordAccount) {
            pushToast(level: "warn", text: "Le trousseau a refusé d'oublier le mot de passe enregistré.")
        }
        Preferences.setRememberPassword(rememberPassword, store)
    }

    /// Appelé quand la case « Se souvenir » change d'état.
    public func rememberPasswordChanged() {
        savePassword()
    }

    private func startConnection() {
        guard let url = serverURLValue else {
            return
        }
        conn.attemptStarted()
        engine.connecting(room: room)
        connected = false
        connectionLabel = "connexion…"
        ws.connect(url: url)
    }

    private func dropSession(_ reason: String) {
        ws.close()
        connected = false
        engine.sessionLost()
        if wantConnection {
            let now = VSTime.now()
            conn.socketDown(now)
            connectionLabel = "reconnexion dans \(conn.secondsUntilRetry(now)) s…"
        } else {
            connectionLabel = "hors ligne"
        }
        if !reason.isEmpty {
            lastError = reason
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
        case .welcome(let w):
            conn.opened()
            connected = true
            connectionLabel = "connecté à « \(w.room) »"
            users = w.users
            if !w.room.isEmpty {
                room = w.room
            }
            noteServerVersion(w)
            out += engine.onWelcome(now: now,
                                    selfId: w.selfId,
                                    state: w.state,
                                    selfReady: w.selfReady)
            // Reprise « salle vierge » : le moteur a émis UN control seek,
            // l'utilisateur doit savoir pourquoi le film ne repart pas à zéro
            // (même toast que le client Windows).
            if let resume = engine.resumeToastSec {
                pushToast(level: "info", text: "Reprise à \(AppModel.timeLabel(resume))")
            }
            refreshWatchBanner()

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
            refreshWatchBanner()

        case .chatEvent(let from, let text, _):
            appendChat(from: from, text: text)

        case .toast(let level, let text):
            pushToast(level: level, text: text)

        case .error(let code, let text):
            pushToast(level: "error", text: text.isEmpty ? code : text)
            if Proto.isFatal(code) {
                // Refus du serveur : ARRÊT NET, jamais de nouvelle tentative
                // (c'est la boucle « Nouvelle tentative… » après un mauvais mot
                // de passe que conn_on_refused interdit).
                wantConnection = false
                conn.refused()
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

    // MARK: - Version du serveur (VS-023)

    /// Enregistre ce que le serveur dit de lui-même et lève la bannière s'il
    /// tourne une version applicative plus récente que la nôtre. Purement
    /// informatif : rien n'est bloqué.
    private func noteServerVersion(_ w: Welcome) {
        serverVersion = w.serverVersion
        downloadURL = w.downloadUrl
        showUpdateBanner = AppVersion.newer(w.serverVersion, than: clientVersion)
    }

    public var updateBannerText: String {
        return "Nouvelle version disponible : \(serverVersion) (vous avez \(clientVersion))"
    }

    public func openDownloadPage() {
        showUpdateBanner = false
        guard let url = URL(string: downloadURL), url.scheme == "https" || url.scheme == "http" else {
            return
        }
        NSWorkspace.shared.open(url)
    }

    // MARK: - Dossiers médias (VS-026)

    /// Propose d'ouvrir le média que les autres regardent déjà, tant que nous
    /// n'en avons pas ouvert un. Le bandeau ne ressuscite pas une fois écarté
    /// pour ce fichier.
    private func refreshWatchBanner() {
        if engine.haveFile {
            showWatchBanner = false  // on a déjà notre copie ouverte
            return
        }
        for u in users where !u.id.isEmpty && u.id != engine.selfId && u.hasFile && !u.fileName.isEmpty {
            if u.fileName == dismissedWatchFile {
                return
            }
            // Réaffecter à l'identique réveillerait SwiftUI à chaque broadcast.
            if watchWho != u.name {
                watchWho = u.name
            }
            if watchFile != u.fileName {
                watchFile = u.fileName
            }
            if !showWatchBanner {
                showWatchBanner = true
            }
            return
        }
        if showWatchBanner {
            showWatchBanner = false
        }
    }

    /// Périme le résultat de toute recherche de média en vol.
    private func invalidateMediaSearch() {
        mediaSearchGen += 1
        mediaSearching = false
    }

    public func dismissWatchBanner() {
        dismissedWatchFile = watchFile
        showWatchBanner = false
        invalidateMediaSearch()
    }

    public func dismissMediaNotice() {
        showMediaNotice = false
    }

    public func openSettingsFromNotice() {
        showMediaNotice = false
        showSettings = true
    }

    /// Clic sur le bandeau « X regarde … » : recherche du fichier dans les
    /// dossiers configurés, hors thread principal, puis lancement de VLC.
    public func openWatchedFile() {
        let name = watchFile
        if name.isEmpty || mediaSearching {
            return
        }
        if mediaDirs.isEmpty {
            mediaNotice = "Aucun dossier média configuré — cliquer pour ouvrir les Réglages"
            showMediaNotice = true
            return
        }
        showMediaNotice = false
        mediaSearching = true
        let gen = mediaSearchGen
        pushToast(level: "info", text: "Recherche de « \(name) » dans vos dossiers…")
        MediaLibrary.find(name: name, in: mediaDirs) { [weak self] result in
            guard let self = self, gen == self.mediaSearchGen else {
                return  // recherche périmée : ne surtout pas lancer VLC
            }
            self.mediaSearching = false
            if result.found {
                // Homonymes : le plus gros gagne. C'est une heuristique, elle
                // est tracée pour pouvoir être contestée.
                FileHandle.standardError.write(Data("""
                    vibesync: « \(name) » trouvé (\(result.matches) correspondance(s), \
                    \(result.visited) entrées) : \(result.path)

                    """.utf8))
                self.showWatchBanner = false
                self.open(URL(fileURLWithPath: result.path))
                return
            }
            let ecourtee = result.truncated ? " (recherche écourtée)" : ""
            self.mediaNotice = "« \(name) » introuvable\(ecourtee) — cliquer pour ouvrir les Réglages"
            self.showMediaNotice = true
            self.showWatchBanner = false
        }
    }

    /// Ajout d'un dossier média par le dialogue natif.
    public func addMediaDir() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        panel.title = "Choisir un dossier de médias"
        panel.prompt = "Ajouter"
        if panel.runModal() == NSApplication.ModalResponse.OK, let url = panel.url {
            var dirs = mediaDirs
            dirs.append(url.path)
            mediaDirs = Preferences.normalizeMediaDirs(dirs)
            Preferences.setMediaDirs(mediaDirs, store)
        }
    }

    public func removeMediaDir(at index: Int) {
        guard index >= 0 && index < mediaDirs.count else {
            return
        }
        mediaDirs.remove(at: index)
        Preferences.setMediaDirs(mediaDirs, store)
    }

    // MARK: - Chemin de VLC

    /// Le champ a changé (frappe ou retour du sélecteur) : on enregistre et on
    /// recalcule l'état affiché. Un accès disque par frappe, comme le client
    /// Windows — c'est un stat sur un chemin, pas une recherche.
    public func vlcPathChanged() {
        Preferences.setVLCPath(vlcPath, store)
        refreshVLCStatus()
    }

    public func refreshVLCStatus() {
        let status = VLCLauncher.pathStatus(setting: vlcPath)
        if status != vlcStatus {
            vlcStatus = status
        }
    }

    /// « Parcourir… » : l'utilisateur désigne VLC.app ou un binaire nu. On
    /// stocke EXACTEMENT ce qu'il a choisi ; la traversée d'un bundle vers son
    /// exécutable est faite au lancement (VLCLauncher.settingBinary), pour que
    /// le champ reste lisible et corrigeable à la main.
    public func browseVLC() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        // Un bundle .app est un dossier : il ne doit surtout pas être traversé,
        // sinon on ne peut plus le sélectionner, seulement entrer dedans.
        panel.treatsFilePackagesAsDirectories = false
        panel.allowsMultipleSelection = false
        panel.directoryURL = URL(fileURLWithPath: "/Applications")
        panel.title = "Choisir VLC"
        panel.message = "Sélectionnez VLC.app, ou directement le binaire vlc."
        panel.prompt = "Choisir"
        panel.allowedContentTypes = [.application, .unixExecutable, .executable]
        if panel.runModal() == NSApplication.ModalResponse.OK, let url = panel.url {
            vlcPath = url.path
            vlcPathChanged()
        }
    }

    // MARK: - Pilote du harnais de test réel

    /// Vrai si l'application est pilotée par l'environnement.
    public var isAutoPiloted: Bool {
        return auto != nil
    }

    /// Démarre le pilote : connexion immédiate, puis ouverture du média.
    /// Sans pilote, ne fait rien. Appelé par l'AppDelegate au lancement.
    public func startAutoPilot() {
        guard let auto = auto else {
            return
        }
        connect()
        if !auto.file.isEmpty {
            open(URL(fileURLWithPath: auto.file))
        }
        autoWriteStatus(VSTime.now())
    }

    /// Un tour de pilote : commandes en attente puis état publié.
    private func autoPump(_ now: Nanos) {
        guard let auto = auto else {
            return
        }
        autoRunCommands(auto)
        if now - autoLastStatus >= Int64(AutoPilot.statusEverySec * 1e9) {
            autoLastStatus = now
            autoWriteStatus(now)
        }
    }

    /// Consomme les lignes ajoutées au fichier de commandes depuis le dernier
    /// passage. Le fichier n'est jamais réécrit par l'app : le script y ajoute,
    /// nous comptons ce qui a déjà été fait.
    private func autoRunCommands(_ auto: AutoPilot) {
        if auto.commandsPath.isEmpty {
            return
        }
        guard let raw = try? String(contentsOfFile: auto.commandsPath, encoding: .utf8) else {
            return  // pas encore créé : rien à faire
        }
        let lines = raw.components(separatedBy: "\n")
        // On ne traite qu'une ligne terminée par un saut de ligne : la dernière
        // peut être en cours d'écriture. Le découpage laisse de toute façon un
        // dernier élément à ignorer — vide si le fichier finit proprement,
        // partiel sinon.
        let complete = lines.count - 1
        while autoCommandsDone < complete {
            let line = lines[autoCommandsDone]
            autoCommandsDone += 1
            guard let cmd = AutoPilot.parse(line) else {
                continue
            }
            run(cmd)
        }
    }

    /// Exécute une commande du pilote. Public pour les tests.
    public func run(_ command: AutoPilot.Command) {
        switch command {
        case .play:
            apply(engine.userControl(now: VSTime.now(), action: .play, positionSec: nil))
        case .pause:
            apply(engine.userControl(now: VSTime.now(), action: .pause, positionSec: nil))
        case .seek(let sec):
            apply(engine.userControl(now: VSTime.now(), action: .seek, positionSec: sec))
        case .ready(let value):
            apply(engine.setReady(value))
        case .chat(let text):
            apply(engine.chat(text))
        case .openFile(let path):
            open(URL(fileURLWithPath: path))
        case .quit:
            // Passe par AppKit : c'est le chemin normal de fermeture, donc
            // celui qui envoie la close 1000 et arrête VLC (VS-028).
            NSApplication.shared.terminate(nil)
        }
        refresh()
    }

    /// Écrit l'état courant, une ligne de JSON, pour que le script puisse
    /// asserter. Écriture par fichier temporaire puis renommage : le lecteur ne
    /// voit jamais de ligne tronquée.
    private func autoWriteStatus(_ now: Nanos) {
        guard let auto = auto, !auto.statusPath.isEmpty else {
            return
        }
        let st = engine.status
        // Écrit par l'écrivain JSON du C (json.c), comme tout ce que
        // l'application produit depuis VS-033 : mêmes échappements, même
        // représentation des flottants que sur le fil.
        let line = Scratch.shared.use { arena -> String in
            var w = JsonWriter()
            jw_init(&w, arena)
            jw_obj_begin(&w)
            jw_kv_i64(&w, "ts", VSTime.toUnixMs(now))
            // Le harnais lance les instances par `open -n`, qui ne rend pas le
            // pid : c'est nous qui le disons.
            jw_kv_i64(&w, "pid", Int64(ProcessInfo.processInfo.processIdentifier))
            AppModel.jwText(&w, "scenario", auto.scenario)
            AppModel.jwText(&w, "name", pseudo)
            AppModel.jwText(&w, "room", room)
            AppModel.jwText(&w, "phase", connected ? "connected" : (wantConnection ? "connecting" : "idle"))
            jw_kv_bool(&w, "connected", connected ? 1 : 0)
            jw_kv_i64(&w, "users", Int64(users.count))
            jw_kv_bool(&w, "ready", ready ? 1 : 0)
            AppModel.jwText(&w, "file", engine.fileName)
            jw_kv_bool(&w, "fileDeclared", engine.fileDeclared ? 1 : 0)
            jw_kv_bool(&w, "vlcRunning", vlcRunning ? 1 : 0)
            AppModel.jwText(&w, "vlcState", engine.haveStatus ? st.state.rawValue : "stopped")
            jw_kv_num(&w, "positionSec", engine.haveStatus ? st.positionSec : 0)
            jw_kv_num(&w, "durationSec", durationSec)
            jw_kv_num(&w, "roomPositionSec", engine.expectedPosition(now))
            jw_kv_bool(&w, "paused", roomPaused ? 1 : 0)
            jw_kv_num(&w, "driftSec", driftSec)
            jw_kv_bool(&w, "buffering", buffering ? 1 : 0)
            jw_kv_i64(&w, "latencyMs", latencyMs)
            AppModel.jwText(&w, "error", formError)
            // Libellés de l'interface : c'est là que se lisent la cause d'un
            // échec de connexion et l'état du lancement de VLC.
            AppModel.jwText(&w, "connection", connectionLabel)
            AppModel.jwText(&w, "lastError", lastError)
            AppModel.jwText(&w, "media", mediaLabel)
            jw_obj_end(&w)
            return coreString(jw_result(&w)) + "\n"
        }
        let tmp = auto.statusPath + ".tmp"
        do {
            try line.write(toFile: tmp, atomically: false, encoding: .utf8)
            _ = try? FileManager.default.removeItem(atPath: auto.statusPath)
            try FileManager.default.moveItem(atPath: tmp, toPath: auto.statusPath)
        } catch {
            // Un état non écrit ne doit pas faire tomber l'application.
            FileHandle.standardError.write(Data("vibesync: état auto non écrit : \(error)\n".utf8))
        }
    }

    /// Paire clé/valeur textuelle de l'écrivain C : la chaîne Swift n'est vue
    /// comme `Str8` que le temps de l'appel.
    private static func jwText(_ w: inout JsonWriter, _ key: String, _ value: String) {
        withStr8(value) { jw_kv_str(&w, key, $0) }
    }

    // MARK: - Boucle

    private func tick() {
        let now = VSTime.now()
        autoPump(now)
        // `isActive` et non `isOpen` : pendant le handshake il n'y a rien à
        // relancer. Relancer toutes les 200 ms tuait la tentative en cours
        // avant qu'elle aboutisse — aucune connexion possible vers un serveur
        // distant.
        if wantConnection && !ws.isActive && conn.shouldAttempt(now) {
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
            // Une seule lecture : chaque accès recopie l'instantané depuis
            // l'état C (nom de fichier compris).
            let st = engine.status
            positionSec = st.positionSec
            durationSec = st.lengthSec > 0 ? st.lengthSec : engine.fileDurationSec
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
        // Notre copie est ouverte : plus rien à proposer, et une recherche de
        // média encore en vol ne doit plus rien lancer.
        invalidateMediaSearch()
        showWatchBanner = false
        showMediaNotice = false

        var size: Int64 = 0
        if let attrs = try? FileManager.default.attributesOfItem(atPath: url.path),
           let n = attrs[FileAttributeKey.size] as? NSNumber {
            size = n.int64Value
        }
        mediaName = url.lastPathComponent
        mediaLabel = "lancement de VLC…"

        VLCLauncher.launch(filePath: url.path, setting: vlcPath) { [weak self] result in
            guard let self = self else {
                return
            }
            switch result {
            case .success(let process):
                self.vlc = process
                self.vlcRunning = true
                self.mediaLabel = "chargement de « \(url.lastPathComponent) »…"
                // Comme la référence Go (Engine.OpenFile) : le fichier n'est
                // déclaré au serveur qu'APRÈS le succès du lancement — sinon on
                // annoncerait un média que personne ne peut encore lire, et un
                // échec laisserait un setFile mensonger dans la salle.
                self.apply(self.engine.openFile(name: url.lastPathComponent, sizeBytes: size))
            case .failure(let err):
                // Rien de déclaré : seul le message d'erreur est remonté.
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
