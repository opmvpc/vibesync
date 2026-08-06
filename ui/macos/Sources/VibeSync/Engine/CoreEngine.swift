// CoreEngine.swift — frontière Swift ↔ moteur C commun (ADR-010, phase 3).
//
// La machine à états de synchronisation n'existe plus qu'une fois par produit
// natif : c'est `core/src/engine.c`, partagée avec le client Windows et gelée
// par test/vectors/*.json. Ce fichier ne décide de RIEN — il traduit le
// vocabulaire Swift (VLCStatus, RoomState, Decision…) en appels C et retraduit
// les sorties. Toute règle de comportement qui se trouverait ici serait un bug
// de conception : elle divergerait de Windows le jour même.
//
// Trois règles d'interop, tenues par la totalité du fichier :
//
//   1. Aucun pointeur C ne survit à l'appel. Une chaîne Swift n'est vue comme
//      `Str8` que le temps d'un appel (`withStr8`), et les `StrBuf` d'état du
//      moteur sont recopiés en `String` à la lecture (`string(_:)`). Le C ne
//      conserve jamais une adresse qui nous appartient : `strbuf_set` copie.
//   2. Tout l'état tient dans le `VsEngine` détenu par cette classe — tampons
//      bornés, aucune allocation, aucune durée de vie à gérer.
//   3. Les appels sont sérialisés par la file principale, exactement comme
//      l'était le `struct Engine` qu'il remplace (AppModel n'appelle le moteur
//      que depuis le timer et les rappels réseau/VLC, tous rendus sur main).

import Foundation
import VSCore

// MARK: - Temps
//
// L'arithmétique du temps vient elle aussi du C (base_core.c / base_posix.c) :
// nanosecondes depuis l'epoch Unix, division plancher pour les millisecondes,
// et la même conversion en secondes que time.Duration.Seconds() en Go. C'est
// ce qui permet de reproduire les vecteurs au bit près.

/// Instant ou durée en nanosecondes.
public typealias Nanos = Int64

public enum VSTime {

    /// Horloge murale (clock_gettime(CLOCK_REALTIME) côté C).
    public static func now() -> Nanos {
        return vs_now_ns()
    }

    /// Millisecondes epoch (division plancher).
    public static func toUnixMs(_ ns: Nanos) -> Int64 {
        return vs_ns_to_unix_ms(ns)
    }

    public static func fromUnixMs(_ ms: Int64) -> Nanos {
        return ms * 1_000_000
    }

    /// Durée en secondes (partie entière + reste, comme en Go).
    public static func seconds(_ dur: Nanos) -> Double {
        return vs_ns_seconds(dur)
    }

    public static func milliseconds(_ ms: Int64) -> Nanos {
        return ms * 1_000_000
    }
}

// MARK: - Ponts de chaînes

/// Voit une chaîne Swift comme un `Str8` le temps d'un appel — et pas une
/// instruction de plus. Le tampon est vivant pour toute la durée du corps ;
/// une chaîne vide reçoit quand même une adresse valide (le C ne déréférence
/// jamais un `Str8` de longueur 0, mais un tampon Swift vide n'a pas d'adresse
/// stable).
@inline(__always)
private func withStr8<R>(_ s: String, _ body: (Str8) -> R) -> R {
    var bytes = Array(s.utf8)
    let length = bytes.count
    if bytes.isEmpty {
        bytes = [0]
    }
    return bytes.withUnsafeMutableBufferPointer { buffer in
        body(Str8(data: buffer.baseAddress, len: length))
    }
}

/// Recopie un `StrBuf` (tampon borné, inclus par valeur dans l'état C) en
/// `String`. La copie est délibérée : rien de ce que l'interface garde ne doit
/// pointer dans le moteur.
private func string(_ buf: StrBuf) -> String {
    var copy = buf
    let capacity = MemoryLayout.size(ofValue: copy.data)
    let length = max(0, min(Int(copy.len), capacity))
    return withUnsafeBytes(of: &copy.data) { raw in
        String(decoding: raw.prefix(length), as: UTF8.self)
    }
}

// MARK: - Le moteur

public final class CoreEngine {

    public enum Phase {
        case idle
        case connecting
        case connected
    }

    public enum Correction {
        case none
        case nudge
        case seek
    }

    /// L'unique exemplaire de l'état : ~30 Ko de tampons bornés, copiable,
    /// sans un seul pointeur vers l'extérieur.
    private var engine = VsEngine()

