// VSCoreVectorsTests.swift — rejeu des vecteurs de conformité À TRAVERS L'API C
// (ADR-010, VS-031 phase 2).
//
// VectorsTests.swift rejoue les mêmes fichiers contre le moteur SWIFT natif.
// Ce test-ci rejoue exactement le même scénario contre `VSCore`, la couche C
// commune aux deux clients : c'est la DOUBLE COUVERTURE de la phase 2. Tant
// qu'elle est verte, on sait que le C compilé sur macOS se comporte comme la
// référence Go — et donc que la bascule de la phase 3 (VS-032) ne changera
// aucun comportement observable.
//
// Le déroulé reproduit celui de core/tests/test_core.c (run_vector) au geste
// près : mêmes appels, même ordre, même règle `keepOutput`. Ce qui reste en
// Swift est le décor — lecture du JSON du vecteur et faux VLC (FakeVLC.swift,
// partagé avec VectorsTests) ; tout ce qui est jugé passe par le C :
// vlc_parse_status, engine_on_welcome/_pong/_roomstate/_vlc_status/_tick.

import XCTest
import VSCore
@testable import VibeSync

final class VSCoreVectorsTests: XCTestCase {

    /// Taille du fichier vidéo factice écrit par le générateur de vecteurs
    /// (vectors_test.go : []byte("données vidéo") = 15 octets UTF-8).
    private let vectorFileSize: Int64 = 15
    /// Origine de l'horloge simulée (vlctest.NewClock : 2026-08-05 20:00 UTC).
    private let baseMs: Int64 = 1785960000000
    private let tolerance: Double = 1e-3

    /// Arène de travail du C : elle sert à `vlc_parse_status`, remise à zéro à
    /// chaque poll (le moteur, lui, n'alloue rien — c'est une machine à états
    /// pure, tout son état tient dans `VsEngine`).
    private var arena: OpaquePointer!
    /// Pas de trace effectivement rejoués : un rejeu qui ne compare rien
    /// passerait sinon en silence.
    private var replayedSteps = 0

    override func setUpWithError() throws {
        arena = arena_create(16 * 1024 * 1024)
        XCTAssertNotNil(arena, "arène impossible à créer")
    }

    override func tearDownWithError() throws {
        if arena != nil {
            arena_destroy(arena)
            arena = nil
        }
    }

    // MARK: - Ponts C ↔ Swift
    //
    // Trois conversions, et pas une de plus : une chaîne Swift vue comme Str8
    // le temps d'un appel, un StrBuf relu en String, et les tableaux de taille
    // fixe de VsOutput (importés en tuples) relus comme des tampons.

    private func withStr8<R>(_ s: String, _ body: (Str8) -> R) -> R {
        var bytes = Array(s.utf8)
        let length = bytes.count
        // Un pointeur non nul même pour une chaîne vide : le C compare des
        // longueurs, il ne déréférence jamais un Str8 de longueur 0, mais un
        // tampon Swift vide n'a pas d'adresse stable.
        if bytes.isEmpty {
            bytes = [0]
        }
        return bytes.withUnsafeMutableBufferPointer { buffer in
            body(Str8(data: buffer.baseAddress, len: length))
        }
    }

    private func text(_ buf: StrBuf) -> String {
        var copy = buf
        let length = max(0, Int(copy.len))
        return withUnsafeBytes(of: &copy.data) { raw in
            String(decoding: raw.prefix(length), as: UTF8.self)
        }
    }

    private func commands(_ out: inout VsOutput) -> [VsCmd] {
        let count = Int(out.cmd_count)
        return withUnsafeBytes(of: &out.cmds) { raw in
            let items = raw.bindMemory(to: VsCmd.self)
            return (0..<count).map { items[$0] }
        }
    }

    private func messages(_ out: inout VsOutput) -> [VsMsg] {
        let count = Int(out.msg_count)
        return withUnsafeBytes(of: &out.msgs) { raw in
            let items = raw.bindMemory(to: VsMsg.self)
            return (0..<count).map { items[$0] }
        }
    }

    private func roomState(_ object: [String: Any]?) -> VsRoomState {
        var rs = VsRoomState()
        rs.paused = JSON.bool(object, "paused") ? 1 : 0
        rs.position_sec = JSON.number(object, "positionSec")
        rs.rate = JSON.number(object, "rate")
        rs.ref_server_ms = JSON.int(object, "refServerMs")
        withStr8(JSON.string(object, "setBy")) { strbuf_set(&rs.set_by, $0) }
        return rs
    }

