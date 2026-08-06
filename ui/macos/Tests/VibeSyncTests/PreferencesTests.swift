// PreferencesTests.swift — réglages persistants, versions, recherche de média.
//
// Rien de ce qui est testé ici ne touche au profil de l'utilisateur : le
// magasin de réglages est injecté (MemoryStore) et la recherche travaille dans
// une arborescence temporaire. Le trousseau (Keychain.swift) n'est pas testé :
// il demanderait un accès réel au trousseau de la session, ce qui n'a rien à
// faire dans une suite automatique — il est vérifié à la main (VS-025).

import XCTest
@testable import VibeSync

/// Magasin de réglages en mémoire.
final class MemoryStore: PrefStore {
    var values: [String: Any] = [:]

    func string(forKey key: String) -> String? {
        return values[key] as? String
    }

    func stringArray(forKey key: String) -> [String]? {
        return values[key] as? [String]
    }

    func object(forKey key: String) -> Any? {
        return values[key]
    }

    func set(_ value: Any?, forKey key: String) {
        if let value = value {
            values[key] = value
        } else {
            values.removeValue(forKey: key)
        }
    }
}

final class PreferencesTests: XCTestCase {

    // MARK: Jeton de session (VS-028)

    func testSessionTokenIsGeneratedOnceThenReused() {
        let store = MemoryStore()
        let first = Preferences.sessionToken(store)
        XCTAssertEqual(first.count, sessionTokenLength)
        XCTAssertTrue(Preferences.validSessionToken(first))
        XCTAssertEqual(store.string(forKey: Preferences.keySession), first)

        // Un second processus (même magasin) doit retrouver LE MÊME jeton :
        // c'est tout l'objet de VS-028.
        XCTAssertEqual(Preferences.sessionToken(store), first)

        // Un magasin neuf en tire un autre.
        XCTAssertNotEqual(Preferences.sessionToken(MemoryStore()), first)
    }

    func testInvalidSessionTokenIsReplaced() {
        for bad in ["", "zz", "pas-de-l-hexa", String(repeating: "a", count: 15),
                    String(repeating: "ab", count: 100)] {
            XCTAssertFalse(Preferences.validSessionToken(bad), "« \(bad) » accepté à tort")
            let store = MemoryStore()
            store.set(bad, forKey: Preferences.keySession)
            let token = Preferences.sessionToken(store)
            XCTAssertTrue(Preferences.validSessionToken(token))
            XCTAssertNotEqual(token, bad)
            XCTAssertEqual(store.string(forKey: Preferences.keySession), token, "le neuf n'est pas persisté")
        }
        // Un jeton plus long que 16 octets mais valide est conservé tel quel.
        let long = String(repeating: "ab", count: 32)
        XCTAssertTrue(Preferences.validSessionToken(long))
        let store = MemoryStore()
        store.set(long, forKey: Preferences.keySession)
        XCTAssertEqual(Preferences.sessionToken(store), long)
    }

    // MARK: Mot de passe mémorisé (VS-025)

    func testRememberPasswordDefaultsToTrue() {
        let store = MemoryStore()
        XCTAssertTrue(Preferences.rememberPassword(store), "cochée par défaut, comme sur Windows")
        Preferences.setRememberPassword(false, store)
        XCTAssertFalse(Preferences.rememberPassword(store))
        Preferences.setRememberPassword(true, store)
        XCTAssertTrue(Preferences.rememberPassword(store))
    }

    // MARK: Dossiers médias (VS-026)

    func testMediaDirsRoundTrip() {
        let store = MemoryStore()
        Preferences.setMediaDirs(["/a", " /b ", "", "/a", "/c"], store)
        XCTAssertEqual(Preferences.mediaDirs(store), ["/a", "/b", "/c"],
                       "rognage, vides et doublons écartés")

        let many = (0..<20).map { "/d\($0)" }
        Preferences.setMediaDirs(many, store)
        XCTAssertEqual(Preferences.mediaDirs(store).count, Preferences.maxMediaDirs)

        // Rien d'enregistré : le dossier Téléchargements par défaut.
        XCTAssertEqual(Preferences.mediaDirs(MemoryStore()), Preferences.defaultMediaDirs())
    }

