// VLCLauncher.swift — localisation et lancement de VLC.
//
// Drapeaux : la liste `darwin` du driver Go (internal/vlc/launch.go,
// `launchArgs`), c'est-à-dire les 15 drapeaux du blindage VS-029 MOINS la
// famille « instance unique » que le VLC macOS ne connaît pas. Voir
// `VLCLauncher.launchArgs` ci-dessous.

import Darwin
import Foundation

public final class VLCProcess {
    public let process: Process
    public let client: VLCClient
    /// À vrai, la fermeture du client ne tue pas VLC.
    public var keepAlive: Bool = false

    init(process: Process, client: VLCClient) {
        self.process = process
        self.client = client
    }

    public var isRunning: Bool {
        return process.isRunning
    }

    public func terminate() {
        if keepAlive {
            return
        }
        if process.isRunning {
            process.terminate()
        }
    }
}

/// Ce que l'interface a le droit de dire du chemin de VLC — l'équivalent du
/// `settings_vlc_state` du client Windows (ui/win32/src/ui.c). Fonction pure du
/// couple (réglage, disque) : c'est `VLCLauncher.pathStatus` qui le calcule, la
/// vue ne fait que le peindre.
public enum VLCPathStatus: Equatable {
    /// Réglage vide, détection automatique réussie (le binaire retenu).
    case detected(String)
    /// Réglage vide et aucun VLC trouvé sur la machine.
    case undetected
    /// Réglage renseigné et exécutable (le binaire réellement lancé — pour un
    /// bundle, celui qui est dedans).
    case configured(String)
    /// Réglage renseigné mais rien d'exécutable au bout : il sera IGNORÉ.
    case invalid

    public enum Severity {
        case ok
        case info
        case warn
        case error
    }

    public var severity: Severity {
        switch self {
        case .detected:
            return .info
        case .undetected:
            return .warn
        case .configured:
            return .ok
        case .invalid:
            return .error
        }
    }

    public var text: String {
        switch self {
        case .detected(let path):
            return "VLC détecté : \(path)"
        case .undetected:
            return "Aucun VLC détecté sur cette machine : indiquez le chemin de VLC.app."
        case .configured(let path):
            return "VLC trouvé à ce chemin : \(path)"
        case .invalid:
            return "VLC introuvable à ce chemin — corrigez-le, ou videz le champ "
                 + "pour revenir à la détection automatique."
        }
    }
}

public enum VLCLauncher {

    /// Variable d'environnement prioritaire (même nom que la référence Go).
    public static let envBinary = "VIBESYNC_VLC"

    /// Prédicat « ce chemin est un fichier exécutable ». Injecté dans les
    /// fonctions de résolution pour que les tests n'aient pas besoin de disque.
    public typealias ExistsFn = (String) -> Bool

    // MARK: - Résolution du binaire
    //
    // ORDRE, identique au client Windows (ui/win32/src/main.c, apply_vlc_path) :
    //
    //   1. le réglage de l'utilisateur, s'il n'est pas vide ET s'il mène à un
    //      exécutable ;
    //   2. %VIBESYNC_VLC% / $VIBESYNC_VLC ;
    //   3. les emplacements standards, puis le PATH.
    //
    // Le réglage passe donc DEVANT l'environnement. Sur Windows le même ordre
    // est obtenu autrement (le réglage écrase la variable au démarrage, un
    // réglage vide restituant la valeur héritée) ; ici rien n'est écrit dans
    // l'environnement du processus, le réglage est simplement passé à `launch`.
    //
    // Conséquence pour le harnais de test réel (scripts/run-real-macos.sh) : il
    // isole chaque instance par VIBESYNC_SUITE, donc leur réglage est vide et
    // $VIBESYNC_VLC continue de commander. Vérifié : une UserDefaults ouverte
    // sur une suite lit la valeur de la suite quand elle existe, et NON celle
    // du domaine applicatif normal quand la suite est muette.