    // MARK: - Localisation des vecteurs

    private func vectorsDirectory() -> URL? {
        let fm = FileManager.default
        var candidates: [String] = []
        if let forced = ProcessInfo.processInfo.environment["VIBESYNC_VECTORS"], !forced.isEmpty {
            candidates.append(forced)
        }
        candidates.append("test/vectors")
        candidates.append("../../test/vectors")

        for path in candidates {
            let url = URL(fileURLWithPath: path, isDirectory: true)
            var isDir: ObjCBool = false
            if fm.fileExists(atPath: url.path, isDirectory: &isDir), isDir.boolValue {
                return url
            }
        }
        // Repli : remonter depuis ce fichier source (…/ui/macos/Tests/VibeSyncTests).
        var dir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        for _ in 0..<6 {
            let candidate = dir.appendingPathComponent("test").appendingPathComponent("vectors")
            var isDir: ObjCBool = false
            if fm.fileExists(atPath: candidate.path, isDirectory: &isDir), isDir.boolValue {
                return candidate
            }
            dir = dir.deletingLastPathComponent()
        }
        return nil
    }

    // MARK: - Test

    func testConformanceVectorsThroughCCore() throws {
        guard let dir = vectorsDirectory() else {
            XCTFail("répertoire test/vectors introuvable (positionnez VIBESYNC_VECTORS)")
            return
        }
        let names = try FileManager.default
            .contentsOfDirectory(atPath: dir.path)
            .filter { $0.hasSuffix(".json") }
            .sorted()
        XCTAssertGreaterThanOrEqual(names.count, 15,
                                    "\(names.count) vecteur(s) dans \(dir.path), attendu au moins 15")
        for name in names {
            replay(dir.appendingPathComponent(name))
        }
        // Plancher, pas un compte exact : les 15 vecteurs totalisent 202 pas
        // aujourd'hui, un vecteur ajouté ne doit pas rendre ce test rouge. Le
        // but est d'attraper le rejeu qui ne compare RIEN (trace vide, sortie
        // anticipée) — panne silencieuse, sinon.
        XCTAssertGreaterThan(replayedSteps, 100,
                             "\(replayedSteps) pas de trace rejoués : le rejeu n'a pas eu lieu")
    }

