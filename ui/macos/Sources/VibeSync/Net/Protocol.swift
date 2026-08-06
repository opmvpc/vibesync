// Protocol.swift — frontière Swift ↔ protocole C commun (ADR-010, phase 4).
//
// L'encodage et le décodage de docs/protocol.md n'existent plus qu'en un
// exemplaire pour les deux clients natifs : `core/src/protocol.c`, au-dessus du
// parseur `core/src/json.c`. Ce fichier ne décide de RIEN — il traduit le
// vocabulaire Swift (ClientMessage, Welcome, RoomState…) en appels C et
// retraduit ce qui en sort. Toute règle de lecture qui se trouverait ici serait
// un bug de conception : elle divergerait du client Windows le jour même.
//
// Ce que le C apporte et que le décodage Swift n'avait pas (docs/protocol.md
// §forward-compat) : un message dont un champ OBLIGATOIRE manque ou est mal
// typé est INVALIDE, donc ignoré, au lieu d'être rempli de zéros silencieux —
// un `pong` vide devenait {t:0, serverMs:0} et empoisonnait l'offset d'horloge.
// C'est déjà ce que fait `on_server_message` dans ui/win32/src/main.c.

import Foundation
import VSCore

public let protocolVersion: Int64 = Int64(VS_PROTOCOL_VERSION)
/// Jeton de reprise de session : 16 octets aléatoires en hexadécimal.
public let sessionTokenBytes: Int = Int(VS_SESSION_TOKEN_BYTES)
public let sessionTokenLength: Int = Int(VS_SESSION_TOKEN_LEN)

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

/// Contenu du `welcome` (docs/protocol.md §Messages serveur → client). Un
/// struct plutôt qu'un n-uplet : le message a grossi avec VS-023 et chaque
/// champ mérite son nom.
public struct Welcome {
    public var selfId: String = ""
    public var room: String = ""
    /// Optionnel côté Swift par héritage ; en pratique le C n'accepte pas un
    /// welcome sans état de salle recevable (il l'invalide), donc ce champ est
    /// toujours renseigné quand un welcome arrive jusqu'ici.
    public var state: RoomState? = nil
    public var users: [ServerUser] = []
    /// Ready que le serveur nous attribue. Le moteur ne l'adopte PAS au welcome
    /// (c'est notre état local qui fait foi au re-join) : ce champ n'est là que
    /// pour l'affichage et le diagnostic.
    public var selfReady: Bool? = nil
    /// VS-023 : version applicative du serveur et lien de téléchargement.
    /// Champs additifs, absents des serveurs plus anciens.
    public var serverVersion: String = ""
    public var downloadUrl: String = ""

    public init() {}
}

public enum ServerMessage {
    case welcome(Welcome)
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
    /// sont vides (proto_encode_hello).
    public static func encodeHello(name: String,
                                   room: String,
                                   password: String,
                                   session: String) -> String {
        return Scratch.shared.use { arena in
            withStr8(name) { n in
                withStr8(room) { r in
                    withStr8(password) { p in
                        withStr8(session) { s in
                            coreString(proto_encode_hello(arena, n, r, p, s))
                        }
                    }
                }
            }
        }
    }

    /// Encode une décision du moteur (proto_encode_msg).
    public static func encode(_ m: ClientMessage) -> String {
        var msg = VsMsg()
        switch m {
        case .ping(let t):
            msg.kind = VS_MSG_PING
            msg.t = t
        case .setReady(let ready):
            msg.kind = VS_MSG_SET_READY
            msg.ready = ready ? 1 : 0
        case .setFile(let name, let durationSec, let sizeBytes):
            msg.kind = VS_MSG_SET_FILE
            withStr8(name) { strbuf_set(&msg.name, $0) }
            msg.duration_sec = durationSec
            msg.size_bytes = sizeBytes
        case .control(let action, let positionSec):
            msg.kind = VS_MSG_CONTROL
            msg.action = Proto.vsAction(action)
            msg.position_sec = positionSec
        case .report(let positionSec, let paused, let buffering):
            msg.kind = VS_MSG_REPORT
            msg.position_sec = positionSec
            msg.paused = paused ? 1 : 0
            msg.buffering = buffering ? 1 : 0
        case .chat(let text):
            msg.kind = VS_MSG_CHAT
            withStr8(text) { strbuf_set(&msg.text, $0) }
        }
        return Scratch.shared.use { arena in
            coreString(proto_encode_msg(arena, &msg))
        }
    }

    private static func vsAction(_ a: ControlAction) -> VsAction {
        switch a {
        case .play: return VS_ACT_PLAY
        case .pause: return VS_ACT_PAUSE
        case .seek: return VS_ACT_SEEK
        }
    }

    // MARK: Serveur → client

