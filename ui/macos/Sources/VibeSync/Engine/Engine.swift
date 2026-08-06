// Engine.swift — moteur de synchronisation, machine à états PURE.
//
// Port Swift de ui/win32/src/engine.c (lui-même port de internal/client, la
// référence Go), conforme à docs/protocol.md §Comportements client et gelé par
// test/vectors/*.json.
//
// Aucune dépendance réseau, VLC ou interface : le moteur reçoit des
// observations (pong, roomState, statut VLC, tic d'horloge) et rend des
// décisions. C'est ce qui le rend rejouable contre les vecteurs.

import Foundation

public struct Engine {

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

    // MARK: - État interne

    /// Ce que le moteur croit que VLC est en train de faire.
    private struct Expectation {
        var valid: Bool = false
        var paused: Bool = false
        var pos: Double = 0
        var at: Nanos = 0
        var rate: Double = 1

        func predict(_ now: Nanos) -> Double {
            if paused {
                return pos
            }
            var r = rate
            if r <= 0 {
                r = 1
            }
            var d = VSTime.seconds(now - at)
            if d < 0 {
                d = 0
            }
            return pos + d * r
        }
    }

    /// Détecteur de buffering : la position n'avance plus alors que VLC se
    /// déclare en lecture (l'interface HTTP de VLC n'expose pas d'état de
    /// buffering). Port fidèle de internal/vlc (BufferingDetector), suspension
    /// et anti-masquage compris.
    private struct BufferDetect {
        var have: Bool = false
        var buffering: Bool = false
        var stallFrom: Nanos? = nil
        var lastPos: Double = 0
        var lastAt: Nanos = 0
        /// Instant jusqu'auquel la détection est neutralisée : un seek ou une
        /// transition play/pause fige mécaniquement la position le temps que
        /// VLC obéisse, ce n'est pas un buffering (VS-017).
        var suspendUntil: Nanos? = nil

        mutating func reset() {
            have = false
            buffering = false
            // Sans cet oubli, un stall entamé avant le reset ferait basculer en
            // buffering dès la deuxième observation qui suit.
            stallFrom = nil
        }

        /// Oublie le stall en cours et neutralise la détection jusqu'à now+d.
        ///
        /// Anti-masquage : la demande est ignorée tant qu'on n'est pas à
        /// minSuspendGap de la fin de la suspension précédente — il reste donc
        /// toujours une fenêtre de vision entre deux, sans quoi un moteur qui
        /// corrige en boucle un lecteur figé resterait aveugle indéfiniment.
        /// Le verdict courant est conservé : envoyer un seek ne prouve pas que
        /// la lecture est repartie.
        mutating func suspend(_ now: Nanos, _ d: Nanos) {
            if let until = suspendUntil, now < until + Sync.minSuspendGap {
                return
            }
            have = false
            stallFrom = nil
            suspendUntil = now + d
        }

        func suspended(_ now: Nanos) -> Bool {
            guard let until = suspendUntil else {
                return false
            }
            return now < until
        }

        mutating func observe(_ st: VLCStatus, _ now: Nanos) -> Bool {
            if suspended(now) {
                // On continue d'ancrer la position pour que la reprise de la
                // détection ne voie pas d'un coup tout le saut accumulé pendant
                // la suspension. Aucun diagnostic posé ni levé.
                have = true
                lastPos = st.positionSec
                lastAt = now
                stallFrom = nil
                return buffering
            }
            if st.state != .playing {
                have = true
                lastPos = st.positionSec
                lastAt = now
                stallFrom = nil
                buffering = false
                return false
            }
            if !have {
                have = true
                lastPos = st.positionSec
                lastAt = now
                return false
            }
            let elapsed = VSTime.seconds(now - lastAt)
            if elapsed <= 0 {
                return buffering
            }
            let progressed = st.positionSec - lastPos
            lastPos = st.positionSec
            lastAt = now
            if progressed >= elapsed * Sync.bufferMinRatio {
                stallFrom = nil
                buffering = false
                return false
            }
            guard let from = stallFrom else {
                stallFrom = now
                return buffering
            }
            if now - from >= Sync.bufferWindow {
                buffering = true
            }
            return buffering
        }
    }

    // MARK: - État observable (interface et tests)

    public private(set) var phase: Phase = .idle
    public private(set) var selfId: String = ""
    public private(set) var ready: Bool = false
    /// Salle courante (celle de la demande de connexion, corrigée par le welcome).
    public private(set) var room: String = ""

