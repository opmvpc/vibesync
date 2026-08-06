// MediaLibrary.swift — frontière Swift ↔ recherche de médias commune
// (VS-026 pour la fonction, ADR-010 phase 4 pour la bascule).
//
// L'algorithme borné — descente récursive limitée en profondeur ET en nombre
// d'entrées, liens jamais suivis, homonyme le plus gros retenu — n'existe plus
// qu'une fois : `media_find_with` (core/src/media_core.c), au-dessus de la
// primitive de parcours de la plateforme (`vs_dir_ops`, core/posix/
// media_posix.c : opendir/readdir, lstat, comparaison de noms NFC insensible à
// la casse). Il ne reste ici que le passage sur un thread de travail et la
// conversion du résultat.
//
// Deux choses que le C apporte et que le parcours Foundation retiré n'avait pas
// (les deux valaient déjà pour le client Windows) :
//   - les dossiers CACHÉS sont explorés (`FileManager` les sautait) : un média
//     rangé sous un dossier commençant par un point se retrouve maintenant ;
//   - le nombre d'entrées visitées est celui de `readdir` brut, « . » et « .. »
//     compris, donc comparable à celui de `FindFirstFileW` sous Windows.

import Foundation
import VSCore

public enum MediaLibrary {

    /// Profondeur maximale explorée sous un dossier configuré.
    public static let maxDepth = Int(MEDIA_MAX_DEPTH)
    /// Nombre maximal d'entrées visitées, toutes racines confondues.
    public static let maxEntries = Int(MEDIA_MAX_ENTRIES)
    /// Nombre de dossiers racines pris en compte (borne du C commun).
    public static let maxDirs = Int(MEDIA_MAX_DIRS)

    public struct Result {
        public var found: Bool = false
        public var path: String = ""
        public var sizeBytes: Int64 = 0
        /// Nombre de fichiers portant ce nom (homonymes).
        public var matches: Int = 0
        public var visited: Int = 0
        /// Vrai si une borne a coupé la recherche : « introuvable » devient
        /// alors « pas trouvé dans ce qu'on a eu le temps de regarder ».
        public var truncated: Bool = false

        public init() {}
    }

    /// Recherche synchrone. Appelée hors thread principal (cf. `find(name:in:completion:)`).
    public static func find(name: String, in dirs: [String]) -> Result {
        var out = Result()
        let needle = name.trimmingCharacters(in: .whitespacesAndNewlines)
        if needle.isEmpty || dirs.isEmpty {
            return out
        }

        var roots = [StrBuf](repeating: StrBuf(), count: maxDirs)
        var count = 0
        for dir in dirs where count < maxDirs {
            if dir.isEmpty {
                continue
            }
            var buf = StrBuf()
            withStr8(dir) { strbuf_set(&buf, $0) }
            roots[count] = buf
            count += 1
        }
        if count == 0 {
            return out
        }

        // Arène DÉDIÉE : la recherche peut durer des secondes sur un volume
        // réseau, elle ne doit pas retenir l'arène partagée des appels courts
        // (protocole, statut VLC) que la file principale utilise pendant ce
        // temps. 4 Mo, comme le thread de recherche du client Windows.
        let scratch = Scratch(reserveBytes: 4 * 1024 * 1024)
        scratch.use { arena in
            var find = MediaFind()
            withStr8(needle) { target in
                _ = media_find_with(arena, vs_dir_ops(), &roots, count, target, &find)
            }
            out.found = find.found != 0
            out.path = coreString(find.path)
            out.sizeBytes = find.size_bytes
            out.matches = Int(find.matches)
            out.visited = Int(find.visited)
            out.truncated = find.truncated != 0
        }
        return out
    }

    /// Recherche hors thread principal ; `completion` est rappelée dessus.
    public static func find(name: String,
                            in dirs: [String],
                            completion: @escaping (Result) -> Void) {
        DispatchQueue.global(qos: .userInitiated).async {
            let result = find(name: name, in: dirs)
            DispatchQueue.main.async {
                completion(result)
            }
        }
    }
}
