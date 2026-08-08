// VLCPathTests.swift — résolution du chemin de VLC (parité avec le champ
// « Chemin de VLC » du client Windows).
//
// Tout ce qui est testé ici est pur : le disque est remplacé par un ensemble de
// chemins « exécutables », et l'environnement par un dictionnaire. Les seuls
// tests qui touchent au vrai disque sont ceux du bit d'exécution, à la fin,
// dans un dossier temporaire.

import XCTest
@testable import VibeSync

final class VLCPathTests: XCTestCase {

    /// Faux disque : ces chemins-là, et eux seuls, sont des exécutables.
    private func disk(_ paths: String...) -> VLCLauncher.ExistsFn {
        let set = Set(paths)
        return { set.contains($0) }
    }

    private let appBinary = "/Applications/VLC.app/Contents/MacOS/VLC"

    // MARK: Résolution d'un bundle (fonction pure)

    func testResolveBundle() {
        XCTAssertEqual(VLCLauncher.resolveBundle("/Applications/VLC.app"), appBinary)
        XCTAssertEqual(VLCLauncher.resolveBundle("/Users/thib/Applications/VLC.app"),
                       "/Users/thib/Applications/VLC.app/Contents/MacOS/VLC")
        // Casse de l'extension indifférente (le Finder tolère « .APP »).
        XCTAssertEqual(VLCLauncher.resolveBundle("/Applications/VLC.APP"),
                       "/Applications/VLC.APP/Contents/MacOS/VLC")
        // Un bundle renommé garde son nom : c'est le chemin NOMINAL, le
        // rattrapage par le bundle lui-même est fait plus loin (avec disque).
        XCTAssertEqual(VLCLauncher.resolveBundle("/Applications/VLC 3.app"),
                       "/Applications/VLC 3.app/Contents/MacOS/VLC 3")
        // Un binaire nu est déjà ce qu'on veut exécuter.
        for nu in ["/opt/homebrew/bin/vlc", "/usr/local/bin/vlc", appBinary, "", "/", ".app"] {
            XCTAssertEqual(VLCLauncher.resolveBundle(nu), nu, "« \(nu) » modifié à tort")
        }
    }

    func testNormalize() {
        XCTAssertEqual(VLCLauncher.normalize("  /Applications/VLC.app  "), "/Applications/VLC.app")
        // Le Finder et les glisser-déposer ajoutent volontiers une barre finale.
        XCTAssertEqual(VLCLauncher.normalize("/Applications/VLC.app/"), "/Applications/VLC.app")
        XCTAssertEqual(VLCLauncher.normalize("/Applications/VLC.app///"), "/Applications/VLC.app")
        XCTAssertEqual(VLCLauncher.normalize("~/Applications/VLC.app"),
                       NSHomeDirectory() + "/Applications/VLC.app")
        XCTAssertEqual(VLCLauncher.normalize("   \n\t "), "")
        XCTAssertEqual(VLCLauncher.normalize(""), "")
        XCTAssertEqual(VLCLauncher.normalize("/"), "/", "la racine ne doit pas être rabotée à vide")
    }

    // MARK: Réglage seul

    func testSettingBinary() {
        let exists = disk(appBinary, "/opt/homebrew/bin/vlc")

        // Un bundle : traversé vers son exécutable.
        XCTAssertEqual(VLCLauncher.settingBinary("/Applications/VLC.app", exists: exists), appBinary)
        XCTAssertEqual(VLCLauncher.settingBinary(" /Applications/VLC.app/ ", exists: exists), appBinary)
        // Un binaire : rendu tel quel.
        XCTAssertEqual(VLCLauncher.settingBinary("/opt/homebrew/bin/vlc", exists: exists),
                       "/opt/homebrew/bin/vlc")
        XCTAssertEqual(VLCLauncher.settingBinary(appBinary, exists: exists), appBinary)
        // Vide (ou blanc) : pas de réglage, ce n'est pas une erreur.
        XCTAssertNil(VLCLauncher.settingBinary("", exists: exists))
        XCTAssertNil(VLCLauncher.settingBinary("   ", exists: exists))
        // Chemins qui ne mènent nulle part.
        XCTAssertNil(VLCLauncher.settingBinary("/nawak/VLC.app", exists: exists))
        XCTAssertNil(VLCLauncher.settingBinary("/usr/local/bin/vlc", exists: exists))
        // Un bundle présent mais sans exécutable dedans reste invalide.
        XCTAssertNil(VLCLauncher.settingBinary("/Applications/VLC.app",
                                               exists: disk("/Applications/VLC.app")))
    }

