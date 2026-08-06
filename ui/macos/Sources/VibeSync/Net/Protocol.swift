// Protocol.swift — encodage/décodage de docs/protocol.md (v1).
//
// Aucune E/S : des chaînes en entrée, des chaînes en sortie. Le décodage est
// tolérant (un type inconnu est ignoré, pas fatal) et assainit tout ce qui
// entre dans le moteur.

import Foundation
import Security

public let protocolVersion: Int64 = 1
/// Jeton de reprise de session : 16 octets aléatoires en hexadécimal.
public let sessionTokenBytes: Int = 16
public let sessionTokenLength: Int = sessionTokenBytes * 2

// MARK: - Serveur → client

public struct ServerUser {
    public var id: String = ""
    public var name: String = ""
    public var ready: Bool = false
    public var positionSec: Double = 0
    public var latencyMs: Int64 = 0
    public var hasFile: Bool = false
    public var fileName: String = ""
    public var fileDurationSec: Double = 0
    public var fileSizeBytes: Int64 = 0

    public init() {}
}

public enum ServerMessage {
    case welcome(selfId: String, room: String, state: RoomState?, users: [ServerUser], selfReady: Bool?)
    case pong(Pong)
    case roomState(RoomState)
    case users([ServerUser])
    case chatEvent(from: String, text: String, serverMs: Int64)
    case toast(level: String, text: String)
    case error(code: String, text: String)
    /// Forward-compat : type inconnu, à ignorer sans fermer la connexion.
    case unknown(type: String)
}

public enum Proto {

    // MARK: Client → serveur

    /// Premier message de la session. `password` et `session` sont omis s'ils
    /// sont vides.
    public static func encodeHello(name: String,
                                   room: String,
                                   password: String,
                                   session: String) -> String {
        var data: [(String, JSONVal)] = [
            ("version", .int(protocolVersion)),
            ("name", .str(name)),
            ("room", .str(room)),
        ]
        if !password.isEmpty {
            data.append(("password", .str(password)))
        }
        if !session.isEmpty {
            data.append(("session", .str(session)))
        }
        return envelope("hello", data)
    }

    /// Encode une décision du moteur.
    public static func encode(_ m: ClientMessage) -> String {
        switch m {
        case .ping(let t):
            return envelope("ping", [("t", .int(t))])
        case .setReady(let ready):
            return envelope("setReady", [("ready", .bool(ready))])
        case .setFile(let name, let durationSec, let sizeBytes):
            return envelope("setFile", [
                ("name", .str(name)),
                ("durationSec", .num(durationSec)),
                ("sizeBytes", .int(sizeBytes)),
            ])
        case .control(let action, let positionSec):
            return envelope("control", [
                ("action", .str(action.rawValue)),
                ("positionSec", .num(positionSec)),
            ])
        case .report(let positionSec, let paused, let buffering):
            return envelope("report", [
                ("positionSec", .num(positionSec)),
                ("paused", .bool(paused)),
                ("buffering", .bool(buffering)),
            ])
        case .chat(let text):
            return envelope("chat", [("text", .str(text))])
        }
    }

    private static func envelope(_ type: String, _ data: [(String, JSONVal)]) -> String {
        let root = JSONVal.obj([
            ("type", .str(type)),
            ("data", .obj(data)),
        ])
        return root.encoded
    }

    // MARK: Serveur → client

    /// Analyse une enveloppe {type, data}. `nil` si le message est illisible
    /// (JSON invalide, enveloppe sans type).
    public static func decode(_ raw: String) -> ServerMessage? {
        guard let root = JSON.object(JSON.parse(raw)) else {
            return nil
        }
        guard let type = root["type"] as? String, !type.isEmpty else {
            return nil
        }
        return fill(type: type, data: root["data"] as? [String: Any])
    }