    /// Binaire à lancer, tous critères confondus. `nil` : aucun VLC.
    public static func binary(setting: String,
                              env: [String: String] = ProcessInfo.processInfo.environment,
                              exists: ExistsFn = isExecutableFile) -> String? {
        // Un réglage invalide n'est pas fatal : on le laisse tomber et on
        // retombe sur la détection (l'interface, elle, le signale en rouge).
        if let configured = settingBinary(setting, exists: exists) {
            return configured
        }
        return locate(env: env, exists: exists)
    }

    /// État à afficher sous le champ des Réglages, pour le réglage donné.
    public static func pathStatus(setting: String,
                                  env: [String: String] = ProcessInfo.processInfo.environment,
                                  exists: ExistsFn = isExecutableFile) -> VLCPathStatus {
        if setting.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            if let found = locate(env: env, exists: exists) {
                return .detected(found)
            }
            return .undetected
        }
        if let configured = settingBinary(setting, exists: exists) {
            return .configured(configured)
        }
        return .invalid
    }

    /// Binaire correspondant au réglage seul. `nil` si le réglage est vide, ou
    /// s'il ne mène à aucun exécutable.
    public static func settingBinary(_ setting: String, exists: ExistsFn = isExecutableFile) -> String? {
        let path = normalize(setting)
        if path.isEmpty {
            return nil
        }
        let candidate = resolveBundle(path)
        if exists(candidate) {
            return candidate
        }
        // Un bundle renommé (« VLC 3.app ») ne porte plus le nom de son
        // exécutable : on demande alors au bundle lui-même. Purement de
        // rattrapage — le chemin nominal ci-dessus n'a pas besoin du disque.
        if path.lowercased().hasSuffix(".app"),
           let inside = Bundle(path: path)?.executableURL?.path,
           exists(inside) {
            return inside
        }
        return nil
    }

    /// Rogne, développe le `~` et retire les barres obliques finales — ce que
    /// donne un copier-coller ou un glisser-déposer depuis le Finder.
    public static func normalize(_ setting: String) -> String {
        var path = setting.trimmingCharacters(in: .whitespacesAndNewlines)
        if path.isEmpty {
            return ""
        }
        path = (path as NSString).expandingTildeInPath
        while path.count > 1 && path.hasSuffix("/") {
            path.removeLast()
        }
        return path
    }

    /// Chemin nominal de l'exécutable d'un bundle : `…/VLC.app` →
    /// `…/VLC.app/Contents/MacOS/VLC`. Tout le reste est rendu tel quel — un
    /// binaire nu (`/opt/homebrew/bin/vlc`) est déjà ce qu'on veut exécuter.
    /// Fonction pure : aucun accès disque, gelée par les tests.
    public static func resolveBundle(_ path: String) -> String {
        guard path.lowercased().hasSuffix(".app") else {
            return path
        }
        let name = (path as NSString).lastPathComponent
        let base = String(name.dropLast(4))  // « VLC.app » → « VLC »
        if base.isEmpty {
            return path
        }
        return path + "/Contents/MacOS/" + base
    }

    /// Détection automatique : l'environnement d'abord, puis les emplacements
    /// standards. Ne connaît PAS le réglage — c'est ce qui permet à l'interface
    /// d'afficher ce que donnerait un champ vide sans toucher au champ.
    public static func locate(env: [String: String] = ProcessInfo.processInfo.environment,
                              exists: ExistsFn = isExecutableFile) -> String? {
        let forced = env[envBinary] ?? ""
        if !forced.isEmpty {
            // Une variable qui pointe dans le vide reste une consigne
            // explicite : on ne va pas chercher ailleurs derrière son dos.
            let candidate = resolveBundle(normalize(forced))
            return exists(candidate) ? candidate : nil
        }
        let home = NSHomeDirectory()
        let candidates = [
            "/Applications/VLC.app/Contents/MacOS/VLC",
            home + "/Applications/VLC.app/Contents/MacOS/VLC",
            "/Applications/VLC/VLC.app/Contents/MacOS/VLC",
            "/opt/homebrew/bin/vlc",
            "/usr/local/bin/vlc",
        ]
        for path in candidates where exists(path) {
            return path
        }
        // Dernier recours : le PATH.
        for dir in (env["PATH"] ?? "").split(separator: ":") {
            let path = String(dir) + "/vlc"
            if exists(path) {
                return path
            }
        }
        return nil
    }

    /// Fichier régulier ET exécutable par nous. Le bit d'exécution compte :
    /// sans lui `Process.run()` échouerait plus loin, avec un message bien plus
    /// obscur qu'un « VLC introuvable à ce chemin » dans les Réglages.
    public static func isExecutableFile(_ path: String) -> Bool {
        var isDir: ObjCBool = false
        let fm = FileManager.default
        guard fm.fileExists(atPath: path, isDirectory: &isDir), !isDir.boolValue else {
            return false
        }
        return fm.isExecutableFile(atPath: path)
    }

    /// Lance VLC sur `filePath` et attend que son interface HTTP réponde.
    /// Le rappel arrive sur la file principale.
    ///
    /// `setting` est le chemin réglé dans les Réglages (vide = détection
    /// automatique) ; il est passé explicitement plutôt que relu ici, pour que
    /// le lanceur reste sans état.
    public static func launch(filePath: String,
                              setting: String = "",
                              timeoutSec: Double = 20,
                              completion: @escaping (Result<VLCProcess, VLCError>) -> Void) {
        guard let binary = binary(setting: setting) else {
            DispatchQueue.main.async { completion(.failure(.notFound)) }
            return
        }
        let port = freePort() ?? Int.random(in: 42000...52000)
        let password = randomPassword()

        let process = Process()
        process.executableURL = URL(fileURLWithPath: binary)
        // Le média vient en DERNIER, après toutes les options : VLC prendrait
        // le reste pour des MRL.
        process.arguments = launchArgs(port: port, password: password) + [filePath]
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
        do {
            try process.run()
        } catch {
            DispatchQueue.main.async { completion(.failure(.spawn(error.localizedDescription))) }
            return
        }

        let client = VLCClient(port: port, password: password)
        let handle = VLCProcess(process: process, client: client)
        client.waitReady(timeoutSec: timeoutSec) { result in
            if case .failure(let err) = result {
                handle.terminate()
                completion(.failure(err))
                return
            }
            // §Chargement de fichier : VLC démarre la lecture tout seul à
            // l'ouverture. On ne rend la main qu'une fois la pause à la
            // position 0 OBSERVÉE (vlc.Prepare côté Go) — et l'appelant ne
            // déclare le fichier au serveur qu'après ce succès. Un échec (VLC
            // qui refuse pause/seek, média jamais chargé) est propagé : rien
            // n'est déclaré.
            client.prepare { prepared in
                switch prepared {
                case .success:
                    completion(.success(handle))
                case .failure(let err):
                    handle.terminate()
                    completion(.failure(err))
                }
            }
        }
    }

    /// Les drapeaux de lancement, sans le média — TOUT ce dont on dépend est
    /// forcé explicitement (VS-029). Fonction pure : gelée par
    /// `VLCLaunchArgsTests`, sur le modèle de `launchArgs` côté Go
    /// (internal/vlc/launch.go) et du bloc `vlc_build_command` du harnais C
    /// (core/tests/test_core.c). Les trois listes doivent rester identiques.
    ///
    /// Le raisonnement complet est dans `vlc_build_command` (core/src/vlc_core.c).
    /// En résumé : le vlcrc de l'utilisateur gagne sur les défauts de VLC,
    /// jamais sur la ligne de commande, et un VLC configuré par Syncplay
    /// faisait échouer l'attache HTTP en laissant un VLC orphelin en lecture.
    /// Chaque drapeau neutralise un réglage qui peut venir du vlcrc :
    ///
    ///   --extraintf=http     l'interface de pilotage ; sur la ligne de commande
    ///                        elle REMPLACE l'`extraintf` du vlcrc.
    ///   --lua-intf=http      filet si le vlcrc a fait de luaintf l'interface
    ///                        PRINCIPALE : au moins c'est notre script http qui
    ///                        s'exécute, pas syncplay.lua.
    ///   --playlist-autostart sinon rien ne démarre, le statut reste
    ///                        « stopped » et `prepare` tourne dans le vide.
    ///   --start-paused       accepté mais INOPÉRANT sur macOS (VLC 3.0.23
    ///                        démarre quand même la lecture, l'interface
    ///                        `macosx` s'en charge de son côté). Gardé pour
    ///                        l'alignement des trois implémentations ; ici
    ///                        c'est `prepare` qui tranche, seule autorité
    ///                        (docs/protocol.md §Chargement de fichier).
    ///   --no-random --no-loop --no-repeat  le moteur de sync raisonne sur un
    ///                        média unique joué une fois.
    ///   --no-play-and-exit   VLC ne doit pas disparaître en fin de média.
    ///   --no-video-title-show  confort, déjà là avant VS-029.
    ///
    /// ABSENTE : la famille « instance unique » (`--no-one-instance`,
    /// `--no-one-instance-when-started-from-file`, `--no-playlist-enqueue`).
    /// libvlc-module.c ne la déclare que sous Windows (ou Linux avec D-Bus) ;
    /// le VLC macOS refuse de démarrer sur chacun des trois — « unknown option
    /// or missing mandatory argument », vérifié un par un sur VLC 3.0.23. Elle
    /// n'y sert de toute façon à rien : lancer deux fois le binaire du bundle
    /// donne bien deux processus indépendants, chacun avec son interface HTTP
    /// (le harnais de test réel en fait tourner deux).
    ///
    /// ABSENT aussi : `--intf=<module>`. Forcer l'interface principale
    /// obligerait à parier sur son nom (qt/qt4/macosx selon version et OS) et
    /// un nom inconnu empêche VLC de démarrer.
    static func launchArgs(port: Int, password: String) -> [String] {
        return [
            "--extraintf=http",
            "--lua-intf=http",
            "--http-host=127.0.0.1",
            "--http-port=\(port)",
            "--http-password=" + password,
            "--playlist-autostart",
            "--start-paused",
            "--no-random",
            "--no-loop",
            "--no-repeat",
            "--no-play-and-exit",
            "--no-video-title-show",
        ]
    }

    private static func randomPassword() -> String {
        var bytes = [UInt8](repeating: 0, count: 16)
        for i in 0..<bytes.count {
            bytes[i] = UInt8.random(in: 0...255)
        }
        return Proto.hex(bytes)
    }

    /// Réserve puis relâche un port libre sur la loopback (comme freePort()
    /// côté Go). `nil` si l'OS refuse : l'appelant tire alors au hasard.
    static func freePort() -> Int? {
        let fd = Darwin.socket(AF_INET, SOCK_STREAM, 0)
        if fd < 0 {
            return nil
        }
        defer { Darwin.close(fd) }

        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = 0
        addr.sin_addr.s_addr = inet_addr("127.0.0.1")

        var bound: Int32 = -1
        withUnsafePointer(to: &addr) { raw in
            raw.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                bound = Darwin.bind(fd, sa, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        if bound != 0 {
            return nil
        }

        var out = sockaddr_in()
        var length = socklen_t(MemoryLayout<sockaddr_in>.size)
        var named: Int32 = -1
        withUnsafeMutablePointer(to: &out) { raw in
            raw.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                named = Darwin.getsockname(fd, sa, &length)
            }
        }
        if named != 0 {
            return nil
        }
        let port = Int(UInt16(bigEndian: out.sin_port))
        return port > 0 ? port : nil
    }
}