    func testMediaSearch() throws {
        let fm = FileManager.default
        let root = fm.temporaryDirectory.appendingPathComponent("vibesync-media-\(UUID().uuidString)")
        let nested = root.appendingPathComponent("films/saison 1")
        try fm.createDirectory(at: nested, withIntermediateDirectories: true)
        try Data(repeating: 0, count: 10).write(to: root.appendingPathComponent("Ep1.mkv"))
        try Data(repeating: 0, count: 4096).write(to: nested.appendingPathComponent("ep1.mkv"))
        try Data(repeating: 0, count: 7).write(to: nested.appendingPathComponent("autre.mkv"))
        defer { try? fm.removeItem(at: root) }

        // Nom exact, casse indifférente ; à égalité de nom, le plus gros gagne.
        let hit = MediaLibrary.find(name: "EP1.MKV", in: [root.path])
        XCTAssertTrue(hit.found)
        XCTAssertEqual(hit.matches, 2)
        XCTAssertEqual(hit.sizeBytes, 4096)
        // /var est un lien vers /private/var : on compare les composants utiles.
        XCTAssertTrue(hit.path.hasSuffix("films/saison 1/ep1.mkv"), hit.path)
        XCTAssertFalse(hit.truncated)

        // Correspondance partielle : ce n'est pas une correspondance.
        XCTAssertFalse(MediaLibrary.find(name: "ep1", in: [root.path]).found)
        XCTAssertFalse(MediaLibrary.find(name: "ep2.mkv", in: [root.path]).found)

        // Entrées vides : aucune recherche, aucun plantage.
        XCTAssertFalse(MediaLibrary.find(name: "", in: [root.path]).found)
        XCTAssertFalse(MediaLibrary.find(name: "ep1.mkv", in: []).found)
        XCTAssertFalse(MediaLibrary.find(name: "ep1.mkv", in: ["/ce/dossier/n/existe/pas"]).found)
    }

    func testMediaSearchDepthIsBounded() throws {
        let fm = FileManager.default
        let root = fm.temporaryDirectory.appendingPathComponent("vibesync-deep-\(UUID().uuidString)")
        var deep = root
        for i in 0..<(MediaLibrary.maxDepth + 3) {
            deep = deep.appendingPathComponent("n\(i)")
        }
        try fm.createDirectory(at: deep, withIntermediateDirectories: true)
        try Data(repeating: 0, count: 3).write(to: deep.appendingPathComponent("loin.mkv"))
        defer { try? fm.removeItem(at: root) }

        XCTAssertFalse(MediaLibrary.find(name: "loin.mkv", in: [root.path]).found,
                       "au-delà de la profondeur maximale, on ne descend plus")
    }

    // MARK: Versions (VS-023)

    func testNewerVersion() {
        // Mêmes cas que internal/client/version_test.go.
        let cases: [(String, String, Bool)] = [
            ("1.2.3", "1.2.3", false),
            ("1.2.4", "1.2.3", true),
            ("1.2.2", "1.2.3", false),
            ("1.3.0", "1.2.9", true),
            ("2.0.0", "1.99.99", true),
            ("1.0.0", "2.0.0", false),
            ("0.3", "0.2.9", true),
            ("0.2", "0.2.0", false),
            ("v1.2.4", "1.2.3", true),
            ("1.2.4-rc1", "1.2.3", true),
            ("1.2.3-rc1", "1.2.3", false),
            ("1.2.3", "1.2.3-rc1", true),
            ("1.2.3-rc2", "1.2.3-rc1", false),
            ("1.2.2-rc1", "1.2.3", false),
            ("1.2.3-", "1.2.3", false),
            ("1.2.3+build7", "1.2.3", false),
            ("1.2.4+build7", "1.2.3", true),
            ("1.2.3+b", "1.2.3-rc1", true),
            ("1.10.0", "1.9.0", true),
            ("9.9.9", "dev", false),
            ("dev", "1.0.0", false),
            ("", "1.0.0", false),
            ("1.0.0", "", false),
            ("on-verra-plus-tard", "1.0.0", false),
            ("1.2.3.4", "1.0.0", false),
            ("1..3", "1.0.0", false),
            ("-1.2.3", "1.0.0", false),
            ("99999999999999999999.0.0", "1.0.0", false),
            ("  1.2.4  ", "1.2.3", true),
            // Revue terra : TrimSpace coupe aussi les sauts de ligne — un
            // VERSION lu tel quel dans un fichier ne doit pas devenir illisible.
            ("1.2.4\n", "1.2.3", true),
            ("\t1.2.4\r\n", "1.2.3", true),
            ("1.2.3", "1.2.3\n", false),
            ("1.2.4", "\n1.2.3\n", true),
            // Un « + » en tête est coupé comme métadonnées de build : il ne
            // reste rien à analyser, donc pas de bannière (comme Go).
            ("+1.2.4", "1.2.3", false),
            ("1.+2.4", "1.2.3", false),
        ]
        for (remote, local, want) in cases {
            XCTAssertEqual(AppVersion.newer(remote, than: local), want,
                           "newer(\(remote), \(local))")
        }
    }

    func testNewerVersionIsAntisymmetric() {
        let versions = ["0.0.1", "0.1.0", "0.2.0", "1.0.0", "1.0.1", "1.2.3-rc1", "1.2.3", "v2.0.0"]
        for a in versions {
            for b in versions {
                XCTAssertFalse(AppVersion.newer(a, than: b) && AppVersion.newer(b, than: a),
                               "\(a) et \(b) chacune plus récente que l'autre")
            }
        }
    }
}
