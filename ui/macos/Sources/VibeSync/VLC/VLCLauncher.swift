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

public enum VLCLauncher {

    /// Variable d'environnement prioritaire (même nom que la référence Go).
    public static let envBinary = "VIBESYNC_VLC"

    public static func locate() -> String? {
        let forced = ProcessInfo.processInfo.environment[envBinary] ?? ""
        if !forced.isEmpty {
            return isExecutableFile(forced) ? forced : nil
        }
        let home = NSHomeDirectory()
        let candidates = [
            "/Applications/VLC.app/Contents/MacOS/VLC",
            home + "/Applications/VLC.app/Contents/MacOS/VLC",
            "/Applications/VLC/VLC.app/Contents/MacOS/VLC",
            "/opt/homebrew/bin/vlc",
            "/usr/local/bin/vlc",
        ]
        for path in candidates where isExecutableFile(path) {
            return path
        }
        // Dernier recours : le PATH.
        for dir in (ProcessInfo.processInfo.environment["PATH"] ?? "").split(separator: ":") {
            let path = String(dir) + "/vlc"
            if isExecutableFile(path) {
                return path
            }
        }
        return nil
    }

    private static func isExecutableFile(_ path: String) -> Bool {
        var isDir: ObjCBool = false
        let exists = FileManager.default.fileExists(atPath: path, isDirectory: &isDir)
        return exists && !isDir.boolValue
    }

    /// Lance VLC sur `filePath` et attend que son interface HTTP réponde.
    /// Le rappel arrive sur la file principale.
    public static func launch(filePath: String,
                              timeoutSec: Double = 20,
                              completion: @escaping (Result<VLCProcess, VLCError>) -> Void) {
        guard let binary = locate() else {
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
