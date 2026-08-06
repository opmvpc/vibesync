// VLCStatusTests.swift — parsing et assainissement de /requests/status.json.
// Mêmes cas que internal/vlc/http_test.go et que le harnais C §vlc.

import XCTest
@testable import VibeSync

final class VLCStatusTests: XCTestCase {

    /// Extrait raccourci d'un vrai status.json de VLC 3.
    private let realPayload = """
    {"fullscreen":false,"stats":{"inputbitrate":0.1,"demuxreadbytes":123},\
    "aspectratio":"default","audiodelay":0,"apiversion":3,"currentplid":4,\
    "time":2,"volume":256,"length":1200,"random":false,"audiofilters":{"filter_0":""},\
    "rate":1,"videoeffects":{"hue":0,"saturation":1},"state":"playing",\
    "loop":false,"version":"3.0.20 Vetinari","position":0.00208333333333333,\
    "information":{"chapter":0,"chapters":[],"title":0,"category":{"meta":\
    {"filename":"ep1.mkv","title":"Episode 1"}},"titles":[]},"repeat":false}
    """

    func testRealPayload() {
        guard let st = VLCStatusParser.parse(realPayload) else {
            XCTFail("status.json réaliste refusé")
            return
        }
        XCTAssertEqual(st.state, PlayState.playing)
        XCTAssertEqual(st.positionSec, 2.5, accuracy: 1e-6, "position fine = position × length")
        XCTAssertEqual(st.lengthSec, 1200)
        XCTAssertEqual(st.rate, 1)
        XCTAssertEqual(st.fileName, "ep1.mkv")
        XCTAssertTrue(st.loaded)
    }

    func testSanitization() {
        let cases: [(json: String, pos: Double, length: Double, rate: Double)] = [
            ("{\"state\":\"playing\",\"position\":2.5,\"length\":100,\"rate\":1}", 100, 100, 1),
            ("{\"state\":\"playing\",\"position\":-3,\"length\":100,\"rate\":1}", 0, 100, 1),
            ("{\"state\":\"playing\",\"position\":0.5,\"length\":-10,\"time\":7,\"rate\":1}", 7, 0, 1),
            ("{\"state\":\"playing\",\"position\":0.5,\"length\":100,\"rate\":0}", 50, 100, 1),
            ("{\"state\":\"paused\",\"position\":0.1,\"length\":100,\"rate\":-2}", 10, 100, 1),
            ("{\"state\":\"stopped\"}", 0, 0, 1),
        ]
        for (index, item) in cases.enumerated() {
            guard let st = VLCStatusParser.parse(item.json) else {
                XCTFail("cas \(index) refusé")
                continue
            }
            XCTAssertEqual(st.positionSec, item.pos, accuracy: 1e-9, "cas \(index) position")
            XCTAssertEqual(st.lengthSec, item.length, accuracy: 1e-9, "cas \(index) durée")
            XCTAssertEqual(st.rate, item.rate, accuracy: 1e-9, "cas \(index) rate")
        }
    }

    func testHostileInput() {
        XCTAssertNil(VLCStatusParser.parse("pas du json"))
        XCTAssertNil(VLCStatusParser.parse("[1,2]"))
        XCTAssertNil(VLCStatusParser.parse(""))

        // Types inattendus : valeurs de repli, pas d'échec.
        guard let st = VLCStatusParser.parse("{\"state\":42,\"position\":\"x\"}") else {
            XCTFail("types inattendus non tolérés")
            return
        }
        XCTAssertEqual(st.state, PlayState.stopped)
        XCTAssertEqual(st.positionSec, 0)
        XCTAssertFalse(st.loaded)
    }

    func testMetadataFallback() {
        let json = "{\"state\":\"paused\",\"length\":10,\"position\":0.5," +
                   "\"information\":{\"category\":{\"meta\":{\"filename\":\"  \",\"title\":\"T\"}}}}"
        guard let st = VLCStatusParser.parse(json) else {
            XCTFail("métadonnées")
            return
        }
        XCTAssertEqual(st.fileName, "T", "repli sur title quand filename est vide")
        XCTAssertEqual(st.positionSec, 5)
    }

    func testTimeArithmeticMatchesGo() {
        XCTAssertEqual(VSTime.seconds(200 * 1_000_000), 0.2)
        XCTAssertEqual(VSTime.seconds(20200 * 1_000_000), 20.2)
        XCTAssertEqual(VSTime.toUnixMs(1785960000000 * 1_000_000), 1785960000000)
        XCTAssertEqual(VSTime.toUnixMs(-1_500_000), -2, "division plancher")
    }
}
