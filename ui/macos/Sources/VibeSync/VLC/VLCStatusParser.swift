// VLCStatusParser.swift — lecture de /requests/status.json.
//
// Même assainissement que la référence Go (internal/vlc/http.go statusFrom) et
// que le port C (vlc.c) : position fine = `position` × `length`, fraction
// bornée à [0,1], `rate` ≤ 0 ramené à 1, durée ≤ 0 ignorée, repli sur `time`.

import Foundation

public enum VLCStatusParser {

    /// `nil` si le corps n'est pas un objet JSON.
    public static func parse(_ body: String) -> VLCStatus? {
        guard let root = JSON.object(JSON.parse(body)) else {
            return nil
        }
        return parse(object: root)
    }

    public static func parse(object root: [String: Any]) -> VLCStatus {
        var st = VLCStatus()

        let length = JSON.number(root, "length", 0)
        if length.isFinite && length > 0 {
            st.lengthSec = length
        }

        let rate = JSON.number(root, "rate", 0)
        st.rate = (rate.isFinite && rate > 0) ? rate : 1

        switch JSON.string(root, "state").lowercased() {
        case "playing":
            st.state = .playing
        case "paused":
            st.state = .paused
        default:
            st.state = .stopped
        }

        if st.lengthSec > 0 {
            // `time` n'a qu'une résolution d'une seconde : on préfère la
            // fraction, bornée à [0,1] avant multiplication.
            st.positionSec = clamp01(JSON.number(root, "position", 0)) * st.lengthSec
        } else {
            let time = JSON.number(root, "time", 0)
            if time.isFinite && time > 0 {
                st.positionSec = time
            }
        }

        st.fileName = metaFileName(root)
        return st
    }

    private static func clamp01(_ v: Double) -> Double {
        if !v.isFinite || v < 0 {
            return 0
        }
        return v > 1 ? 1 : v
    }

    /// information.category.meta : filename, puis title, puis now_playing.
    private static func metaFileName(_ root: [String: Any]) -> String {
        guard let information = JSON.child(root, "information"),
              let category = JSON.child(information, "category"),
              let meta = JSON.child(category, "meta") else {
            return ""
        }
        for key in ["filename", "title", "now_playing"] {
            let value = JSON.string(meta, key)
            let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
            if !trimmed.isEmpty {
                return value
            }
        }
        return ""
    }
}
