// ParticipantFileTests.swift — VS-040 : quelles lignes de la liste des
// participants mènent quelque part quand on les double-clique.
//
// La règle est une fonction pure (AppModel.participantFileToOpen) parce que
// c'est le seul moyen de la tester : le reste du chemin — recherche dans les
// dossiers, lancement de VLC — est déjà couvert par PreferencesTests et par
// les séances réelles. Ce fichier ne teste QUE la décision, y compris les
// trois cas où le double-clic ne doit rien faire.

import XCTest
@testable import VibeSync

final class ParticipantFileTests: XCTestCase {

    private func user(id: String, file: String, hasFile: Bool = true) -> ServerUser {
        var u = ServerUser()
        u.id = id
        u.name = "u-" + id
        u.hasFile = hasFile
        u.fileName = file
        return u
    }

    /// Le cas nominal : quelqu'un d'autre, un fichier déclaré, différent du
    /// nôtre — c'est ce nom-là qu'on ira chercher.
    func testAutreFichierEstOuvrable() {
        let u = user(id: "b", file: "S01E02.mkv")
        XCTAssertEqual(AppModel.participantFileToOpen(user: u, selfId: "a", myFile: "S01E01.mkv"),
                       "S01E02.mkv")
    }

    /// Sans fichier ouvert de notre côté, tout nom déclaré diffère du nôtre.
    func testSansFichierChezNous() {
        let u = user(id: "b", file: "S01E02.mkv")
        XCTAssertEqual(AppModel.participantFileToOpen(user: u, selfId: "a", myFile: ""),
                       "S01E02.mkv")
    }

    /// Cas limite 1 : double-clic sur soi-même = rien.
    func testSoiMemeIgnore() {
        let u = user(id: "a", file: "S01E02.mkv")
        XCTAssertNil(AppModel.participantFileToOpen(user: u, selfId: "a", myFile: "S01E01.mkv"))
    }

    /// Cas limite 2 : participant sans fichier déclaré = rien. Les deux formes
    /// comptent : le drapeau baissé, et le nom vide malgré le drapeau.
    func testParticipantSansFichierIgnore() {
        let sansDrapeau = user(id: "b", file: "S01E02.mkv", hasFile: false)
        XCTAssertNil(AppModel.participantFileToOpen(user: sansDrapeau, selfId: "a", myFile: ""))
        let nomVide = user(id: "b", file: "")
        XCTAssertNil(AppModel.participantFileToOpen(user: nomVide, selfId: "a", myFile: ""))
    }

    /// Cas limite 3 : il regarde déjà notre fichier = rien. La comparaison est
    /// insensible à la casse, comme celle du bandeau et comme la recherche
    /// dans les dossiers (media_core.c).
    func testMemeFichierQueNousIgnore() {
        let u = user(id: "b", file: "S01E01.mkv")
        XCTAssertNil(AppModel.participantFileToOpen(user: u, selfId: "a", myFile: "S01E01.mkv"))
        let casse = user(id: "b", file: "s01e01.MKV")
        XCTAssertNil(AppModel.participantFileToOpen(user: casse, selfId: "a", myFile: "S01E01.mkv"))
    }

    /// Un identifiant vide n'est pas un participant : rien à ouvrir. Sans
    /// cette garde, une ligne mal formée deviendrait cliquable dès que notre
    /// propre identifiant est vide (avant le welcome).
    func testIdentifiantVideIgnore() {
        let u = user(id: "", file: "S01E02.mkv")
        XCTAssertNil(AppModel.participantFileToOpen(user: u, selfId: "", myFile: ""))
    }
}
