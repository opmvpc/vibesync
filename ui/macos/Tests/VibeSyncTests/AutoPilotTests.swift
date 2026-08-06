// AutoPilotTests.swift — mode auto (harnais scripts/run-real-macos.sh).
//
// Ce qui est testable sans interface ni réseau : la lecture de
// l'environnement, l'analyse des commandes, et l'isolation du magasin de
// réglages entre deux instances.

import XCTest
@testable import VibeSync

final class AutoPilotTests: XCTestCase {

    // MARK: Lecture de l'environnement

    func testDisabledWithoutURL() {
        XCTAssertNil(AutoPilot.fromEnvironment([:]),
                     "sans VIBESYNC_AUTO_URL, l'application doit rester normale")
        XCTAssertNil(AutoPilot.fromEnvironment(["VIBESYNC_AUTO_NAME": "thib",
                                                "VIBESYNC_AUTO_FILE": "/tmp/a.wav"]),
                     "les autres variables seules n'activent rien")
        XCTAssertNil(AutoPilot.fromEnvironment(["VIBESYNC_AUTO_URL": "   "]),
                     "une URL vide n'active rien")
    }

    func testReadsEnvironment() {
        guard let auto = AutoPilot.fromEnvironment([
            "VIBESYNC_AUTO_URL": " wss://exemple.fr/ws ",
            "VIBESYNC_AUTO_NAME": "alice",
            "VIBESYNC_AUTO_ROOM": "vibesync-test-42",
            "VIBESYNC_AUTO_PASSWORD": " secret ",
            "VIBESYNC_AUTO_FILE": "/tmp/a.wav",
            "VIBESYNC_AUTO_STATUS": "/tmp/a.json",
            "VIBESYNC_AUTO_CMDS": "/tmp/a.cmds",
            "VIBESYNC_AUTO_SCENARIO": "driver",
        ]) else {
            XCTFail("pilote non activé")
            return
        }
        XCTAssertEqual(auto.url, "wss://exemple.fr/ws")
        XCTAssertEqual(auto.name, "alice")
        XCTAssertEqual(auto.room, "vibesync-test-42")
        XCTAssertEqual(auto.password, " secret ",
                       "le mot de passe n'est pas rogné : un espace en fait partie")
        XCTAssertEqual(auto.file, "/tmp/a.wav")
        XCTAssertEqual(auto.statusPath, "/tmp/a.json")
        XCTAssertEqual(auto.commandsPath, "/tmp/a.cmds")
        XCTAssertEqual(auto.scenario, "driver")
    }

    func testDefaults() {
        guard let auto = AutoPilot.fromEnvironment(["VIBESYNC_AUTO_URL": "ws://127.0.0.1:8080/ws"]) else {
            XCTFail("pilote non activé")
            return
        }
        XCTAssertEqual(auto.name, "auto")
        XCTAssertEqual(auto.room, "salon")
        XCTAssertTrue(auto.password.isEmpty)
        XCTAssertTrue(auto.file.isEmpty)
        XCTAssertTrue(auto.statusPath.isEmpty)
        XCTAssertTrue(auto.commandsPath.isEmpty)
    }

    // MARK: Commandes

    func testParseCommands() {
        XCTAssertEqual(AutoPilot.parse("play"), .play)
        XCTAssertEqual(AutoPilot.parse("  PAUSE  "), .pause)
        XCTAssertEqual(AutoPilot.parse("seek 42.5"), .seek(42.5))
        XCTAssertEqual(AutoPilot.parse("seek   7"), .seek(7))
        XCTAssertEqual(AutoPilot.parse("ready"), .ready(true))
        XCTAssertEqual(AutoPilot.parse("ready 0"), .ready(false))
        XCTAssertEqual(AutoPilot.parse("unready"), .ready(false))
        XCTAssertEqual(AutoPilot.parse("chat coucou tout le monde"), .chat("coucou tout le monde"))
        XCTAssertEqual(AutoPilot.parse("open /tmp/a.wav"), .openFile("/tmp/a.wav"))
        XCTAssertEqual(AutoPilot.parse("quit"), .quit)

        // Ignorés plutôt que fatals : le fichier est écrit par un script shell.
        XCTAssertNil(AutoPilot.parse(""))
        XCTAssertNil(AutoPilot.parse("   "))
        XCTAssertNil(AutoPilot.parse("# commentaire"))
        XCTAssertNil(AutoPilot.parse("danse"))
        XCTAssertNil(AutoPilot.parse("seek"), "un seek sans position n'est pas une commande")
        XCTAssertNil(AutoPilot.parse("seek beaucoup"))
        XCTAssertNil(AutoPilot.parse("chat"))
    }

    // MARK: Isolation des instances

    func testSuiteIsolatesInstances() {
        let suiteA = "vibesync.test.\(UUID().uuidString)"
        let suiteB = "vibesync.test.\(UUID().uuidString)"
        defer {
            UserDefaults.standard.removePersistentDomain(forName: suiteA)
            UserDefaults.standard.removePersistentDomain(forName: suiteB)
        }
        let a = Preferences.store([Preferences.suiteEnv: suiteA])
        let b = Preferences.store([Preferences.suiteEnv: suiteB])

        // Ce qui compte vraiment : deux instances sur la même machine ne
        // doivent PAS présenter le même jeton de session au serveur (VS-028).
        let tokenA = Preferences.sessionToken(a)
        let tokenB = Preferences.sessionToken(b)
        XCTAssertNotEqual(tokenA, tokenB, "les deux instances partagent leur jeton de session")
        XCTAssertEqual(Preferences.sessionToken(a), tokenA, "le jeton doit rester stable dans sa suite")

        // Sans la variable, on retombe sur les réglages normaux.
        XCTAssertTrue(Preferences.store([:]) === UserDefaults.standard)
        XCTAssertTrue(Preferences.store([Preferences.suiteEnv: "  "]) === UserDefaults.standard)
    }
}