    // Mémoire de séance (§Salle vierge) : dernière position de salle connue, et
    // pour quelle salle. Elle survit aux coupures — c'est tout son intérêt —
    // mais pas à un changement de salle. Sans elle, aucune reprise n'est
    // possible : un premier join ne propose jamais rien.
    private var resumeRoom: String = ""
    private var resumePos: Double = 0
    private var resumeKnown: Bool = false

    // horloge serveur
    private var offsets: [Int64] = []
    public private(set) var haveOffset: Bool = false
    public private(set) var offsetMs: Int64 = 0
    public private(set) var latencyMs: Int64 = 0

    // état de salle de référence
    public private(set) var roomState = RoomState(paused: true, positionSec: 0, rate: 1, refServerMs: 0, setBy: "")
    public private(set) var haveState: Bool = false
    private var pendingRoomState: RoomState? = nil

    // fenêtres temporelles (nil = « pas armée »)
    private var graceUntil: Nanos? = nil
    private var holdUntil: Nanos? = nil
    private var userHoldUntil: Nanos? = nil

    // lecteur
    public private(set) var status = VLCStatus()
    public private(set) var haveStatus: Bool = false
    public private(set) var vlcError: Bool = false
    private var expect = Expectation()
    private var buf = BufferDetect()
    public private(set) var buffering: Bool = false
    private var appliedRate: Double = 1
    public private(set) var nudging: Bool = false
    public private(set) var drift: Double = 0
    public private(set) var correcting: Correction = .none

    // fichier déclaré
    public private(set) var fileName: String = ""
    public private(set) var fileDurationSec: Double = 0
    public private(set) var fileSizeBytes: Int64 = 0
    public private(set) var haveFile: Bool = false
    /// Vrai dès que la durée du média a été observée et déclarée au serveur —
    /// l'UI s'en sert pour distinguer « chargement » de « prêt ».
    public var fileDeclared: Bool {
        return haveFile && fileDurationSec > 0
    }

    // tâches périodiques
    private var lastPing: Nanos? = nil
    private var lastReport: Nanos? = nil

    public init() {}

    // MARK: - Utilitaires

    public static func clampPosition(_ pos: Double, _ duration: Double) -> Double {
        var p = pos
        if p < 0 {
            p = 0
        }
        if duration > 0 && p > duration {
            p = duration
        }
        return p
    }

    private func duration() -> Double {
        if haveStatus && status.lengthSec > 0 {
            return status.lengthSec
        }
        if haveFile && fileDurationSec > 0 {
            return fileDurationSec
        }
        return 0
    }

    public var roomRate: Double {
        return roomState.rate <= 0 ? 1 : roomState.rate
    }

    public func nowServerMs(_ now: Nanos) -> Int64 {
        return VSTime.toUnixMs(now) + offsetMs
    }

    /// Position attendue de la salle (0 tant qu'aucun état n'est connu).
    public func expectedPosition(_ now: Nanos) -> Double {
        if !haveState {
            return 0
        }
        if roomState.paused {
            return roomState.positionSec > 0 ? roomState.positionSec : 0
        }
        let elapsed = Double(nowServerMs(now) - roomState.refServerMs) / 1000.0
        let pos = roomState.positionSec + elapsed * roomRate
        return pos < 0 ? 0 : pos
    }

    /// Durée connue du média (statut VLC puis fichier déclaré).
    public var knownDurationSec: Double {
        return duration()
    }

    // MARK: - Cycle de vie de la connexion

    /// Oublie tout ce qui sert à corriger : hors état connecté, l'état de
    /// référence est invalidé jusqu'au welcome suivant
    /// (docs/protocol.md §Conditions de correction).
    private mutating func invalidateReference() {
        selfId = ""
        haveState = false
        haveOffset = false
        offsets.removeAll()
        pendingRoomState = nil
        userHoldUntil = nil
        holdUntil = nil
        nudging = false
        correcting = .none
        drift = 0
    }

    /// Démarrage (ou redémarrage) d'une boucle de connexion vers `room`.
    public mutating func connecting(room newRoom: String) {
        let trimmed = newRoom.trimmingCharacters(in: .whitespaces)
        if trimmed != resumeRoom.trimmingCharacters(in: .whitespaces) {
            // Une séance suivie ailleurs ne se propose pas ici.
            resumeRoom = ""
            resumePos = 0
            resumeKnown = false
        }
        room = newRoom
        phase = .connecting
        invalidateReference()
    }