    /// Position proposée par la reprise « salle vierge » lors du dernier
    /// welcome (nil sinon). Le moteur reste pur : il signale seulement qu'un
    /// toast « Reprise à … » est à afficher, c'est l'interface qui l'affiche.
    public private(set) var resumeToastSec: Double?

    public init() {
        engine_init(&engine)
    }

    // MARK: - Utilitaires exposés (interface, tests)

    public static func clampPosition(_ pos: Double, _ duration: Double) -> Double {
        return engine_clamp_position(pos, duration)
    }

    /// §Assainissement : rend nil si l'état de salle est irrecevable.
    public static func sanitize(_ rs: RoomState) -> RoomState? {
        var input = vsRoomState(rs)
        var output = VsRoomState()
        if engine_sanitize_roomstate(&input, &output) == 0 {
            return nil
        }
        return roomState(output)
    }

    /// Doublement borné de la temporisation de reconnexion (1 s → 10 s).
    public static func nextBackoff(_ current: Nanos) -> Nanos {
        return engine_next_backoff(current)
    }

    // MARK: - État observable

    public var phase: Phase {
        if engine.phase == VS_PHASE_CONNECTED {
            return .connected
        }
        if engine.phase == VS_PHASE_CONNECTING {
            return .connecting
        }
        return .idle
    }

    public var selfId: String {
        return string(engine.self_id)
    }

    /// Salle de la session en cours (posée par `connecting(room:)`).
    public var room: String {
        return string(engine.session_room)
    }

    public var ready: Bool {
        return engine.ready != 0
    }

    public var haveOffset: Bool {
        return engine.have_offset != 0
    }

    public var offsetMs: Int64 {
        return engine.offset_ms
    }

    public var latencyMs: Int64 {
        return engine.latency_ms
    }

    public var roomState: RoomState {
        return CoreEngine.roomState(engine.room_state)
    }

    public var haveState: Bool {
        return engine.have_state != 0
    }

    public var status: VLCStatus {
        var st = VLCStatus()
        st.state = CoreEngine.playState(engine.status.state)
        st.positionSec = engine.status.position_sec
        st.lengthSec = engine.status.length_sec
        st.rate = engine.status.rate
        st.fileName = string(engine.status.file_name)
        return st
    }

    public var haveStatus: Bool {
        return engine.have_status != 0
    }

    public var vlcError: Bool {
        return engine.vlc_error != 0
    }

    public var buffering: Bool {
        return engine.buffering != 0
    }

    public var nudging: Bool {
        return engine.nudging != 0
    }

    public var drift: Double {
        return engine.drift
    }

    public var correcting: Correction {
        if engine.correcting == VS_CORRECT_SEEK {
            return .seek
        }
        if engine.correcting == VS_CORRECT_NUDGE {
            return .nudge
        }
        return .none
    }

    public var fileName: String {
        return string(engine.file_name)
    }

    public var fileDurationSec: Double {
        return engine.file_duration_sec
    }

    public var haveFile: Bool {
        return engine.have_file != 0
    }

    /// Vrai dès que la durée du média a été observée et déclarée au serveur —
    /// l'interface s'en sert pour distinguer « chargement » de « prêt ».
    public var fileDeclared: Bool {
        return haveFile && fileDurationSec > 0
    }

    /// Position attendue de la salle (0 tant qu'aucun état n'est connu).
    public func expectedPosition(_ now: Nanos) -> Double {
        return engine_expected_position(&engine, now)
    }

    public var roomRate: Double {
        return engine_room_rate(&engine)
    }

    public func nowServerMs(_ now: Nanos) -> Int64 {
        return engine_now_server_ms(&engine, now)
    }

    /// Messages composés hors ligne, en attente de livraison (§File d'attente).
    public var pendingChats: [String] {
        let count = engine_pending_chat_count(&engine)
        var out: [String] = []
        for index in 0..<count {
            let s = engine_pending_chat(&engine, index)
            guard let data = s.data, s.len > 0 else {
                out.append("")
                continue
            }
            out.append(String(decoding: UnsafeBufferPointer(start: data, count: Int(s.len)), as: UTF8.self))
        }
        return out
    }

    // MARK: - Cycle de vie de la connexion

