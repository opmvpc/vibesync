// VLCStatusParser.swift — frontière Swift ↔ lecture de /requests/status.json
// (ADR-010, phase 4).
//
// L'assainissement (position fine = `position` × `length`, fraction bornée à
// [0,1], `rate` ≤ 0 ramené à 1, durée ≤ 0 ignorée, repli sur `time`, ordre de
// préférence des métadonnées) n'existe plus qu'une fois : `vlc_parse_status`
// dans core/src/vlc_core.c, au-dessus du parseur core/src/json.c. Il ne reste
// ici que la conversion du VsStatus du C vers le VLCStatus de l'interface.

import Foundation
import VSCore

public enum VLCStatusParser {

    /// `nil` si le corps n'est pas un objet JSON.
    public static func parse(_ body: String) -> VLCStatus? {
        return withStr8(body) { parse(str8: $0) }
    }

    /// Même chose sur le corps HTTP brut : le détour par `String` n'apporte
    /// rien, le C lit de l'UTF-8.
    public static func parse(_ body: Data) -> VLCStatus? {
        return withStr8(bytes: [UInt8](body)) { parse(str8: $0) }
    }

    private static func parse(str8 body: Str8) -> VLCStatus? {
        return Scratch.shared.use { arena -> VLCStatus? in
            var out = VsStatus()
            if vlc_parse_status(arena, body, &out) == 0 {
                return nil
            }
            var st = VLCStatus()
            switch out.state {
            case VS_PLAY_PLAYING: st.state = .playing
            case VS_PLAY_PAUSED: st.state = .paused
            default: st.state = .stopped
            }
            st.positionSec = out.position_sec
            st.lengthSec = out.length_sec
            st.rate = out.rate
            st.fileName = coreString(out.file_name)
            return st
        }
    }
}