    /// Construit un message à partir d'un type et d'un nœud `data` déjà
    /// analysé (utilisé par le rejeu des vecteurs, qui portent les mêmes
    /// données sans l'enveloppe).
    public static func fill(type: String, data: [String: Any]?) -> ServerMessage {
        switch type {
        case "welcome":
            let selfId = JSON.string(data, "selfId")
            let room = JSON.string(data, "room")
            var state: RoomState? = nil
            if let st = JSON.child(data, "state") {
                state = readRoomState(st)
            }
            let users = readUsers(JSON.array(data, "users"))
            var selfReady: Bool? = nil
            if !selfId.isEmpty {
                for u in users where u.id == selfId {
                    selfReady = u.ready
                    break
                }
            }
            return .welcome(selfId: selfId, room: room, state: state, users: users, selfReady: selfReady)

        case "pong":
            return .pong(Pong(t: JSON.int(data, "t"), serverMs: JSON.int(data, "serverMs")))

        case "roomState":
            guard let d = data else {
                return .roomState(RoomState(paused: false, positionSec: 0, rate: 0, refServerMs: 0, setBy: ""))
            }
            return .roomState(readRoomState(d))

        case "users":
            return .users(readUsers(JSON.array(data, "users")))

        case "chatEvent", "chat":
            return .chatEvent(from: JSON.string(data, "from"),
                              text: JSON.string(data, "text"),
                              serverMs: JSON.int(data, "serverMs"))

        case "toast":
            return .toast(level: JSON.string(data, "level", "info"), text: JSON.string(data, "text"))

        case "error":
            return .error(code: JSON.string(data, "code"), text: JSON.string(data, "text"))

        default:
            return .unknown(type: type)
        }
    }

    private static func readRoomState(_ o: [String: Any]) -> RoomState {
        return RoomState(paused: JSON.bool(o, "paused"),
                         positionSec: JSON.number(o, "positionSec"),
                         rate: JSON.number(o, "rate"),
                         refServerMs: JSON.int(o, "refServerMs"),
                         setBy: JSON.string(o, "setBy"))
    }

    private static let maxUsers = 64

    private static func readUsers(_ raw: [Any]) -> [ServerUser] {
        var out: [ServerUser] = []
        for item in raw {
            if out.count >= maxUsers {
                break
            }
            guard let o = item as? [String: Any] else {
                continue
            }
            var u = ServerUser()
            u.id = JSON.string(o, "id")
            u.name = JSON.string(o, "name")
            u.ready = JSON.bool(o, "ready")
            u.positionSec = JSON.number(o, "positionSec")
            u.latencyMs = JSON.int(o, "latencyMs")
            if let f = JSON.child(o, "file") {
                u.hasFile = true
                u.fileName = JSON.string(f, "name")
                u.fileDurationSec = JSON.number(f, "durationSec")
                u.fileSizeBytes = JSON.int(f, "sizeBytes")
            }
            out.append(u)
        }
        return out
    }

    /// Un code d'erreur fatal interdit de réessayer.
    public static func isFatal(_ code: String) -> Bool {
        return code == "version_mismatch" || code == "bad_password" || code == "name_taken"
    }

    // MARK: Jeton de session

    /// 16 octets du générateur du système, en hexadécimal minuscule. Stable
    /// pour toute la vie du processus (docs/protocol.md §Reprise de session).
    public static func sessionToken() -> String {
        var bytes = [UInt8](repeating: 0, count: sessionTokenBytes)
        let status = SecRandomCopyBytes(kSecRandomDefault, bytes.count, &bytes)
        if status != errSecSuccess {
            // Repli : le jeton n'est pas un secret d'authentification, il sert
            // à reconnaître notre propre session zombie.
            for i in 0..<bytes.count {
                bytes[i] = UInt8.random(in: 0...255)
            }
        }
        return hex(bytes)
    }

    public static func hex(_ bytes: [UInt8]) -> String {
        let digits = Array("0123456789abcdef".unicodeScalars)
        var out = String.UnicodeScalarView()
        out.reserveCapacity(bytes.count * 2)
        for b in bytes {
            out.append(digits[Int(b >> 4)])
            out.append(digits[Int(b & 0x0f)])
        }
        return String(out)
    }
}
