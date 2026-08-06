// JSON.swift — encodage écrit main, décodage via JSONSerialization.
//
// Les enveloppes du protocole sont hétérogènes ({type, data} où `data` change
// de forme selon `type`) : Codable imposerait un type par message et des
// conteneurs manuels de toute façon. On fait comme le port C — un écrivain
// minimal côté sortie, des accesseurs type-sûrs côté entrée — ce qui donne un
// contrôle exact sur le format des nombres (représentation la plus courte qui
// relit la même valeur, indispensable pour `positionSec`).

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