    /// Analyse une enveloppe {type, data}. `nil` si le message est illisible
    /// (JSON invalide, enveloppe sans type) **ou invalide** (type connu mais
    /// champ obligatoire absent ou mal typé) : dans les deux cas il est ignoré
    /// sans fermer la connexion, comme le fait le client Windows.
    public static func decode(_ raw: String) -> ServerMessage? {
        return Scratch.shared.use { arena -> ServerMessage? in
            withStr8(raw) { text -> ServerMessage? in
                guard let m = proto_decode(arena, text), m.pointee.invalid == 0 else {
                    return nil
                }
                // Tout est recopié ici : les Str8 du message pointent dans
                // l'arène, qui est rendue à la sortie de `use`.
                return message(m.pointee)
            }
        }
    }

    private static func message(_ m: VsInMsg) -> ServerMessage {
        switch m.kind {
        case VS_IN_WELCOME:
            var w = Welcome()
            w.selfId = coreString(m.self_id)
            w.room = coreString(m.room)
            if m.have_state != 0 {
                w.state = roomState(m.state)
            }
            w.users = users(m)
            if m.have_self_ready != 0 {
                w.selfReady = m.self_ready != 0
            }
            w.serverVersion = coreString(m.server_version)
            w.downloadUrl = coreString(m.download_url)
            return .welcome(w)

        case VS_IN_PONG:
            return .pong(Pong(t: m.pong.t, serverMs: m.pong.server_ms))

        case VS_IN_ROOMSTATE:
            return .roomState(roomState(m.state))

        case VS_IN_USERS:
            return .users(users(m))

        case VS_IN_CHATEVENT:
            return .chatEvent(from: coreString(m.from),
                              text: coreString(m.text),
                              serverMs: m.server_ms)

        case VS_IN_TOAST:
            return .toast(level: coreString(m.level), text: coreString(m.text))

        case VS_IN_ERROR:
            return .error(code: coreString(m.code), text: coreString(m.text))

        default:
            return .unknown(type: coreString(m.type))
        }
    }

    private static func roomState(_ rs: VsRoomState) -> RoomState {
        return RoomState(paused: rs.paused != 0,
                         positionSec: rs.position_sec,
                         rate: rs.rate,
                         refServerMs: rs.ref_server_ms,
                         setBy: coreString(rs.set_by))
    }

    private static func users(_ m: VsInMsg) -> [ServerUser] {
        guard let raw = m.users, m.user_count > 0 else {
            return []
        }
        var out: [ServerUser] = []
        out.reserveCapacity(Int(m.user_count))
        for index in 0..<Int(m.user_count) {
            let u = raw[index]
            var user = ServerUser()
            user.id = coreString(u.id)
            user.name = coreString(u.name)
            user.ready = u.ready != 0
            user.positionSec = u.position_sec
            user.latencyMs = u.latency_ms
            user.hasFile = u.has_file != 0
            user.fileName = coreString(u.file_name)
            user.fileDurationSec = u.file_duration_sec
            user.fileSizeBytes = u.file_size_bytes
            out.append(user)
        }
        return out
    }

    /// Un code d'erreur fatal interdit de réessayer (proto_error_is_fatal).
    public static func isFatal(_ code: String) -> Bool {
        return withStr8(code) { proto_error_is_fatal($0) != 0 }
    }

    // MARK: Jeton de session

    /// 16 octets du générateur du système, en hexadécimal minuscule
    /// (proto_session_token → vs_random_bytes → arc4random_buf).
    public static func sessionToken() -> String {
        var buffer = [CChar](repeating: 0, count: sessionTokenLength + 1)
        let ok = buffer.withUnsafeMutableBufferPointer { raw in
            proto_session_token(raw.baseAddress, Int(raw.count))
        }
        if ok == 0 {
            // Le générateur du système n'échoue pas en pratique ; s'il le
            // faisait, un jeton non aléatoire vaut mieux qu'un jeton vide (il
            // ne sert qu'à reconnaître notre propre session zombie).
            var bytes = [UInt8](repeating: 0, count: sessionTokenBytes)
            for i in 0..<bytes.count {
                bytes[i] = UInt8.random(in: 0...255)
            }
            return hex(bytes)
        }
        return String(cString: buffer)
    }

    /// Forme attendue d'un jeton relu des réglages (proto_session_token_valid).
    public static func isValidSessionToken(_ token: String) -> Bool {
        return withStr8(token) { proto_session_token_valid($0) != 0 }
    }

    /// Hexadécimal minuscule (proto_hex).
    public static func hex(_ bytes: [UInt8]) -> String {
        if bytes.isEmpty {
            return ""
        }
        var out = [CChar](repeating: 0, count: bytes.count * 2 + 1)
        out.withUnsafeMutableBufferPointer { raw in
            proto_hex(bytes, bytes.count, raw.baseAddress)
        }
        return String(cString: out)
    }
}