    private func replay(_ url: URL) {
        let label = url.lastPathComponent
        guard let raw = try? String(contentsOf: url, encoding: .utf8),
              let root = JSON.object(JSON.parse(raw)) else {
            XCTFail("\(label) : vecteur illisible")
            return
        }
        let vectorName = JSON.string(root, "name", label)
        let pollMs = JSON.int(root, "pollIntervalMs", 200)
        guard let initial = JSON.child(root, "initialVLC") else {
            XCTFail("\(vectorName) : initialVLC absent")
            return
        }
        let events = JSON.array(root, "events")
        let trace = JSON.array(root, "trace")
        if trace.isEmpty {
            XCTFail("\(vectorName) : trace vide")
            return
        }

        var now: Nanos = baseMs * 1_000_000
        let fake = FakeVLC(now: now)
        let fileName = JSON.string(initial, "fileName", "media.mkv")
        fake.load(name: fileName, length: JSON.number(initial, "durationSec", 0), now: now)
        fake.seek(JSON.number(initial, "positionSec", 0), now)
        if JSON.string(initial, "state") == "playing" {
            fake.play(now)
        }

        var engine = VsEngine()
        engine_init(&engine)
        var out = VsOutput()
        vs_output_reset(&out)
        withStr8(fileName) { engine_open_file(&engine, $0, vectorFileSize, &out) }
        vs_output_reset(&out)  // le générateur draine les sorties d'ouverture

        var eventIndex = 0
        // keepOutput : par défaut, ce que le moteur émet en réaction immédiate à
        // un événement ne fait pas partie de la trace (le générateur Go le
        // draine). Un événement marqué « keepOutput » garde cette réaction, qui
        // est alors attendue en tête du pas de trace suivant.
        var keepPending = false

        for rawStep in trace {
            guard let step = rawStep as? [String: Any] else {
                continue
            }
            let stepMs = JSON.int(step, "atMs", 0)

            // Les événements datés avant le prochain poll s'appliquent d'abord.
            while eventIndex < events.count {
                guard let event = events[eventIndex] as? [String: Any] else {
                    eventIndex += 1
                    continue
                }
                let at = JSON.int(event, "atMs", 0)
                if at >= stepMs {
                    break
                }
                now = baseMs * 1_000_000 + at * 1_000_000
                let type = JSON.string(event, "type")
                let data = JSON.child(event, "data")
                switch type {
                case "wait":
                    now += JSON.int(data, "durationMs", 0) * 1_000_000
                case "connectionLost":
                    engine_session_lost(&engine)
                case "userPause":
                    fake.pause(now)
                case "userPlay":
                    fake.play(now)
                case "userSeek":
                    fake.seek(JSON.number(data, "positionSec", 0), now)
                case "openFile":
                    // Changement de média en cours de séance (VS-039).
                    // `fake.load` reproduit ce que le driver garantit au
                    // moteur : nouveau fichier chargé, EN PAUSE À 0
                    // (docs/protocol.md §Chargement de fichier). Les commandes
                    // de préparation sont au driver, pas au moteur : elles ne
                    // figurent pas dans la trace.
                    let opened = JSON.string(data, "fileName")
                    fake.load(name: opened, length: JSON.number(data, "durationSec", 0), now: now)
                    withStr8(opened) { engine_open_file(&engine, $0, vectorFileSize, &out) }
                case "welcome":
                    apply(welcome: data, now: now, engine: &engine, out: &out)
                case "pong":
                    let pong = VsPong(t: JSON.int(data, "t", 0), server_ms: JSON.int(data, "serverMs", 0))
                    engine_on_pong(&engine, now, pong)
                case "roomState":
                    var rs = roomState(data)
                    engine_on_roomstate(&engine, now, &rs)
                default:
                    break
                }
                if JSON.bool(event, "keepOutput") {
                    keepPending = true
                } else if !keepPending {
                    vs_output_reset(&out)
                }
                eventIndex += 1
            }

            // Un poll : lecture de VLC, ingestion, décisions.
            now += pollMs * 1_000_000
            let elapsedMs = (now - baseMs * 1_000_000) / 1_000_000
            XCTAssertEqual(elapsedMs, stepMs,
                           "\(vectorName) : désynchronisation d'horloge (\(elapsedMs) ms vs \(stepMs) ms)")

            // Le status.json simulé passe par le parseur C : json.c et
            // vlc_core.c sont sur le chemin critique du rejeu, comme dans le
            // client réel.
            let body = fake.statusJSON(now)
            var status = VsStatus()
            let mark = arena_pos(arena)
            let parsed = withStr8(body) { vlc_parse_status(arena, $0, &status) }
            arena_pop_to(arena, mark)
            if parsed == 0 {
                XCTFail("\(vectorName) : status.json simulé illisible")
                return
            }

            if !keepPending {
                vs_output_reset(&out)
            }
            keepPending = false
            engine_on_vlc_status(&engine, now, &status, &out)
            engine_on_tick(&engine, now, &out)
            XCTAssertEqual(out.dropped, 0, "\(vectorName) : débordement de la file de décisions")

            let at = "\(vectorName) @\(stepMs)ms"
            XCTAssertEqual(String(cString: vs_play_state_name(engine.status.state)),
                           JSON.string(step, "vlcState"), "\(at) : état VLC")
            assertClose(engine.status.position_sec, JSON.number(step, "vlcPositionSec"),
                        "\(at) : position VLC")
            assertClose(engine_expected_position(&engine, now), JSON.number(step, "expectedPositionSec"),
                        "\(at) : position attendue")
            assertClose(engine.drift, JSON.number(step, "driftSec"), "\(at) : drift")

            let got = commands(&out)
            checkCommands(at, JSON.array(step, "vlcCommands"), got)
            checkMessages(at, JSON.array(step, "toServer"), messages(&out))

            // Les commandes décidées sont appliquées à VLC (comme le ferait le
            // pilote HTTP).
            replayedSteps += 1
            for command in got {
                guard let kind = VLCCommand.Kind(rawValue: String(cString: vs_cmd_name(command.kind))) else {
                    continue
                }
                fake.apply(VLCCommand(kind, command.value), now)
            }
        }
    }