    public mutating func sessionLost() {
        phase = .connecting
        invalidateReference()
    }

    public mutating func disconnected() {
        phase = .idle
        invalidateReference()
    }

    // MARK: - État de salle

    /// §Assainissement : toute donnée entrante est validée.
    public static func sanitize(_ rs: RoomState) -> RoomState? {
        if !rs.positionSec.isFinite || rs.positionSec < 0 {
            return nil
        }
        if !rs.rate.isFinite || rs.rate < Sync.minRate || rs.rate > Sync.maxRate {
            return nil
        }
        if !rs.paused && rs.refServerMs <= 0 {
            return nil
        }
        return rs
    }

    /// Installe l'état de référence et arme la fenêtre de grâce.
    private mutating func adopt(_ rs: RoomState, _ now: Nanos) {
        roomState = rs
        haveState = true
        graceUntil = now + Sync.graceWindow
    }

    public mutating func onRoomState(now: Nanos, _ raw: RoomState) {
        guard let rs = Engine.sanitize(raw) else {
            return
        }
        if let hold = userHoldUntil, now < hold {
            if !selfId.isEmpty && rs.setBy == selfId {
                // Écho de notre propre control : le hold tombe.
                userHoldUntil = nil
                pendingRoomState = nil
                adopt(rs, now)
                return
            }
            // roomState d'autrui pendant le hold : mémorisé (le dernier gagne).
            pendingRoomState = rs
            return
        }
        adopt(rs, now)
    }

    public mutating func onPong(now: Nanos, _ p: Pong) {
        let nowMs = VSTime.toUnixMs(now)
        var rtt = nowMs - p.t
        if rtt < 0 {
            rtt = 0
        }
        let offset = p.serverMs + rtt / 2 - nowMs
        offsets.append(offset)
        if offsets.count > Sync.offsetSamples {
            offsets.removeFirst(offsets.count - Sync.offsetSamples)
        }
        // Médiane glissante des 5 dernières mesures.
        let sorted = offsets.sorted()
        offsetMs = sorted.isEmpty ? 0 : sorted[sorted.count / 2]
        latencyMs = rtt / 2
        haveOffset = true
    }

    public mutating func onSelfReady(_ value: Bool) {
        ready = value
    }

    /// Dit si ce welcome déclenche une reprise « salle vierge »
    /// (docs/protocol.md §Erreurs et robustesse), et à quelle position.
    ///
    /// Conditions cumulatives : la salle n'a jamais reçu de control (`setBy`
    /// vide, position 0) ET nous étions déjà connectés à CETTE salle dans ce
    /// processus, avec une position de séance connue au-delà du seuil. La
    /// position proposée est la dernière position de SALLE connue, jamais la
    /// position brute de VLC. À appeler AVANT d'adopter l'état du welcome.
    private func virginResumeCandidate(_ rs: RoomState, _ welcomeRoom: String) -> Double? {
        if !rs.setBy.isEmpty || rs.positionSec != 0 {
            return nil
        }
        if !resumeKnown || resumeRoom.isEmpty || resumeRoom != welcomeRoom {
            return nil
        }
        if resumePos <= Sync.virginResumeSec {
            return nil
        }
        return Engine.clampPosition(resumePos, duration())
    }

    @discardableResult
    public mutating func onWelcome(now: Nanos,
                                   selfId newSelfId: String,
                                   room welcomeRoom: String,
                                   state: RoomState?) -> [Decision] {
        var out: [Decision] = []
        selfId = newSelfId
        phase = .connected
        if !welcomeRoom.isEmpty {
            room = welcomeRoom
        }
        // Décidé AVANT d'adopter l'état : l'adoption écrase la référence sur
        // laquelle repose la reprise.
        var resume: Double? = nil
        if let st = state, let sane = Engine.sanitize(st) {
            resume = virginResumeCandidate(sane, room)
        }
        // Le welcome est la référence d'une session neuve : aucun hold ni
        // roomState en attente ne lui survit.
        userHoldUntil = nil
        pendingRoomState = nil
        holdUntil = nil
        nudging = false
        if let st = state {
            onRoomState(now: now, st)
        }
        // Volontairement pas d'adoption du ready serveur ici : le serveur vient
        // de nous créer un membre neuf, donc « pas prêt ». C'est notre état
        // local qui fait foi au (re)join. Re-déclarer systématiquement notre
        // état : l'état courant fait foi (§File d'attente hors ligne).
        if haveFile {
            out.append(.server(.setFile(name: fileName,
                                        durationSec: fileDurationSec,
                                        sizeBytes: fileSizeBytes)))
        }
        out.append(.server(.setReady(ready: ready)))
        out.append(.server(.ping(t: VSTime.toUnixMs(now))))
        lastPing = now
        // Salle vierge : proposer la séance perdue avant tout alignement. C'est
        // un control comme un autre : il arme le hold post-action, ce qui
        // suspend l'alignement sur l'état vierge jusqu'à l'écho du serveur.
        if let pos = resume {
            userAction(.seek, pos, now, &out, armGrace: false)
        }
        return out
    }

