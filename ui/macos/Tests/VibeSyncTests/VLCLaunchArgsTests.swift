// VLCLaunchArgsTests.swift — gel de la ligne de lancement de VLC (VS-029).
//
// Troisième exemplaire du même gel, après internal/vlc/launch_test.go
// (TestLaunchArgsGeleDarwin) et le bloc `vlc_build_command` de
// core/tests/test_core.c. Chaque drapeau neutralise un réglage que le vlcrc de
// l'utilisateur pourrait imposer ; les perdre, c'est reproduire le retour
// terrain « VLC s'ouvre et joue, l'app dit aucun fichier ouvert ».

import XCTest

@testable import VibeSync

final class VLCLaunchArgsTests: XCTestCase {

    /// Les 12 drapeaux de la liste `darwin`, dans l'ordre, égalité stricte.
    /// C'est la liste Windows des 15 MOINS la famille « instance unique », que
    /// le VLC macOS ne connaît pas (refus de démarrage, vérifié un par un sur
    /// VLC 3.0.23).
    func testLaunchArgsGele() {
        let want = [
            "--extraintf=http",
            "--lua-intf=http",
            "--http-host=127.0.0.1",
            "--http-port=41234",
            "--http-password=deadbeef",
            "--playlist-autostart",
            "--start-paused",
            "--no-random",
            "--no-loop",
            "--no-repeat",
            "--no-play-and-exit",
            "--no-video-title-show",
        ]
        let got = VLCLauncher.launchArgs(port: 41234, password: "deadbeef")
        XCTAssertEqual(got, want, "la ligne de lancement a bougé")
    }

    /// Les formes positives sont des pièges : `--one-instance` seul est
    /// réactivé par VLC quand le média vient d'un fichier, `--playlist-enqueue`
    /// enfile le média au lieu de l'ouvrir. Aucune ne doit jamais apparaître —
    /// et sur macOS, aucune forme négative non plus (VLC refuserait de
    /// démarrer).
    func testLaunchArgsPasDeFormeInstanceUnique() {
        let interdits = [
            "--one-instance",
            "--no-one-instance",
            "--one-instance-when-started-from-file",
            "--no-one-instance-when-started-from-file",
            "--playlist-enqueue",
            "--no-playlist-enqueue",
        ]
        let got = VLCLauncher.launchArgs(port: 41234, password: "deadbeef")
        for interdit in interdits {
            XCTAssertFalse(got.contains(interdit), "drapeau interdit présent : \(interdit)")
        }
    }

    /// Port et mot de passe sont les seules parties variables, et l'interface
    /// HTTP n'écoute jamais ailleurs que sur la loopback.
    func testLaunchArgsPortEtMotDePasse() {
        let got = VLCLauncher.launchArgs(port: 5000, password: "s3cr3t")
        XCTAssertTrue(got.contains("--http-port=5000"), "port absent : \(got)")
        XCTAssertTrue(got.contains("--http-password=s3cr3t"), "mot de passe absent : \(got)")
        for arg in got where arg.hasPrefix("--http-host=") {
            XCTAssertEqual(arg, "--http-host=127.0.0.1", "interface HTTP exposée hors loopback")
        }
    }

    /// Le média vient APRÈS toutes les options : VLC prendrait le reste de la
    /// ligne pour des MRL. `launchArgs` ne le contient donc pas — c'est
    /// `launch` qui l'ajoute en dernier.
    func testLaunchArgsNeContientPasLeMedia() {
        for arg in VLCLauncher.launchArgs(port: 41234, password: "deadbeef") {
            XCTAssertTrue(arg.hasPrefix("--"), "argument non-option dans la liste : \(arg)")
        }
    }
}
