// FakeVLC.swift — faux lecteur pour le rejeu des vecteurs.
//
// Port de la FakeVlc du harnais C (ui/win32/src/test_main.c), elle-même copie
// de internal/vlc/vlctest.Fake : position qui avance selon l'horloge simulée,
// seek arrondi à la seconde par l'interface HTTP, `position` = pos/length et
// `length` arrondie — c'est ce qui explique les résidus de virgule flottante
// visibles dans les vecteurs.

import Foundation
@testable import VibeSync

final class FakeVLC {
    var state: String = "stopped"
    var pos: Double = 0
    var length: Double = 0
    var rate: Double = 1
    var lastAt: Nanos = 0
    var file: String = ""

    init(now: Nanos) {
        lastAt = now
    }

    private func clamp(_ v: Double, _ lo: Double, _ hi: Double) -> Double {
        if hi > 0 && v > hi {
            return hi
        }
        return v < lo ? lo : v
    }

    func advance(_ now: Nanos) {
        let elapsed = VSTime.seconds(now - lastAt)
        lastAt = now
        if elapsed <= 0 || state != "playing" {
            return
        }
        pos = clamp(pos + elapsed * rate, 0, length)
    }

    func load(name: String, length newLength: Double, now: Nanos) {
        advance(now)
        file = name
        length = newLength
        pos = 0
        state = "paused"
        rate = 1
    }

    func play(_ now: Nanos) {
        advance(now)
        if state != "stopped" {
            state = "playing"
        }
    }

    func pause(_ now: Nanos) {
        advance(now)
        if state != "stopped" {
            state = "paused"
        }
    }

    func seek(_ seconds: Double, _ now: Nanos) {
        advance(now)
        pos = clamp(seconds, 0, length)
    }

    /// Applique une décision du moteur, comme le ferait le driver HTTP.
    func apply(_ cmd: VLCCommand, _ now: Nanos) {
        advance(now)
        switch cmd.kind {
        case .pause:
            if state == "playing" {
                state = "paused"
            }
        case .resume:
            if state == "paused" {
                state = "playing"
            }
        case .seek:
            var v = cmd.value
            if !v.isFinite || v < 0 {
                v = 0
            }
            pos = clamp(v.rounded(), 0, length)
        case .rate:
            if cmd.value.isFinite && cmd.value > 0 {
                rate = cmd.value
            }
        }
    }

    private func floorLike(_ v: Double) -> Double {
        var r = v.rounded()
        if r > v {
            r -= 1
        }
        return r
    }

    /// Réponse que renverrait /requests/status.json.
    func statusJSON(_ now: Nanos) -> String {
        advance(now)
        let roundedLength = length.rounded()
        var ratio: Double = 0
        if roundedLength > 0 {
            ratio = pos / roundedLength
            if ratio < 0 {
                ratio = 0
            }
            if ratio > 1 {
                ratio = 1
            }
        }
        let root = JSONVal.obj([
            ("state", .str(state)),
            ("length", .num(roundedLength)),
            ("time", .num(floorLike(pos))),
            ("rate", .num(rate)),
            ("position", .num(ratio)),
            ("volume", .int(256)),
            ("information", .obj([
                ("category", .obj([
                    ("meta", .obj([
                        ("filename", .str(file)),
                    ])),
                ])),
            ])),
        ])
        return root.encoded
    }
}
