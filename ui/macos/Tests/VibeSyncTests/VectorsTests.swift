// VectorsTests.swift — rejeu des vecteurs de conformité test/vectors/*.json.
//
// C'est LE contrat du moteur : pour le même `initialVLC` et les mêmes
// `events`, l'implémentation Swift doit produire les mêmes `vlcCommands` et
// `toServer` aux mêmes instants que la référence Go et que le port C.
//
// Répertoire des vecteurs : variable d'environnement VIBESYNC_VECTORS, sinon
// ../../test/vectors relatif au répertoire courant (racine du paquet), sinon
// déduit de l'emplacement de ce fichier.

import XCTest
@testable import VibeSync

final class VectorsTests: XCTestCase {

    /// Taille du fichier vidéo factice écrit par le générateur de vecteurs
    /// (vectors_test.go : []byte("données vidéo") = 15 octets UTF-8).
    private let vectorFileSize: Int64 = 15
    /// Origine de l'horloge simulée (vlctest.NewClock : 2026-08-05 20:00 UTC).
    private let baseMs: Int64 = 1785960000000
    private let tolerance: Double = 1e-3

    // MARK: - Localisation des vecteurs

    private func vectorsDirectory() -> URL? {
        let fm = FileManager.default
        var candidates: [String] = []
        if let forced = ProcessInfo.processInfo.environment["VIBESYNC_VECTORS"], !forced.isEmpty {
            candidates.append(forced)
        }
        candidates.append("../../test/vectors")
        candidates.append("test/vectors")

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

    func testConformanceVectors() throws {
        guard let dir = vectorsDirectory() else {
            XCTFail("répertoire test/vectors introuvable (positionnez VIBESYNC_VECTORS)")
            return
        }
        let names = try FileManager.default
            .contentsOfDirectory(atPath: dir.path)
            .filter { $0.hasSuffix(".json") }
            .sorted()
        XCTAssertGreaterThanOrEqual(names.count, 13,
                                    "\(names.count) vecteur(s) dans \(dir.path), attendu au moins 13")
        for name in names {
            replay(dir.appendingPathComponent(name))
        }
    }

    private func replay(_ url: URL) {
        let label = url.lastPathComponent
        guard let text = try? String(contentsOf: url, encoding: .utf8),
              let root = JSON.object(JSON.parse(text)) else {
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

        var engine = Engine()
        // Les sorties de l'ouverture sont drainées par le générateur.
        engine.openFile(now: now, name: fileName, sizeBytes: vectorFileSize)

        var eventIndex = 0
        // keepOutput : par défaut, ce que le moteur émet en réaction immédiate à
        // un événement ne fait pas partie de la trace (le générateur Go le
        // draine). Un événement marqué « keepOutput » garde cette réaction, qui
        // est alors attendue en tête du `toServer` du premier pas qui suit —
        // c'est le cas quand la réaction EST la règle testée (reprise « salle
        // vierge »).
        var pending: [Decision] = []
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
                    engine.sessionLost()
                case "userPause":
                    fake.pause(now)
                case "userPlay":
                    fake.play(now)
                case "userSeek":
                    fake.seek(JSON.number(data, "positionSec", 0), now)
                default:
                    switch Proto.fill(type: type, data: data) {
                    case .welcome(let w):
                        pending += engine.onWelcome(now: now,
                                                    selfId: w.selfId,
                                                    room: w.room,
                                                    state: w.state)
                    case .pong(let p):
                        engine.onPong(now: now, p)
                    case .roomState(let rs):
                        engine.onRoomState(now: now, rs)
                    default:
                        break
                    }
                }
                if JSON.bool(event, "keepOutput") {
                    keepPending = true
                } else if !keepPending {
                    pending.removeAll()
                }
                eventIndex += 1
            }

            // Un poll : lecture de VLC, ingestion, décisions.
            now += pollMs * 1_000_000
            let elapsedMs = (now - baseMs * 1_000_000) / 1_000_000
            XCTAssertEqual(elapsedMs, stepMs,
                           "\(vectorName) : désynchronisation d'horloge (\(elapsedMs) ms vs \(stepMs) ms)")

            guard let status = VLCStatusParser.parse(fake.statusJSON(now)) else {
                XCTFail("\(vectorName) : status.json simulé illisible")
                return
            }
            if !keepPending {
                pending.removeAll()
            }
            keepPending = false
            var out = pending
            pending.removeAll()
            out += engine.onVLCStatus(now: now, status)
            out += engine.onTick(now: now)

            let at = "\(vectorName) @\(stepMs)ms"
            XCTAssertEqual(engine.status.state.rawValue, JSON.string(step, "vlcState"),
                           "\(at) : état VLC")
            assertClose(engine.status.positionSec, JSON.number(step, "vlcPositionSec"),
                        "\(at) : position VLC")
            assertClose(engine.expectedPosition(now), JSON.number(step, "expectedPositionSec"),
                        "\(at) : position attendue")
            assertClose(engine.drift, JSON.number(step, "driftSec"), "\(at) : drift")
            checkCommands(at, JSON.array(step, "vlcCommands"), out.vlcCommands)
            checkMessages(at, JSON.array(step, "toServer"), out.serverMessages)

            for cmd in out.vlcCommands {
                fake.apply(cmd, now)
            }
        }
    }

    // MARK: - Comparaisons

    private func assertClose(_ got: Double, _ want: Double, _ message: String) {
        if abs(got - want) > tolerance {
            XCTFail("\(message) : \(got), attendu \(want)")
        }
    }

    private func checkCommands(_ at: String, _ want: [Any], _ got: [VLCCommand]) {
        if want.count != got.count {
            let names = got.map { $0.kind.rawValue }.joined(separator: ", ")
            XCTFail("\(at) : \(got.count) commande(s) VLC [\(names)], attendu \(want.count)")
            return
        }
        for (index, rawWanted) in want.enumerated() {
            guard let wanted = rawWanted as? [String: Any] else {
                continue
            }
            let expectedKind = JSON.string(wanted, "cmd")
            let command = got[index]
            if command.kind.rawValue != expectedKind {
                XCTFail("\(at) : commande \(index) = \(command.kind.rawValue), attendu \(expectedKind)")
                continue
            }
            guard let wantedValue = (wanted["value"] as? NSNumber)?.doubleValue else {
                continue
            }
            // La valeur consignée est celle envoyée à VLC : seek arrondi à la
            // seconde, rate au millième.
            let sent = command.kind == .seek ? command.value.rounded() : command.value
            assertClose(sent, wantedValue, "\(at) : valeur de \(expectedKind)")
        }
    }

    private func checkMessages(_ at: String, _ want: [Any], _ got: [ClientMessage]) {
        if want.count != got.count {
            let names = got.map { $0.type }.joined(separator: ", ")
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
            if message.type != type {
                XCTFail("\(at) : message \(index) = \(message.type), attendu \(type)")
                continue
            }
            let data = JSON.child(wanted, "data")
            switch message {
            case .ping(let t):
                XCTAssertEqual(t, JSON.int(data, "t"), "\(at) : ping t")
            case .setReady(let ready):
                XCTAssertEqual(ready, JSON.bool(data, "ready"), "\(at) : setReady")
            case .setFile(let name, let durationSec, let sizeBytes):
                XCTAssertEqual(name, JSON.string(data, "name"), "\(at) : setFile name")
                assertClose(durationSec, JSON.number(data, "durationSec"), "\(at) : setFile durationSec")
                XCTAssertEqual(sizeBytes, JSON.int(data, "sizeBytes"), "\(at) : setFile sizeBytes")
            case .control(let action, let positionSec):
                XCTAssertEqual(action.rawValue, JSON.string(data, "action"), "\(at) : control action")
                assertClose(positionSec, JSON.number(data, "positionSec"), "\(at) : control positionSec")
            case .report(let positionSec, let paused, let buffering):
                assertClose(positionSec, JSON.number(data, "positionSec"), "\(at) : report positionSec")
                XCTAssertEqual(paused, JSON.bool(data, "paused"), "\(at) : report paused")
                XCTAssertEqual(buffering, JSON.bool(data, "buffering"), "\(at) : report buffering")
            case .chat(let text):
                XCTAssertEqual(text, JSON.string(data, "text"), "\(at) : chat")
            }
        }
    }
}
