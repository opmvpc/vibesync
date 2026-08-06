// ProtocolTests.swift — encodage/décodage du protocole et jeton de session.
// Les charges utiles sont reprises de la référence Go et du harnais C
// (core/tests/test_core.c §protocol).

import XCTest
@testable import VibeSync

final class ProtocolTests: XCTestCase {

    // MARK: hello

    func testHelloOmitsEmptyFields() {
        let raw = Proto.encodeHello(name: "thib", room: "salon", password: "", session: "")
        guard let root = JSON.object(JSON.parse(raw)) else {
            XCTFail("hello illisible : \(raw)")
            return
        }
        XCTAssertEqual(JSON.string(root, "type"), "hello")
        guard let data = JSON.child(root, "data") else {
            XCTFail("data absent du hello")
            return
        }
        XCTAssertEqual(JSON.int(data, "version"), 1)
        XCTAssertEqual(JSON.string(data, "name"), "thib")
        XCTAssertEqual(JSON.string(data, "room"), "salon")
        XCTAssertNil(data["password"], "mot de passe vide non omis")
        XCTAssertNil(data["session"], "session vide non omise")
    }

    func testHelloCarriesPasswordAndSession() {
        let token = Proto.sessionToken()
        let raw = Proto.encodeHello(name: "thib", room: "salon", password: "s3cr3t", session: token)
        let data = JSON.child(JSON.object(JSON.parse(raw)), "data")
        XCTAssertEqual(JSON.string(data, "password"), "s3cr3t")
        XCTAssertEqual(JSON.string(data, "session").count, sessionTokenLength)
    }

    func testSessionTokenIsHexAndRandom() {
        let a = Proto.sessionToken()
        let b = Proto.sessionToken()
        XCTAssertEqual(a.count, sessionTokenLength)
        XCTAssertNotEqual(a, b, "jeton constant !")
        for character in a {
            XCTAssertTrue(character.isHexDigit && character.lowercased() == String(character),
                          "caractère non hexadécimal minuscule : \(character)")
        }
    }

    // MARK: Messages sortants

    func testEncodeMessages() {
        var data = JSON.child(JSON.object(JSON.parse(Proto.encode(.control(action: .seek, positionSec: 42.5)))), "data")
        XCTAssertEqual(JSON.string(data, "action"), "seek")
        XCTAssertEqual(JSON.number(data, "positionSec"), 42.5)

        // La précision du flottant ne doit pas être perdue à l'encodage.
        data = JSON.child(JSON.object(JSON.parse(Proto.encode(
            .report(positionSec: 101.00000000000001, paused: true, buffering: false)))), "data")
        XCTAssertEqual(JSON.number(data, "positionSec"), 101.00000000000001)
        XCTAssertTrue(JSON.bool(data, "paused"))
        XCTAssertFalse(JSON.bool(data, "buffering", true))

        data = JSON.child(JSON.object(JSON.parse(Proto.encode(
            .setFile(name: "ep1 \"guillemets\".mkv", durationSec: 1200, sizeBytes: 15)))), "data")
        XCTAssertEqual(JSON.string(data, "name"), "ep1 \"guillemets\".mkv")
        XCTAssertEqual(JSON.number(data, "durationSec"), 1200)
        XCTAssertEqual(JSON.int(data, "sizeBytes"), 15)

        data = JSON.child(JSON.object(JSON.parse(Proto.encode(.ping(t: 1785960002000)))), "data")
        XCTAssertEqual(JSON.int(data, "t"), 1785960002000)

        data = JSON.child(JSON.object(JSON.parse(Proto.encode(.chat(text: "salut 😀\nligne")))), "data")
        XCTAssertEqual(JSON.string(data, "text"), "salut 😀\nligne")

        data = JSON.child(JSON.object(JSON.parse(Proto.encode(.setReady(ready: true)))), "data")
        XCTAssertTrue(JSON.bool(data, "ready"))
    }

    // MARK: Messages entrants