    // MARK: Priorité réglage > env > détection

    func testSettingWinsOverEnvironment() {
        let brew = "/opt/homebrew/bin/vlc"
        let exists = disk(appBinary, brew)
        let env = [VLCLauncher.envBinary: brew]

        // 1. le réglage prime sur la variable d'environnement…
        XCTAssertEqual(VLCLauncher.binary(setting: "/Applications/VLC.app", env: env, exists: exists),
                       appBinary)
        // 2. … réglage vide : la variable commande (c'est ce dont vit le
        //    harnais de test réel, qui isole les préférences par VIBESYNC_SUITE
        //    et laisse donc le réglage vide).
        XCTAssertEqual(VLCLauncher.binary(setting: "", env: env, exists: exists), brew)
        // 3. … ni l'un ni l'autre : les emplacements standards.
        XCTAssertEqual(VLCLauncher.binary(setting: "", env: [:], exists: exists), appBinary)
        // Rien nulle part.
        XCTAssertNil(VLCLauncher.binary(setting: "", env: [:], exists: disk()))
    }

    func testInvalidSettingFallsBackWithoutBreakingAnything() {
        let brew = "/opt/homebrew/bin/vlc"
        let exists = disk(brew)
        // Réglage bidon + variable valide : la variable sauve le lancement.
        XCTAssertEqual(VLCLauncher.binary(setting: "/nawak/VLC.app",
                                          env: [VLCLauncher.envBinary: brew],
                                          exists: exists), brew)
        // Réglage bidon seul : la détection standard reprend la main.
        XCTAssertEqual(VLCLauncher.binary(setting: "/nawak/vlc", env: [:], exists: exists), brew)
        // Et si vraiment rien : nil, donc VLCError.notFound, jamais un
        // Process.run() sur un chemin fantôme.
        XCTAssertNil(VLCLauncher.binary(setting: "/nawak/vlc", env: [:], exists: disk()))
    }

    func testEnvironmentPointingNowhereIsNotSilentlyReplaced() {
        // Une variable explicite qui pointe dans le vide est une consigne : on
        // ne va pas chercher un autre VLC derrière le dos de l'utilisateur.
        // (Comportement d'origine de locate(), conservé.)
        XCTAssertNil(VLCLauncher.locate(env: [VLCLauncher.envBinary: "/nawak/vlc"],
                                        exists: disk(appBinary)))
        // Un bundle dans la variable est traversé comme le réglage.
        XCTAssertEqual(VLCLauncher.locate(env: [VLCLauncher.envBinary: "/Applications/VLC.app"],
                                          exists: disk(appBinary)), appBinary)
    }

    func testLocateFallsBackToPath() {
        let exists = disk("/mon/dossier/vlc")
        XCTAssertEqual(VLCLauncher.locate(env: ["PATH": "/bin:/mon/dossier:/usr/bin"], exists: exists),
                       "/mon/dossier/vlc")
        XCTAssertNil(VLCLauncher.locate(env: ["PATH": "/bin:/usr/bin"], exists: exists))
    }

    // MARK: Ligne d'état des Réglages

