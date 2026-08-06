// Types.swift — vocabulaire de la FRONTIÈRE avec le moteur.
//
// Entrées : état observé de VLC, état de salle du serveur, pong.
// Sorties : décisions (commandes VLC + messages serveur).
//
// Depuis VS-032 (phase 3 d'ADR-010) le moteur lui-même est le C commun
// (core/src/engine.c) : ces types ne sont plus « ceux du moteur » mais ceux que
// s'échangent le parseur de statut VLC (VLCStatusParser), le protocole
// (Protocol.swift), l'interface (AppModel) et le wrapper CoreEngine, qui les
// convertit en VsStatus/VsRoomState/VsCmd/VsMsg le temps d'un appel. Ils
// restent en Swift parce que ce sont EUX qui traversent le reste de
// l'application ; les convertir partout coûterait plus cher qu'ici.

import Foundation

// MARK: - Entrées

/// État de lecture rapporté par VLC.
public enum PlayState: String {
    case stopped
    case playing
    case paused
}

/// Instantané de VLC (déjà assaini, cf. VLCStatusParser).
public struct VLCStatus {
    public var state: PlayState
    /// Position fine = `position` × `length` (le champ `time` de VLC n'a
    /// qu'une résolution d'une seconde).
    public var positionSec: Double
    public var lengthSec: Double
    public var rate: Double
    public var fileName: String

    public init(state: PlayState = .stopped,
                positionSec: Double = 0,
                lengthSec: Double = 0,
                rate: Double = 1,
                fileName: String = "") {
        self.state = state
        self.positionSec = positionSec
        self.lengthSec = lengthSec
        self.rate = rate
        self.fileName = fileName
    }

    /// Un média est chargé (par opposition à « stopped »).
    public var loaded: Bool {
        return state == .playing || state == .paused
    }
}

/// État autoritatif d'une salle (docs/protocol.md §Modèle).
public struct RoomState {
    public var paused: Bool
    public var positionSec: Double
    public var rate: Double
    public var refServerMs: Int64
    public var setBy: String

    public init(paused: Bool = true,
                positionSec: Double = 0,
                rate: Double = 1,
                refServerMs: Int64 = 0,
                setBy: String = "") {
        self.paused = paused
        self.positionSec = positionSec
        self.rate = rate
        self.refServerMs = refServerMs
        self.setBy = setBy
    }
}

public struct Pong {
    public var t: Int64
    public var serverMs: Int64

    public init(t: Int64, serverMs: Int64) {
        self.t = t
        self.serverMs = serverMs
    }
}

// MARK: - Sorties

/// Commande à appliquer au VLC local.
public struct VLCCommand: Equatable {
    public enum Kind: String {
        case pause
        case resume
        case seek
        case rate
    }

    public var kind: Kind
    /// Secondes pour `seek`, multiplicateur pour `rate`, 0 sinon.
    public var value: Double

    public init(_ kind: Kind, _ value: Double = 0) {
        self.kind = kind
        self.value = value
    }
}

/// Action volontaire de l'utilisateur, remontée au serveur.
public enum ControlAction: String {
    case play
    case pause
    case seek
}

/// Message client → serveur (docs/protocol.md §Messages client → serveur).
public enum ClientMessage {
    case ping(t: Int64)
    case setReady(ready: Bool)
    case setFile(name: String, durationSec: Double, sizeBytes: Int64)
    case control(action: ControlAction, positionSec: Double)
    case report(positionSec: Double, paused: Bool, buffering: Bool)
    case chat(text: String)

    public var type: String {
        switch self {
        case .ping: return "ping"
        case .setReady: return "setReady"
        case .setFile: return "setFile"
        case .control: return "control"
        case .report: return "report"
        case .chat: return "chat"
        }
    }
}

/// Une décision du moteur : soit une commande au lecteur, soit un message au
/// serveur. L'ordre relatif à l'intérieur de chaque famille est significatif
/// (c'est ce que comparent les vecteurs).
public enum Decision {
    case vlc(VLCCommand)
    case server(ClientMessage)
}

extension Array where Element == Decision {

    public var vlcCommands: [VLCCommand] {
        var out: [VLCCommand] = []
        for d in self {
            if case .vlc(let c) = d {
                out.append(c)
            }
        }
        return out
    }

    public var serverMessages: [ClientMessage] {
        var out: [ClientMessage] = []
        for d in self {
            if case .server(let m) = d {
                out.append(m)
            }
        }
        return out
    }
}