    func testDecodeWelcome() {
        let raw = """
        {"type":"welcome","data":{"selfId":"u1","room":"salon",\
        "state":{"paused":false,"positionSec":100,"rate":1,"refServerMs":1785960000000,"setBy":"u2"},\
        "users":[{"id":"u1","name":"thib","ready":true,\
        "file":{"name":"ep1.mkv","durationSec":1200,"sizeBytes":15}},{"id":"u2","name":"ami"}],\
        "serverVersion":"0.3.0","downloadUrl":"https://exemple.fr/releases"}}
        """
        guard let message = Proto.decode(raw) else {
            XCTFail("welcome non décodé")
            return
        }
        guard case .welcome(let w) = message else {
            XCTFail("welcome non reconnu")
            return
        }
        XCTAssertEqual(w.selfId, "u1")
        XCTAssertEqual(w.room, "salon")
        XCTAssertEqual(w.state?.positionSec, 100)
        XCTAssertEqual(w.state?.refServerMs, 1785960000000)
        XCTAssertEqual(w.state?.setBy, "u2")
        XCTAssertEqual(w.users.count, 2)
        XCTAssertEqual(w.selfReady, true)
        XCTAssertTrue(w.users[0].hasFile)
        XCTAssertEqual(w.users[0].fileDurationSec, 1200)
        // VS-023 : champs additifs de version.
        XCTAssertEqual(w.serverVersion, "0.3.0")
        XCTAssertEqual(w.downloadUrl, "https://exemple.fr/releases")
    }

    func testDecodeOtherMessages() {
        guard case .pong(let p)? = Proto.decode("{\"type\":\"pong\",\"data\":{\"t\":10,\"serverMs\":20}}") else {
            XCTFail("pong")
            return
        }
        XCTAssertEqual(p.t, 10)
        XCTAssertEqual(p.serverMs, 20)

        let rsRaw = "{\"type\":\"roomState\",\"data\":{\"paused\":true,\"positionSec\":12.5," +
                    "\"rate\":1,\"refServerMs\":5,\"setBy\":\"u3\"}}"
        guard case .roomState(let rs)? = Proto.decode(rsRaw) else {
            XCTFail("roomState")
            return
        }
        XCTAssertTrue(rs.paused)
        XCTAssertEqual(rs.positionSec, 12.5)

        guard case .toast(let level, _)? =
            Proto.decode("{\"type\":\"toast\",\"data\":{\"level\":\"warn\",\"text\":\"fichiers différents\"}}") else {
            XCTFail("toast")
            return
        }
        XCTAssertEqual(level, "warn")

        guard case .chatEvent(let from, _, let serverMs)? =
            Proto.decode("{\"type\":\"chatEvent\",\"data\":{\"from\":\"ami\",\"text\":\"coucou\",\"serverMs\":42}}") else {
            XCTFail("chatEvent")
            return
        }
        XCTAssertEqual(from, "ami")
        XCTAssertEqual(serverMs, 42)

        guard case .error(let code, _)? = Proto.decode("{\"type\":\"error\",\"data\":{\"code\":\"bad_password\"}}") else {
            XCTFail("error")
            return
        }
        XCTAssertTrue(Proto.isFatal(code))
        XCTAssertFalse(Proto.isFatal("protocol"))
    }

    func testDecodeRobustness() {
        // Forward-compat : un type inconnu est ignoré, pas fatal.
        guard case .unknown(let type)? = Proto.decode("{\"type\":\"futur\",\"data\":{\"x\":1}}") else {
            XCTFail("message inconnu")
            return
        }
        XCTAssertEqual(type, "futur")

        XCTAssertNil(Proto.decode("pas du json"))
        XCTAssertNil(Proto.decode("{\"data\":{}}"))
        XCTAssertNil(Proto.decode("[]"))
        XCTAssertNil(Proto.decode(""))

        // data absent : ne doit pas planter.
        guard case .welcome(let w)? = Proto.decode("{\"type\":\"welcome\"}") else {
            XCTFail("welcome sans data")
            return
        }
        XCTAssertNil(w.state)
        XCTAssertTrue(w.users.isEmpty)
        XCTAssertTrue(w.serverVersion.isEmpty)
    }