    // MARK: - Lecteur

    /// Un fichier vient d'être ouvert dans VLC (le driver force pause +
    /// position 0 avant de rendre la main). Comme la référence Go : le fichier
    /// est déclaré immédiatement (nom + taille, durée encore inconnue), puis
    /// re-déclaré par declareFile dès que la durée observée diverge.
    @discardableResult
    public mutating func openFile(now: Nanos, name: String, sizeBytes: Int64) -> [Decision] {
        fileName = name
        fileDurationSec = 0
        fileSizeBytes = sizeBytes
        haveFile = true
        status = VLCStatus()
        haveStatus = false
        expect = Expectation()
        // Média neuf : le diagnostic du précédent ne vaut plus rien. Ouvrir un
        // fichier enchaîne pause + seek 0 + démarrage, autant de raisons
        // mécaniques de voir la position figée : on suspend aussi la détection.
        buf.reset()
        buffering = false
        buf.suspend(now, Sync.bufferingSuspend)
        vlcError = false
        appliedRate = 1
        return [.server(.setFile(name: name, durationSec: 0, sizeBytes: sizeBytes))]
    }

    /// Envoie setFile dès qu'un fichier (et sa durée) est connu — et le
    /// re-déclare si le nom ou la durée observés divergent de ce qui a été
    /// annoncé (même règle que la référence Go).
    private mutating func declareFile(_ st: VLCStatus, _ out: inout [Decision]) {
        if !st.loaded || st.lengthSec <= 0 {
            return
        }
        var name = st.fileName
        if name.isEmpty && haveFile {
            name = fileName
        }
        if haveFile && fileName == name && abs(fileDurationSec - st.lengthSec) < 0.5 {
            return
        }
        fileName = name
        fileDurationSec = st.lengthSec
        haveFile = true
        out.append(.server(.setFile(name: name,
                                    durationSec: st.lengthSec,
                                    sizeBytes: fileSizeBytes)))
    }

    /// Remonte une action utilisateur (control) et arme le hold de 2 s.
    /// `armGrace` n'est vrai que pour une action détectée DANS VLC : la fenêtre
    /// de grâce protège alors la ré-observation (la référence Go ne l'arme pas
    /// pour un control venu de l'UI ni pour la reprise « salle vierge »).
    private mutating func userAction(_ act: ControlAction,
                                     _ pos: Double,
                                     _ now: Nanos,
                                     _ out: inout [Decision],
                                     armGrace: Bool = true) {
        out.append(.server(.control(action: act,
                                    positionSec: Engine.clampPosition(pos, duration()))))
        if armGrace {
            graceUntil = now + Sync.graceWindow
        }
        userHoldUntil = now + Sync.userHold
        pendingRoomState = nil
        // Le seek (ou la transition) qui va suivre fige la position, ce n'est
        // pas un buffering.
        buf.suspend(now, Sync.bufferingSuspend)
    }

    /// Compare l'observation à ce que le moteur attendait ; tout écart non
    /// provoqué par lui est une action de l'utilisateur dans VLC.
    private mutating func detectUserAction(_ st: VLCStatus, _ now: Nanos, _ out: inout [Decision]) {
        if !expect.valid || phase != .connected {
            return
        }
        // Transitions depuis/vers « stopped » : fin de média, pas une action.
        if !st.loaded || !status.loaded {
            return
        }
        let nowPaused = st.state != .playing
        if nowPaused != expect.paused {
            userAction(nowPaused ? .pause : .play, st.positionSec, now, &out)
            return
        }
        if abs(st.positionSec - expect.predict(now)) > Sync.userSeekSec {
            userAction(.seek, st.positionSec, now, &out)
        }
    }

