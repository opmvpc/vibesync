// VLCLauncher.swift — localisation et lancement de VLC.
//
// Mêmes drapeaux que le driver Go (internal/vlc/launch.go) : interface HTTP sur
// 127.0.0.1, port et mot de passe aléatoires, pas d'instance unique.

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
        _ = fm
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
        process.arguments = [
            "--extraintf=http",
            "--http-host=127.0.0.1",
            "--http-port=\(port)",
            "--http-password=" + password,
            "--no-video-title-show",
            "--no-one-instance",
            filePath,
        ]
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
            switch result {
            case .success:
                completion(.success(handle))
            case .failure(let err):
                handle.terminate()
                completion(.failure(err))
            }
        }
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