    /// Démarrage (ou redémarrage) d'une boucle de connexion vers `room`.
    /// Changer de salle vide la file de chat hors ligne et oublie la mémoire de
    /// séance : ni les messages ni la position d'une salle ne fuient vers une
    /// autre (engine_set_room).
    public func connecting(room newRoom: String) {
        withStr8(newRoom) { engine_set_room(&engine, $0) }
        engine_connecting(&engine)
    }

    public func sessionLost() {
        engine_session_lost(&engine)
    }

    public func disconnected() {
        engine_disconnected(&engine)
    }

    // MARK: - Entrées serveur

    /// `state` nil (welcome sans état de salle) est passé au C comme un état
    /// nul : `engine_sanitize_roomstate` le refuse (rate 0), exactement ce que
    /// voit le client Windows quand `proto_decode` n'a rien à remplir.
    @discardableResult
    public func onWelcome(now: Nanos,
                          selfId: String,
                          state: RoomState?,
                          selfReady: Bool? = nil) -> [Decision] {
        var out = VsOutput()
        vs_output_reset(&out)
        var st = state.map { CoreEngine.vsRoomState($0) } ?? VsRoomState()
        withStr8(selfId) { id in
            if let value = selfReady {
                var flag: b32 = value ? 1 : 0
                withUnsafePointer(to: &flag) { ready in
                    engine_on_welcome(&engine, now, id, &st, ready, &out)
                }
            } else {
                engine_on_welcome(&engine, now, id, &st, nil, &out)
            }
        }
        resumeToastSec = out.have_resume_toast != 0 ? out.resume_toast_sec : nil
        return CoreEngine.decisions(&out)
    }

    public func onPong(now: Nanos, _ p: Pong) {
        engine_on_pong(&engine, now, VsPong(t: p.t, server_ms: p.serverMs))
    }

    public func onRoomState(now: Nanos, _ rs: RoomState) {
        var value = CoreEngine.vsRoomState(rs)
        engine_on_roomstate(&engine, now, &value)
    }

    public func onSelfReady(_ value: Bool) {
        engine_on_self_ready(&engine, value ? 1 : 0)
    }

    // MARK: - Entrées lecteur

    /// Un fichier vient d'être ouvert dans VLC. Le fichier est déclaré
    /// immédiatement (nom + taille, durée encore inconnue), puis re-déclaré dès
    /// que la durée observée diverge.
    @discardableResult
    public func openFile(name: String, sizeBytes: Int64) -> [Decision] {
        var out = VsOutput()
        vs_output_reset(&out)
        withStr8(name) { engine_open_file(&engine, $0, sizeBytes, &out) }
        return CoreEngine.decisions(&out)
    }

    @discardableResult
    public func onVLCStatus(now: Nanos, _ st: VLCStatus) -> [Decision] {
        var out = VsOutput()
        vs_output_reset(&out)
        var value = VsStatus()
        value.state = CoreEngine.vsPlayState(st.state)
        value.position_sec = st.positionSec
        value.length_sec = st.lengthSec
        value.rate = st.rate
        withStr8(st.fileName) { strbuf_set(&value.file_name, $0) }
        engine_on_vlc_status(&engine, now, &value, &out)
        return CoreEngine.decisions(&out)
    }

    public func onVLCError() {
        engine_on_vlc_error(&engine)
    }

    // MARK: - Tic d'horloge

    @discardableResult
    public func onTick(now: Nanos) -> [Decision] {
        var out = VsOutput()
        vs_output_reset(&out)
        engine_on_tick(&engine, now, &out)
        return CoreEngine.decisions(&out)
    }

    // MARK: - Actions de l'utilisateur venues de l'interface

    @discardableResult
    public func userControl(now: Nanos, action: ControlAction, positionSec: Double?) -> [Decision] {
        var out = VsOutput()
        vs_output_reset(&out)
        engine_user_control(&engine, now, CoreEngine.vsAction(action),
                            positionSec ?? 0, positionSec != nil ? 1 : 0, &out)
        return CoreEngine.decisions(&out)
    }

    @discardableResult
    public func setReady(_ value: Bool) -> [Decision] {
        var out = VsOutput()
        vs_output_reset(&out)
        engine_set_ready(&engine, value ? 1 : 0, &out)
        return CoreEngine.decisions(&out)
    }