    /// welcome : `selfReady` n'est connu que si la liste des participants
    /// contient notre propre identifiant — c'est exactement ce que déduit
    /// proto_fill (protocol.c) à partir du même JSON.
    private func apply(welcome data: [String: Any]?, now: Nanos,
                       engine: inout VsEngine, out: inout VsOutput) {
        let selfId = JSON.string(data, "selfId")
        var state = roomState(JSON.child(data, "state"))
        var selfReady: b32 = 0
        var haveSelfReady = false
        for item in JSON.array(data, "users") {
            guard let user = item as? [String: Any], JSON.string(user, "id") == selfId else {
                continue
            }
            haveSelfReady = true
            selfReady = JSON.bool(user, "ready") ? 1 : 0
            break
        }
        withStr8(selfId) { id in
            if haveSelfReady {
                withUnsafePointer(to: &selfReady) { ready in
                    engine_on_welcome(&engine, now, id, &state, ready, &out)
                }
            } else {
                engine_on_welcome(&engine, now, id, &state, nil, &out)
            }
        }
    }

    // MARK: - Comparaisons

    private func assertClose(_ got: Double, _ want: Double, _ message: String) {
        if abs(got - want) > tolerance {
            XCTFail("\(message) : \(got), attendu \(want)")
        }
    }

    private func checkCommands(_ at: String, _ want: [Any], _ got: [VsCmd]) {
        if want.count != got.count {
            let names = got.map { String(cString: vs_cmd_name($0.kind)) }.joined(separator: ", ")
            XCTFail("\(at) : \(got.count) commande(s) VLC [\(names)], attendu \(want.count)")
            return
        }
        for (index, rawWanted) in want.enumerated() {
            guard let wanted = rawWanted as? [String: Any] else {
                continue
            }
            let expectedKind = JSON.string(wanted, "cmd")
            let name = String(cString: vs_cmd_name(got[index].kind))
            if name != expectedKind {
                XCTFail("\(at) : commande \(index) = \(name), attendu \(expectedKind)")
                continue
            }
            guard let wantedValue = (wanted["value"] as? NSNumber)?.doubleValue else {
                continue
            }
            // La valeur consignée est celle envoyée à VLC : seek arrondi à la
            // seconde, rate au millième.
            let sent = name == "seek" ? got[index].value.rounded() : got[index].value
            assertClose(sent, wantedValue, "\(at) : valeur de \(expectedKind)")
        }
    }

    private func checkMessages(_ at: String, _ want: [Any], _ got: [VsMsg]) {
        if want.count != got.count {
            let names = got.map { String(cString: vs_msg_name($0.kind)) }.joined(separator: ", ")
            var expected: [String] = []
            for item in want {
                if let object = item as? [String: Any] {
                    expected.append(JSON.string(object, "type"))
                }
            }
            XCTFail("\(at) : \(got.count) message(s) [\(names)], attendu \(want.count) [\(expected.joined(separator: ", "))]")
            return
        }
        for (index, rawWanted) in want.enumerated() {
            guard let wanted = rawWanted as? [String: Any] else {
                continue
            }
            let type = JSON.string(wanted, "type")
            let message = got[index]
            let name = String(cString: vs_msg_name(message.kind))
            if name != type {
                XCTFail("\(at) : message \(index) = \(name), attendu \(type)")
                continue
            }
            let data = JSON.child(wanted, "data")
            switch name {
            case "ping":
                XCTAssertEqual(message.t, JSON.int(data, "t"), "\(at) : ping t")
            case "setReady":
                XCTAssertEqual(message.ready != 0, JSON.bool(data, "ready"), "\(at) : setReady")
            case "setFile":
                XCTAssertEqual(text(message.name), JSON.string(data, "name"), "\(at) : setFile name")
                assertClose(message.duration_sec, JSON.number(data, "durationSec"),
                            "\(at) : setFile durationSec")
                XCTAssertEqual(message.size_bytes, JSON.int(data, "sizeBytes"), "\(at) : setFile sizeBytes")
            case "control":
                XCTAssertEqual(String(cString: vs_action_name(message.action)),
                               JSON.string(data, "action"), "\(at) : control action")
                assertClose(message.position_sec, JSON.number(data, "positionSec"),
                            "\(at) : control positionSec")
            case "report":
                assertClose(message.position_sec, JSON.number(data, "positionSec"),
                            "\(at) : report positionSec")
                XCTAssertEqual(message.paused != 0, JSON.bool(data, "paused"), "\(at) : report paused")
                XCTAssertEqual(message.buffering != 0, JSON.bool(data, "buffering"),
                               "\(at) : report buffering")
            case "chat":
                XCTAssertEqual(text(message.text), JSON.string(data, "text"), "\(at) : chat")
            default:
                XCTFail("\(at) : message inattendu \(name)")
            }
        }
    }
}