    public mutating func onVLCError() {
        vlcError = true
        haveStatus = false
        expect.valid = false
    }

    @discardableResult
    public mutating func onVLCStatus(now: Nanos, _ st: VLCStatus) -> [Decision] {
        var out: [Decision] = []
        vlcError = false
        buffering = buf.observe(st, now)
        // Pendant la fenêtre de grâce, l'observation ne fait pas autorité : on
        // ne remplace pas l'attendu, sinon une action utilisateur survenue
        // pendant la grâce serait absorbée définitivement.
        let inGrace: Bool = graceUntil.map { now < $0 } ?? false
        if haveStatus && !inGrace {
            detectUserAction(st, now, &out)
        }
        status = st
        haveStatus = true
        // … sauf s'il n'y a encore aucune attente : il faut bien une référence
        // de départ, y compris si la première observation tombe pendant une grâce.
        if !inGrace || !expect.valid {
            expect.valid = true
            expect.paused = st.state != .playing
            expect.pos = st.positionSec
            expect.at = now
            expect.rate = st.rate
        }
        declareFile(st, &out)
        return out
    }

    // MARK: - Décisions

    /// Met à jour ce que le moteur attend de VLC après ces commandes et arme
    /// les fenêtres anti-boucle.
    private mutating func arm(_ cmds: [VLCCommand], _ now: Nanos) {
        if cmds.isEmpty {
            return
        }
        var hold = false
        for c in cmds {
            switch c.kind {
            case .pause:
                expect.pos = expect.predict(now)
                expect.paused = true
                expect.at = now
                buf.suspend(now, Sync.bufferingSuspend)
                hold = true
            case .resume:
                expect.pos = expect.predict(now)
                expect.paused = false
                expect.at = now
                buf.suspend(now, Sync.bufferingSuspend)
                hold = true
            case .seek:
                expect.pos = c.value.rounded()  // VLC arrondit à la seconde
                expect.at = now
                buf.suspend(now, Sync.bufferingSuspend)
                hold = true
            case .rate:
                expect.pos = expect.predict(now)
                expect.at = now
                expect.rate = c.value
                appliedRate = c.value
            }
        }
        graceUntil = now + Sync.graceWindow
        if hold {
            holdUntil = now + Sync.graceWindow
        }
    }

    /// Décide des corrections à appliquer à VLC.
    ///
    /// Conditions de correction (docs/protocol.md) : état connecté, état de
    /// salle de référence valide, et au moins une mesure d'offset d'horloge.
    private mutating func plan(_ now: Nanos, _ out: inout [Decision]) {
        correcting = .none
        if !haveStatus || !status.loaded {
            drift = 0
            return
        }
        if phase != .connected || !haveState {
            drift = 0
            nudging = false
            return
        }
        let base = roomRate
        let expected = Engine.clampPosition(expectedPosition(now), status.lengthSec)
        let driftValue = status.positionSec - expected
        drift = driftValue
        if !haveOffset {
            return  // pas encore d'horloge serveur fiable
        }
        if let h = userHoldUntil, now < h {
            return  // hold post-action : on attend l'écho
        }
        if let h = holdUntil, now < h {
            return  // une correction est déjà en vol
        }

        var acts: [VLCCommand] = []
        let absDrift = abs(driftValue)

        if roomState.paused {
            // En pause : jamais de nudge, seek uniquement au-delà du seuil (le
            // seek HTTP est arrondi à la seconde, il rapproche toujours sous
            // ce seuil).
            nudging = false
            if status.state == .playing {
                acts.append(VLCCommand(.pause))
            }
            if abs(appliedRate - base) > 1e-3 || abs(status.rate - base) > 1e-3 {
                acts.append(VLCCommand(.rate, base))
            }
            if absDrift >= Sync.pausedSeekSec {
                acts.append(VLCCommand(.seek, expected))
                correcting = .seek
            }
        } else if status.state == .paused {
            // §Départ et reprise de lecture : la salle passe en lecture alors
            // que VLC est en pause. On cale d'abord VLC sur la position de
            // référence (seek au-delà de 0,3 s d'écart) PUIS on lance la
            // lecture — le nudge à 5 %/s mettrait 10 s à résorber 0,5 s.
            nudging = false
            if absDrift >= Sync.startSeekSec {
                acts.append(VLCCommand(.seek, expected))
                correcting = .seek
            }
            acts.append(VLCCommand(.resume))
            if abs(appliedRate - base) > 1e-3 || abs(status.rate - base) > 1e-3 {
                acts.append(VLCCommand(.rate, base))
            }
        } else {
            var target = base
            var wantRate = false
            if absDrift >= Sync.hardSeekSec {
                correcting = .seek
                nudging = false
                acts.append(VLCCommand(.seek, expected))
                wantRate = true
            } else if (nudging && absDrift >= Sync.nudgeExitSec) ||
                      (!nudging && absDrift > Sync.deadZoneSec) {
                // Hystérésis : on engage au-delà de 0,1 s, on ne lâche qu'en
                // dessous de 0,03 s — sinon le rate oscillerait autour du seuil.
                nudging = true
                correcting = .nudge
                target = driftValue > 0 ? base * Sync.nudgeSlow   // en avance : ralentir
                                        : base * Sync.nudgeFast   // en retard : accélérer
                wantRate = true
            } else {
                nudging = false
                wantRate = true
            }
            if wantRate && (abs(appliedRate - target) > 1e-3 || abs(status.rate - target) > 1e-3) {
                acts.append(VLCCommand(.rate, target))
            }
        }

        if acts.isEmpty {
            return
        }
        for c in acts {
            out.append(.vlc(c))
        }
        arm(acts, now)
    }

