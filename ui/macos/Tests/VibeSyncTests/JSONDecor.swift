// JSONDecor.swift — JSON du DÉCOR DE TEST, plus du produit (VS-033).
//
// Ce fichier était `Sources/VibeSync/Net/JSON.swift` : un écrivain JSON écrit
// main et des accesseurs tolérants au-dessus de JSONSerialization. Depuis la
// phase 4 d'ADR-010, plus une ligne de l'application n'en a besoin — le
// protocole, le statut VLC et l'état publié par le pilote passent tous par
// json.c, l'écrivain et le parseur du C commun. Il ne reste que deux usages,
// tous deux dans les tests, et tous deux du DÉCOR (ce qui est jugé, lui, passe
// par le C) :
//
//   - lire les vecteurs de conformité test/vectors/*.json et les cas de test,
//     qu'il faut parcourir librement (VectorsTests, VSCoreVectorsTests) ;
//   - fabriquer le status.json du faux VLC (FakeVLC).
//
// Il a donc migré dans la cible de tests plutôt que d'être supprimé : ce qui
// compte est qu'il ne soit plus livré.

import Foundation

/// Valeur JSON à écrire. L'ordre des clés est celui de la construction.
public indirect enum JSONVal {
    case str(String)
    case num(Double)
    case int(Int64)
    case bool(Bool)
    case obj([(String, JSONVal)])
    case arr([JSONVal])
    case null

    public var encoded: String {
        switch self {
        case .str(let s):
            return JSONVal.quote(s)
        case .num(let v):
            return JSONVal.number(v)
        case .int(let v):
            return String(v)
        case .bool(let b):
            return b ? "true" : "false"
        case .obj(let pairs):
            var parts: [String] = []
            parts.reserveCapacity(pairs.count)
            for (k, v) in pairs {
                parts.append(JSONVal.quote(k) + ":" + v.encoded)
            }
            return "{" + parts.joined(separator: ",") + "}"
        case .arr(let items):
            return "[" + items.map({ $0.encoded }).joined(separator: ",") + "]"
        case .null:
            return "null"
        }
    }

    /// Représentation JSON d'un flottant : la description Swift est la plus
    /// courte qui relit exactement la même valeur. Les valeurs non finies sont
    /// interdites en JSON : elles deviennent 0 (elles sont déjà rejetées en
    /// amont par l'assainissement).
    static func number(_ v: Double) -> String {
        if !v.isFinite {
            return "0"
        }
        if v == v.rounded() && abs(v) < 1e15 {
            // 1200.0 → "1200" plutôt que "1200.0" : même valeur, plus proche
            // de ce qu'écrivent les autres implémentations.
            return String(Int64(v))
        }
        return "\(v)"
    }

    static func quote(_ s: String) -> String {
        var out = "\""
        for scalar in s.unicodeScalars {
            switch scalar {
            case "\"":
                out += "\\\""
            case "\\":
                out += "\\\\"
            case "\n":
                out += "\\n"
            case "\r":
                out += "\\r"
            case "\t":
                out += "\\t"
            default:
                if scalar.value < 0x20 {
                    out += String(format: "\\u%04x", scalar.value)
                } else {
                    out.unicodeScalars.append(scalar)
                }
            }
        }
        return out + "\""
    }
}

/// Accesseurs tolérants sur le résultat de JSONSerialization : une clé absente
/// ou d'un type inattendu rend la valeur par défaut, jamais une exception.
public enum JSON {

    public static func parse(_ text: String) -> Any? {
        guard let data = text.data(using: .utf8) else {
            return nil
        }
        return try? JSONSerialization.jsonObject(with: data, options: [])
    }

    public static func parse(_ data: Data) -> Any? {
        return try? JSONSerialization.jsonObject(with: data, options: [])
    }

    public static func object(_ value: Any?) -> [String: Any]? {
        return value as? [String: Any]
    }

    public static func child(_ value: Any?, _ key: String) -> [String: Any]? {
        guard let o = value as? [String: Any] else {
            return nil
        }
        return o[key] as? [String: Any]
    }

    public static func string(_ value: Any?, _ key: String, _ fallback: String = "") -> String {
        guard let o = value as? [String: Any], let s = o[key] as? String else {
            return fallback
        }
        return s
    }

    public static func number(_ value: Any?, _ key: String, _ fallback: Double = 0) -> Double {
        guard let o = value as? [String: Any], let n = o[key] as? NSNumber else {
            return fallback
        }
        let d = n.doubleValue
        return d.isFinite ? d : fallback
    }

    public static func int(_ value: Any?, _ key: String, _ fallback: Int64 = 0) -> Int64 {
        guard let o = value as? [String: Any], let n = o[key] as? NSNumber else {
            return fallback
        }
        return n.int64Value
    }

    public static func bool(_ value: Any?, _ key: String, _ fallback: Bool = false) -> Bool {
        guard let o = value as? [String: Any], let n = o[key] as? NSNumber else {
            return fallback
        }
        return n.boolValue
    }

    public static func array(_ value: Any?, _ key: String) -> [Any] {
        guard let o = value as? [String: Any], let a = o[key] as? [Any] else {
            return []
        }
        return a
    }
}
