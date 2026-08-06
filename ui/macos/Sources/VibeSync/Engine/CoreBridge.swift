// CoreBridge.swift — les primitives d'interop Swift ↔ C, et rien d'autre
// (ADR-010, phase 4).
//
// Depuis VS-033 la frontière avec la couche commune n'est plus seulement le
// moteur : le protocole, le statut VLC, la recherche de médias et la politique
// de connexion passent eux aussi par `VSCore`. Les trois gestes qu'ils ont tous
// en commun sont ici, écrits une fois :
//
//   1. `withStr8` — voir une chaîne Swift comme un `Str8` LE TEMPS D'UN APPEL.
//      Aucun pointeur C ne survit à la portée du corps.
//   2. `coreString` — recopier un `Str8`/`StrBuf` rendu par le C en `String`.
//      La copie est délibérée : ce que garde l'interface ne pointe jamais dans
//      une arène qui va être rendue.
//   3. `Scratch` — l'arène de travail. Le C n'alloue que là ; chaque appel
//      encadre sa portée (`temp_begin`/`temp_end`) et rend sa place tout de
//      suite, exactement comme `main.c` sous Windows.
//
// Rappel de discipline : le C ne conserve JAMAIS une adresse qui nous
// appartient (`strbuf_set` copie), et nous ne conservons jamais une adresse qui
// lui appartient (tout est recopié avant la fin de la portée temporaire).

import Foundation
import VSCore

// MARK: - Chaînes

/// Voit une chaîne Swift comme un `Str8` le temps d'un appel. Le tampon est
/// vivant pour toute la durée du corps ; une chaîne vide reçoit quand même une
/// adresse valide (le C ne déréférence jamais un `Str8` de longueur 0, mais un
/// tampon Swift vide n'a pas d'adresse stable).
@inline(__always)
func withStr8<R>(_ s: String, _ body: (Str8) -> R) -> R {
    return withStr8(bytes: Array(s.utf8), body)
}

/// Même chose pour des octets déjà en main (corps HTTP de VLC, par exemple) :
/// on évite le détour par `String`, qui validerait l'UTF-8 deux fois.
@inline(__always)
func withStr8<R>(bytes: [UInt8], _ body: (Str8) -> R) -> R {
    var buffer = bytes
    let length = buffer.count
    if buffer.isEmpty {
        buffer = [0]
    }
    return buffer.withUnsafeMutableBufferPointer { raw in
        body(Str8(data: raw.baseAddress, len: length))
    }
}

/// Recopie un `Str8` rendu par le C (il pointe dans l'arène : il ne survivra
/// pas à la fin de la portée temporaire).
func coreString(_ s: Str8) -> String {
    guard let data = s.data, s.len > 0 else {
        return ""
    }
    return String(decoding: UnsafeBufferPointer(start: data, count: Int(s.len)), as: UTF8.self)
}

/// Recopie un `StrBuf` (tampon borné inclus par valeur dans les structures
/// d'état du C).
func coreString(_ buf: StrBuf) -> String {
    var copy = buf
    let capacity = MemoryLayout.size(ofValue: copy.data)
    let length = max(0, min(Int(copy.len), capacity))
    return withUnsafeBytes(of: &copy.data) { raw in
        String(decoding: raw.prefix(length), as: UTF8.self)
    }
}

/// Représentation décimale d'un flottant par le C (`f64_to_str` : la plus
/// courte qui relit exactement la même valeur). Une valeur non finie donne
/// « 0 », comme à l'encodage JSON.
func coreNumberText(_ v: Double) -> String {
    var buffer = [CChar](repeating: 0, count: 64)
    let n = buffer.withUnsafeMutableBufferPointer { raw in
        f64_to_str(v, raw.baseAddress, Int(raw.count))
    }
    if n <= 0 {
        return "0"
    }
    return String(cString: buffer)
}

// MARK: - Arène

/// L'arène de travail du C, protégée par un verrou.
///
/// Le C ne fait pas un seul `malloc` : tout ce qu'il produit (JSON analysé,
/// message encodé, chemin trouvé) vit dans une arène que l'appelant fournit et
/// reprend aussitôt. `use` encadre chaque appel d'une portée temporaire : la
/// place est rendue à la sortie, quelle que soit l'issue.
///
/// Le verrou est là parce que tous les appelants ne sont pas sur la file
/// principale — le statut de VLC est analysé sur la file d'`URLSession`. Il est
/// RÉCURSIF pour qu'un appel imbriqué (un encodage dans le corps d'un décodage)
/// ne se bloque pas lui-même : les portées temporaires, elles, s'imbriquent
/// naturellement.
///
/// Une recherche longue (les médias) ne doit pas prendre CE verrou : elle se
/// crée la sienne, cf. `MediaLibrary`.
final class Scratch {

    /// Arène partagée des appels courts : protocole, statut VLC, adresses.
    /// 8 Mo réservés, comme le `scratch` du client Windows (main.c) — de
    /// l'espace d'adressage, engagé page par page à l'usage.
    static let shared = Scratch(reserveBytes: 8 * 1024 * 1024)

    private let arena: OpaquePointer
    private let lock = NSRecursiveLock()

    init(reserveBytes: Int) {
        guard let created = arena_create(reserveBytes) else {
            // Une réservation d'espace d'adressage qui échoue au démarrage est
            // un problème de machine, pas un cas à rattraper.
            fatalError("vibesync : réservation d'arène impossible (\(reserveBytes) octets)")
        }
        arena = created
    }

    deinit {
        arena_destroy(arena)
    }

    /// Exécute `body` avec l'arène, et rend toute la place prise.
    func use<R>(_ body: (OpaquePointer) -> R) -> R {
        lock.lock()
        defer { lock.unlock() }
        let mark = temp_begin(arena)
        defer { temp_end(mark) }
        return body(arena)
    }
}