    /// Lève le hold post-action à échéance. Faute d'écho du serveur, le dernier
    /// roomState reçu pendant le hold (celui d'autrui, qui précédait forcément
    /// notre control) est appliqué à ce moment-là.
    private mutating func expireUserHold(_ now: Nanos) {
        guard let h = userHoldUntil, now >= h else {
            return
        }
        userHoldUntil = nil
        guard let rs = pendingRoomState else {
            return
        }
        pendingRoomState = nil
        adopt(rs, now)
    }

    /// Programme ping (2 s) et report (1 s).
    private mutating func periodic(_ now: Nanos, _ out: inout [Decision]) {
        if phase != .connected {
            return
        }
        var wantPing = true
        if let lp = lastPing, now - lp < Sync.pingEvery {
            wantPing = false
        }
        if wantPing {
            lastPing = now
            out.append(.server(.ping(t: VSTime.toUnixMs(now))))
        }
        var wantReport = haveStatus
        if let lr = lastReport, now - lr < Sync.reportEvery {
            wantReport = false
        }
        if wantReport {
            lastReport = now
            out.append(.server(.report(positionSec: status.positionSec,
                                       paused: status.state != .playing,
                                       buffering: buffering)))
        }
    }

    /// Mémorise où en est la séance de la salle courante. Cette mémoire est la
    /// seule chose qui autorise une reprise si le serveur revient amnésique
    /// (§Salle vierge) : alimentée seulement connecté avec un état de salle
    /// valide, elle se fige à la dernière position connue quand la connexion
    /// tombe.
    private mutating func rememberSession(_ now: Nanos) {
        if phase != .connected || !haveState || room.isEmpty {
            return
        }
        resumeRoom = room
        resumePos = expectedPosition(now)
        resumeKnown = true
    }

    /// Tic d'horloge : lève le hold, décide des corrections, tâches périodiques.
    @discardableResult
    public mutating func onTick(now: Nanos) -> [Decision] {
        var out: [Decision] = []
        expireUserHold(now)
        rememberSession(now)
        plan(now, &out)
        periodic(now, &out)
        return out
    }

    // MARK: - Actions de l'utilisateur venues de l'interface

    @discardableResult
    public mutating func userControl(now: Nanos,
                                     action: ControlAction,
                                     positionSec: Double?) -> [Decision] {
        var pos: Double
        if let p = positionSec {
            pos = p
        } else {
            pos = (haveStatus && status.loaded) ? status.positionSec : expectedPosition(now)
        }
        if !pos.isFinite {
            return []  // §Assainissement
        }
        pos = Engine.clampPosition(pos, duration())
        // Hold post-action : corrections suspendues jusqu'à l'écho ou l'expiration.
        var out: [Decision] = []
        userAction(action, pos, now, &out, armGrace: false)
        return out
    }

    @discardableResult
    public mutating func setReady(_ value: Bool) -> [Decision] {
        ready = value
        return [.server(.setReady(ready: value))]
    }

    @discardableResult
    public mutating func chat(_ text: String) -> [Decision] {
        if text.isEmpty {
            return []
        }
        return [.server(.chat(text: text))]
    }
}
