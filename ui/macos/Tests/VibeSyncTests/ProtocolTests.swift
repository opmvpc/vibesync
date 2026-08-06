// ProtocolTests.swift — encodage/décodage du protocole et jeton de session.
// Les charges utiles sont reprises de la référence Go et du harnais C
// (core/tests/test_core.c §protocol).
//
// Depuis VS-033 (phase 4 d'ADR-010) tout ce qui est vérifié ici passe par
// `core/src/protocol.c` : ce fichier ne teste plus une deuxième implémentation
// du protocole, il teste la FRONTIÈRE (les conversions Swift ↔ C) et fige le
// comportement du C commun tel que l'application le voit.

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
    }

    /// Lecture STRICTE (VS-033) : un type connu dont un champ obligatoire
    /// manque ou est mal typé est invalidé et ignoré, au lieu de se remplir de
    /// zéros silencieux. C'est la règle du C commun — donc déjà celle du client
    /// Windows (on_server_message jette `m->invalid`), et ce que le décodage
    /// Swift retiré ne faisait PAS : un `pong` vide devenait {t:0, serverMs:0}
    /// et empoisonnait l'offset d'horloge.
    func testDecodeRejectsIncompleteMessages() {
        // welcome sans data / sans selfId / sans état de salle recevable.
        XCTAssertNil(Proto.decode("{\"type\":\"welcome\"}"))
        XCTAssertNil(Proto.decode("{\"type\":\"welcome\",\"data\":{\"room\":\"salon\"}}"))
        XCTAssertNil(Proto.decode("{\"type\":\"welcome\",\"data\":{\"selfId\":\"u1\"}}"))

        XCTAssertNil(Proto.decode("{\"type\":\"pong\",\"data\":{}}"))
        XCTAssertNil(Proto.decode("{\"type\":\"pong\",\"data\":{\"t\":\"10\",\"serverMs\":20}}"))
        XCTAssertNil(Proto.decode("{\"type\":\"pong\",\"data\":{\"t\":-1,\"serverMs\":20}}"))

        // roomState en LECTURE sans référence : irrecevable (en pause, elle est
        // facultative — c'est la règle de docs/protocol.md).
        XCTAssertNil(Proto.decode("{\"type\":\"roomState\",\"data\":" +
                                  "{\"paused\":false,\"positionSec\":1,\"rate\":1}}"))
        XCTAssertNotNil(Proto.decode("{\"type\":\"roomState\",\"data\":" +
                                     "{\"paused\":true,\"positionSec\":1,\"rate\":1}}"))

        XCTAssertNil(Proto.decode("{\"type\":\"error\",\"data\":{\"text\":\"sans code\"}}"))
        XCTAssertNil(Proto.decode("{\"type\":\"toast\",\"data\":{\"level\":\"warn\"}}"))
        XCTAssertNil(Proto.decode("{\"type\":\"chatEvent\",\"data\":{\"from\":\"ami\"}}"))

        // `users` reste tolérant : c'est un rafraîchissement d'affichage. Les
        // entrées sans identifiant utilisable sont simplement écartées.
        guard case .users(let list)? = Proto.decode(
            "{\"type\":\"users\",\"data\":{\"users\":[{\"name\":\"sans id\"},{\"id\":\"u2\"}]}}") else {
            XCTFail("users")
            return
        }
        XCTAssertEqual(list.count, 1)
        XCTAssertEqual(list[0].id, "u2")
    }

    // MARK: Assainissement du moteur
    //
    // Depuis VS-032 ces règles sont celles du C commun ; ce qui est vérifié ici
    // est qu'elles restent atteignables — et identiques — depuis le wrapper.

    func testSanitizeRoomState() {
        var rs = RoomState(paused: true, positionSec: 0, rate: 1, refServerMs: 0, setBy: "")
        XCTAssertNotNil(CoreEngine.sanitize(rs), "état en pause valide refusé")

        rs.paused = false
        XCTAssertNil(CoreEngine.sanitize(rs), "lecture sans référence acceptée")

        rs.refServerMs = 1785960000000
        XCTAssertNotNil(CoreEngine.sanitize(rs), "état en lecture valide refusé")

        rs.rate = 8
        XCTAssertNil(CoreEngine.sanitize(rs), "rate hors [0,25 ; 4] accepté")
        rs.rate = 0.1
        XCTAssertNil(CoreEngine.sanitize(rs), "rate trop faible accepté")

        rs.rate = 1
        rs.positionSec = -1
        XCTAssertNil(CoreEngine.sanitize(rs), "position négative acceptée")
        rs.positionSec = Double.infinity
        XCTAssertNil(CoreEngine.sanitize(rs), "position infinie acceptée")

        // Règles que seul le C portait (le moteur Swift ne les avait pas) :
        // position déraisonnable et horodatage hors bornes epoch.
        rs.positionSec = 40_000_000
        XCTAssertNil(CoreEngine.sanitize(rs), "position au-delà d'un an de média acceptée")
        rs.positionSec = 10
        rs.refServerMs = 5_000_000_000_000
        XCTAssertNil(CoreEngine.sanitize(rs), "horodatage hors bornes accepté")

        // Aller-retour complet : les champs traversent la frontière intacts.
        rs.refServerMs = 1785960000000
        rs.setBy = "u42"
        guard let sane = CoreEngine.sanitize(rs) else {
            XCTFail("état valide refusé")
            return
        }
        XCTAssertEqual(sane.setBy, "u42")
        XCTAssertEqual(sane.positionSec, 10)
        XCTAssertEqual(sane.refServerMs, 1785960000000)
        XCTAssertFalse(sane.paused)

        XCTAssertEqual(CoreEngine.clampPosition(-5, 100), 0)
        XCTAssertEqual(CoreEngine.clampPosition(500, 100), 100)
        XCTAssertEqual(CoreEngine.clampPosition(500, 0), 500)
    }

    func testBackoffAndOffsetMedian() {
        // Doublement borné 1 s → 10 s (docs/protocol.md §Reconnexion).
        let second: Nanos = 1_000_000_000
        XCTAssertEqual(CoreEngine.nextBackoff(0), second, "premier délai différent de 1 s")
        var backoff = second
        XCTAssertEqual(CoreEngine.nextBackoff(backoff), 2 * second)
        for _ in 0..<10 {
            backoff = CoreEngine.nextBackoff(backoff)
        }
        XCTAssertEqual(backoff, 10 * second, "plafond différent de 10 s")

        // Médiane glissante des 5 dernières mesures : un RTT aberrant est ignoré.
        let engine = CoreEngine()
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
        let engine = CoreEngine()
        engine.connecting(room: "salon")
        XCTAssertEqual(engine.room, "salon")
        engine.onWelcome(now: VSTime.now(),
                         selfId: "u1",
                         state: RoomState(paused: true, positionSec: 10, rate: 1, refServerMs: 1, setBy: "u2"))
        XCTAssertTrue(engine.haveState)
        XCTAssertEqual(engine.selfId, "u1")
        XCTAssertEqual(engine.roomState.setBy, "u2")
        engine.sessionLost()
        XCTAssertFalse(engine.haveState, "référence non invalidée à la coupure")
        XCTAssertFalse(engine.haveOffset)
        XCTAssertTrue(engine.selfId.isEmpty)
    }

    // MARK: Frontière du wrapper (VS-032)

    /// Ce que le wrapper doit garantir et que les vecteurs ne montrent pas :
    /// les entrées de l'interface produisent les bons messages, et les chaînes
    /// traversent la frontière C sans se perdre (UTF-8 multi-octets compris).
    func testWrapperUserActionsAndStrings() {
        let engine = CoreEngine()
        engine.connecting(room: "salon")
        engine.onWelcome(now: VSTime.now(), selfId: "u1", state: nil)
        XCTAssertFalse(engine.haveState, "un welcome sans état de salle ne doit rien adopter")

        let opened = engine.openFile(name: "L'Été — 4K (accentué).mkv", sizeBytes: 4242)
        guard case .server(.setFile(let name, let duration, let size))? = opened.first else {
            XCTFail("openFile n'a pas déclaré le fichier : \(opened.count) décision(s)")
            return
        }
        XCTAssertEqual(name, "L'Été — 4K (accentué).mkv", "chaîne UTF-8 abîmée à la frontière")
        XCTAssertEqual(duration, 0)
        XCTAssertEqual(size, 4242)
        XCTAssertEqual(engine.fileName, "L'Été — 4K (accentué).mkv")
        XCTAssertFalse(engine.fileDeclared, "durée inconnue : le fichier n'est pas encore déclaré")

        let ready = engine.setReady(true)
        XCTAssertTrue(engine.ready)
        guard case .server(.setReady(let value))? = ready.first else {
            XCTFail("setReady sans message")
            return
        }
        XCTAssertTrue(value)

        let seek = engine.userControl(now: VSTime.now(), action: .seek, positionSec: 42)
        guard case .server(.control(let action, let position))? = seek.first else {
            XCTFail("userControl sans control")
            return
        }
        XCTAssertEqual(action, .seek)
        XCTAssertEqual(position, 42)

        let chat = engine.chat("coucou 👋")
        guard case .server(.chat(let text))? = chat.first else {
            XCTFail("chat en ligne non émis")
            return
        }
        XCTAssertEqual(text, "coucou 👋")

        // Statut VLC : l'instantané fait l'aller-retour, et la durée observée
        // déclare le fichier.
        var st = VLCStatus()
        st.state = .paused
        st.positionSec = 12.5
        st.lengthSec = 600
        st.rate = 1
        st.fileName = "L'Été — 4K (accentué).mkv"
        _ = engine.onVLCStatus(now: VSTime.now(), st)
        XCTAssertTrue(engine.haveStatus)
        XCTAssertEqual(engine.status.state, .paused)
        XCTAssertEqual(engine.status.positionSec, 12.5)
        XCTAssertEqual(engine.status.fileName, "L'Été — 4K (accentué).mkv")
        XCTAssertTrue(engine.fileDeclared)
        XCTAssertEqual(engine.fileDurationSec, 600)
    }

    /// File d'attente hors ligne : un chat composé sans session part au welcome
    /// suivant, dans l'ordre — capacité du C commun que le moteur Swift natif
    /// n'avait pas (docs/protocol.md §File d'attente hors ligne).
    func testWrapperQueuesOfflineChat() {
        let engine = CoreEngine()
        engine.connecting(room: "salon")
        XCTAssertTrue(engine.chat("un").isEmpty, "chat hors ligne envoyé quand même")
        XCTAssertTrue(engine.chat("deux").isEmpty)
        XCTAssertEqual(engine.pendingChats, ["un", "deux"])

        let out = engine.onWelcome(now: VSTime.now(), selfId: "u1", state: nil)
        let texts: [String] = out.serverMessages.compactMap {
            if case .chat(let text) = $0 {
                return text
            }
            return nil
        }
        XCTAssertEqual(texts, ["un", "deux"], "file rejouée dans le désordre")
        XCTAssertTrue(engine.pendingChats.isEmpty)

        // Départ volontaire : ce qui n'est pas parti ne partira pas.
        engine.disconnected()
        _ = engine.chat("trois")
        XCTAssertEqual(engine.pendingChats, ["trois"])
        engine.disconnected()
        XCTAssertTrue(engine.pendingChats.isEmpty, "file conservée après un départ volontaire")
    }

    // MARK: Écriture JSON

    /// Échappements de l'écrivain C (json.c) vus depuis l'application : ce qui
    /// part sur le fil doit se relire à l'identique, y compris les caractères
    /// que JSON interdit en clair.
    func testEncoderEscapes() {
        let hostile = "guillemet \" antislash \\ saut\nligne\ttab \u{0001} 😀"
        let raw = Proto.encode(.chat(text: hostile))
        guard let back = JSON.child(JSON.object(JSON.parse(raw)), "data") else {
            XCTFail("relecture impossible : \(raw)")
            return
        }
        XCTAssertEqual(JSON.string(back, "text"), hostile)

        // Nombres : la représentation la plus courte qui relit la même valeur,
        // et un entier reste un entier (1200, pas 1200.0).
        let file = Proto.encode(.setFile(name: "x", durationSec: 1200, sizeBytes: 15))
        XCTAssertTrue(file.contains("\"durationSec\":1200,"), file)
        let report = Proto.encode(.report(positionSec: 0.95, paused: false, buffering: false))
        XCTAssertTrue(report.contains("\"positionSec\":0.95,"), report)
    }
}