    /// Envoie un message de salle… ou le met en file s'il est composé hors
    /// ligne : il partira, dans l'ordre, après le welcome de reconnexion. C'est
    /// le SEUL type de message rejoué (§File d'attente hors ligne) — capacité
    /// que le moteur Swift natif n'avait jamais eue.
    @discardableResult
    public func chat(_ text: String) -> [Decision] {
        var out = VsOutput()
        vs_output_reset(&out)
        withStr8(text) { engine_chat(&engine, $0, &out) }
        return CoreEngine.decisions(&out)
    }

    // MARK: - Conversions

    /// Sorties du moteur : commandes VLC d'abord, messages serveur ensuite.
    /// `VsOutput` sépare les deux familles — l'ordre à l'intérieur de chacune
    /// est celui qui compte (c'est ce que comparent les vecteurs), et les deux
    /// partent de toute façon sur des canaux différents.
    private static func decisions(_ out: inout VsOutput) -> [Decision] {
        var result: [Decision] = []
        let commandCount = Int(out.cmd_count)
        if commandCount > 0 {
            withUnsafeBytes(of: &out.cmds) { raw in
                let items = raw.bindMemory(to: VsCmd.self)
                for index in 0..<commandCount {
                    if let command = self.command(items[index]) {
                        result.append(.vlc(command))
                    }
                }
            }
        }
        let messageCount = Int(out.msg_count)
        if messageCount > 0 {
            withUnsafeBytes(of: &out.msgs) { raw in
                let items = raw.bindMemory(to: VsMsg.self)
                for index in 0..<messageCount {
                    if let message = self.message(items[index]) {
                        result.append(.server(message))
                    }
                }
            }
        }
        return result
    }

    private static func command(_ c: VsCmd) -> VLCCommand? {
        switch c.kind {
        case VS_CMD_PAUSE: return VLCCommand(.pause, c.value)
        case VS_CMD_RESUME: return VLCCommand(.resume, c.value)
        case VS_CMD_SEEK: return VLCCommand(.seek, c.value)
        case VS_CMD_RATE: return VLCCommand(.rate, c.value)
        default: return nil
        }
    }

    private static func message(_ m: VsMsg) -> ClientMessage? {
        switch m.kind {
        case VS_MSG_PING:
            return .ping(t: m.t)
        case VS_MSG_SET_READY:
            return .setReady(ready: m.ready != 0)
        case VS_MSG_SET_FILE:
            return .setFile(name: string(m.name), durationSec: m.duration_sec, sizeBytes: m.size_bytes)
        case VS_MSG_CONTROL:
            return .control(action: action(m.action), positionSec: m.position_sec)
        case VS_MSG_REPORT:
            return .report(positionSec: m.position_sec, paused: m.paused != 0, buffering: m.buffering != 0)
        case VS_MSG_CHAT:
            return .chat(text: string(m.text))
        default:
            return nil
        }
    }

    private static func action(_ a: VsAction) -> ControlAction {
        if a == VS_ACT_PAUSE {
            return .pause
        }
        if a == VS_ACT_SEEK {
            return .seek
        }
        return .play
    }

    private static func vsAction(_ a: ControlAction) -> VsAction {
        switch a {
        case .play: return VS_ACT_PLAY
        case .pause: return VS_ACT_PAUSE
        case .seek: return VS_ACT_SEEK
        }
    }

    private static func playState(_ s: VsPlayState) -> PlayState {
        if s == VS_PLAY_PLAYING {
            return .playing
        }
        if s == VS_PLAY_PAUSED {
            return .paused
        }
        return .stopped
    }

    private static func vsPlayState(_ s: PlayState) -> VsPlayState {
        switch s {
        case .stopped: return VS_PLAY_STOPPED
        case .playing: return VS_PLAY_PLAYING
        case .paused: return VS_PLAY_PAUSED
        }
    }

    private static func vsRoomState(_ rs: RoomState) -> VsRoomState {
        var out = VsRoomState()
        out.paused = rs.paused ? 1 : 0
        out.position_sec = rs.positionSec
        out.rate = rs.rate
        out.ref_server_ms = rs.refServerMs
        withStr8(rs.setBy) { strbuf_set(&out.set_by, $0) }
        return out
    }

    private static func roomState(_ rs: VsRoomState) -> RoomState {
        return RoomState(paused: rs.paused != 0,
                         positionSec: rs.position_sec,
                         rate: rs.rate,
                         refServerMs: rs.ref_server_ms,
                         setBy: string(rs.set_by))
    }
}