    // MARK: Assainissement du moteur

    func testSanitizeRoomState() {
        var rs = RoomState(paused: true, positionSec: 0, rate: 1, refServerMs: 0, setBy: "")
        XCTAssertNotNil(Engine.sanitize(rs), "état en pause valide refusé")

        rs.paused = false
        XCTAssertNil(Engine.sanitize(rs), "lecture sans référence acceptée")

        rs.refServerMs = 1785960000000
        XCTAssertNotNil(Engine.sanitize(rs), "état en lecture valide refusé")

        rs.rate = 8
        XCTAssertNil(Engine.sanitize(rs), "rate hors [0,25 ; 4] accepté")
        rs.rate = 0.1
        XCTAssertNil(Engine.sanitize(rs), "rate trop faible accepté")

        rs.rate = 1
        rs.positionSec = -1
        XCTAssertNil(Engine.sanitize(rs), "position négative acceptée")
        rs.positionSec = Double.infinity
        XCTAssertNil(Engine.sanitize(rs), "position infinie acceptée")

        XCTAssertEqual(Engine.clampPosition(-5, 100), 0)
        XCTAssertEqual(Engine.clampPosition(500, 100), 100)
        XCTAssertEqual(Engine.clampPosition(500, 0), 500)
    }

    func testBackoffAndOffsetMedian() {
        var backoff = Sync.backoffMin
        XCTAssertEqual(Sync.nextBackoff(backoff), 2 * Sync.backoffMin)
        for _ in 0..<10 {
            backoff = Sync.nextBackoff(backoff)
        }
        XCTAssertEqual(backoff, Sync.backoffMax)

        // Médiane glissante des 5 dernières mesures : un RTT aberrant est ignoré.
        var engine = Engine()
        let base: Nanos = 1785960000000 * 1_000_000
        let deltas: [Int64] = [1000, 1200, 800, 60000, 1100]
        for (index, delta) in deltas.enumerated() {
            let now = base + Int64(index) * 2000 * 1_000_000
            let ms = VSTime.toUnixMs(now)
            engine.onPong(now: now, Pong(t: ms - 100, serverMs: ms + delta - 50))
        }
        XCTAssertEqual(engine.offsetMs, 1100)
        XCTAssertEqual(engine.latencyMs, 50)

        for index in 0..<5 {
            let now = base + Int64(10 + index) * 1000 * 1_000_000
            let ms = VSTime.toUnixMs(now)
            engine.onPong(now: now, Pong(t: ms, serverMs: ms + 500))
        }
        XCTAssertEqual(engine.offsetMs, 500, "les vieilles mesures ne sont pas oubliées")
    }

    func testSessionLostInvalidatesReference() {
        var engine = Engine()
        engine.onWelcome(now: VSTime.now(),
                         selfId: "u1",
                         room: "salon",
                         state: RoomState(paused: true, positionSec: 10, rate: 1, refServerMs: 1, setBy: "u2"))
        XCTAssertTrue(engine.haveState)
        engine.sessionLost()
        XCTAssertFalse(engine.haveState, "référence non invalidée à la coupure")
        XCTAssertFalse(engine.haveOffset)
    }

    // MARK: Écriture JSON

    func testJSONWriterEscapes() {
        let value = JSONVal.obj([
            ("a", .str("guillemet \" antislash \\ saut\nligne\ttab")),
            ("b", .num(0.95)),
            ("c", .num(1200)),
            ("d", .bool(false)),
            ("e", .null),
        ])
        guard let back = JSON.object(JSON.parse(value.encoded)) else {
            XCTFail("relecture impossible : \(value.encoded)")
            return
        }
        XCTAssertEqual(JSON.string(back, "a"), "guillemet \" antislash \\ saut\nligne\ttab")
        XCTAssertEqual(JSON.number(back, "b"), 0.95)
        XCTAssertEqual(JSON.number(back, "c"), 1200)
        XCTAssertFalse(JSON.bool(back, "d", true))
    }
}