    func testPathStatus() {
        let brew = "/opt/homebrew/bin/vlc"
        let exists = disk(appBinary, brew)

        // Champ vide : ce que donnerait la détection automatique.
        XCTAssertEqual(VLCLauncher.pathStatus(setting: "", env: [:], exists: exists),
                       .detected(appBinary))
        XCTAssertEqual(VLCLauncher.pathStatus(setting: "  ", env: [:], exists: exists),
                       .detected(appBinary))
        XCTAssertEqual(VLCLauncher.pathStatus(setting: "", env: [VLCLauncher.envBinary: brew],
                                              exists: exists), .detected(brew))
        XCTAssertEqual(VLCLauncher.pathStatus(setting: "", env: [:], exists: disk()), .undetected)

        // Champ renseigné : c'est LUI qu'on juge, pas la détection.
        XCTAssertEqual(VLCLauncher.pathStatus(setting: "/Applications/VLC.app", env: [:], exists: exists),
                       .configured(appBinary))
        XCTAssertEqual(VLCLauncher.pathStatus(setting: brew, env: [:], exists: exists),
                       .configured(brew))
        XCTAssertEqual(VLCLauncher.pathStatus(setting: "/nawak/VLC.app", env: [:], exists: exists),
                       .invalid, "un réglage invalide ne doit pas être maquillé par la détection")

        // Sévérités : c'est ce qui donne la couleur de la ligne.
        XCTAssertEqual(VLCPathStatus.detected(appBinary).severity, .info)
        XCTAssertEqual(VLCPathStatus.configured(appBinary).severity, .ok)
        XCTAssertEqual(VLCPathStatus.undetected.severity, .warn)
        XCTAssertEqual(VLCPathStatus.invalid.severity, .error)
        // Les textes disent le chemin retenu, et jamais rien de vide.
        XCTAssertTrue(VLCPathStatus.detected(appBinary).text.contains(appBinary))
        XCTAssertTrue(VLCPathStatus.configured(brew).text.contains(brew))
        for status: VLCPathStatus in [.detected(appBinary), .undetected, .configured(brew), .invalid] {
            XCTAssertFalse(status.text.isEmpty)
        }
    }

    // MARK: Réglage persistant

    func testPreferencesRoundTrip() {
        let store = MemoryStore()
        // Rien d'enregistré : détection automatique.
        XCTAssertEqual(Preferences.vlcPath(store), "")

        Preferences.setVLCPath("  /Applications/VLC.app  ", store)
        XCTAssertEqual(Preferences.vlcPath(store), "/Applications/VLC.app", "chemin non rogné")

        // Vider le champ est une valeur légitime, pas un effacement de clé.
        Preferences.setVLCPath("   ", store)
        XCTAssertEqual(Preferences.vlcPath(store), "")
        XCTAssertEqual(store.string(forKey: Preferences.keyVLC), "")
    }

    // MARK: Bit d'exécution (vrai disque)

    func testIsExecutableFileNeedsTheExecutableBit() throws {
        let fm = FileManager.default
        let root = fm.temporaryDirectory.appendingPathComponent("vibesync-vlc-\(UUID().uuidString)")
        try fm.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: root) }

        let plain = root.appendingPathComponent("vlc")
        try Data("#!/bin/sh\n".utf8).write(to: plain)
        XCTAssertFalse(VLCLauncher.isExecutableFile(plain.path),
                       "un fichier sans bit d'exécution n'est pas lançable")
        XCTAssertNil(VLCLauncher.settingBinary(plain.path))

        try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: plain.path)
        XCTAssertTrue(VLCLauncher.isExecutableFile(plain.path))
        XCTAssertEqual(VLCLauncher.settingBinary(plain.path), plain.path)

        // Un dossier est « exécutable » au sens POSIX (traversable) : il ne
        // doit surtout pas passer pour un lecteur.
        XCTAssertFalse(VLCLauncher.isExecutableFile(root.path))
        XCTAssertNil(VLCLauncher.settingBinary(root.path))
        XCTAssertFalse(VLCLauncher.isExecutableFile(root.path + "/rien-du-tout"))
    }
}
