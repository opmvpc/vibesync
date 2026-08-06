// MediaLibrary.swift — retrouver chez soi le fichier qu'un participant regarde
// (VS-026). Port de core/src/media_core.c.
//
// Aucun changement de protocole : les noms de fichiers circulent déjà par
// `setFile`/`users`. On cherche le nom EXACT (comparaison insensible à la
// casse) dans les dossiers configurés, récursivement, avec des bornes dures :
// une arborescence pathologique ne doit ni bloquer l'interface ni tourner
// indéfiniment. À nom égal, le plus gros fichier gagne (heuristique : c'est la
// version, pas l'extrait) — et on la trace pour pouvoir la contester.

import Foundation

public enum MediaLibrary {

    /// Profondeur maximale explorée sous un dossier configuré.
    public static let maxDepth = 6
    /// Nombre maximal d'entrées visitées, toutes racines confondues.
    public static let maxEntries = 50_000

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
        let fm = FileManager.default
        let keys: [URLResourceKey] = [.isDirectoryKey, .isSymbolicLinkKey, .fileSizeKey, .nameKey]

        for dir in dirs {
            if out.truncated {
                break
            }
            let root = URL(fileURLWithPath: dir, isDirectory: true)
            guard let walker = fm.enumerator(at: root,
                                             includingPropertiesForKeys: keys,
                                             options: [.skipsHiddenFiles],
                                             errorHandler: { _, _ in true }) else {
                continue  // dossier illisible : on passe, ce n'est pas une erreur
            }
            while let url = walker.nextObject() as? URL {
                if out.visited >= maxEntries {
                    out.truncated = true
                    break
                }
                out.visited += 1
                let values = try? url.resourceValues(forKeys: Set(keys))
                // Liens symboliques ignorés : sans cela une boucle de
                // répertoires ferait tourner la recherche jusqu'à la borne.
                if values?.isSymbolicLink == true {
                    walker.skipDescendants()
                    continue
                }
                if values?.isDirectory == true {
                    // `level` vaut 1 pour le contenu direct de la racine.
                    if walker.level >= maxDepth {
                        walker.skipDescendants()
                    }
                    continue
                }
                let entry = values?.name ?? url.lastPathComponent
                if entry.compare(needle, options: .caseInsensitive) != .orderedSame {
                    continue
                }
                out.matches += 1
                let size = Int64(values?.fileSize ?? 0)
                if out.found && size <= out.sizeBytes {
                    continue
                }
                out.found = true
                out.path = url.path
                out.sizeBytes = size
            }
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
