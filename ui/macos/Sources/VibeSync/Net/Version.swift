// Version.swift — version applicative du client et comparaison semver simple.
//
// Port de internal/client/version.go (VS-023). Volontairement minimal : le
// garde-fou dur de compatibilité reste la version de PROTOCOLE, refusée par le
// serveur au hello. Ici on ne cherche qu'à dire « le serveur est plus récent que
// moi » pour proposer un téléchargement — jamais pour bloquer quoi que ce soit.
//
// Format accepté : `major[.minor[.patch]]`, chiffres seulement, avec un « v »
// initial optionnel, un suffixe de pré-release (`-rc1`) et des métadonnées de
// build (`+sha`). Les composants absents valent 0. Tout le reste (« dev », vide,
// texte) est illisible : dans le doute, on ne dit rien.

import Foundation

public enum AppVersion {

    /// Version d'un client non versionné : illisible en semver, donc jamais de
    /// bannière de mise à jour. C'est ce que rend un binaire lancé hors bundle
    /// (swift run, tests).
    public static let dev = "dev"

    /// Clé Info.plist renseignée par scripts/build-macos.sh depuis le fichier
    /// VERSION de la racine du dépôt. Une clé à nous, pas
    /// CFBundleShortVersionString : le bundle d'un hôte de tests en porte une,
    /// et elle n'a rien à voir avec vibesync.
    public static let infoKey = "VibeSyncVersion"

    /// Version de CE client. Constante pour la vie du processus.
    public static let current: String = {
        guard let raw = Bundle.main.object(forInfoDictionaryKey: infoKey) as? String else {
            return dev
        }
        let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? dev : trimmed
    }()

    /// maxPart borne chaque composant : au-delà, c'est une saisie absurde plutôt
    /// qu'une version.
    static let maxPart = 1_000_000

    struct Parsed {
        var parts: [Int] = [0, 0, 0]
        var pre: Bool = false
    }

    static func parse(_ text: String) -> Parsed? {
        var out = Parsed()
        // strings.TrimSpace en Go coupe aussi \n, \r, \t et \v : .whitespaces
        // seul laisserait passer un VERSION lu avec son saut de ligne.
        var s = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if s.hasPrefix("v") {
            s.removeFirst()
        }
        // Métadonnées de build : hors de l'ordre, on les coupe d'abord.
        if let i = s.firstIndex(of: "+") {
            s = String(s[s.startIndex..<i])
        }
        // Pré-release : elle ne change pas le triplet mais le déclasse.
        if let i = s.firstIndex(of: "-") {
            out.pre = s.index(after: i) < s.endIndex
            s = String(s[s.startIndex..<i])
        }
        if s.isEmpty || s.contains(" ") || s.contains("\t") {
            return nil
        }
        let parts = s.components(separatedBy: ".")
        if parts.count > 3 {
            return nil
        }
        for (index, part) in parts.enumerated() {
            if part.isEmpty {
                return nil
            }
            // Comme strconv.Atoi : un signe est toléré (« +1 »), le négatif est
            // écarté par la borne basse. Aucun signe ne peut de toute façon
            // survivre aux coupes sur « + » et « - » faites plus haut.
            guard let n = Int(part), n >= 0, n <= maxPart else {
                return nil
            }
            out.parts[index] = n
        }
        return out
    }

    /// Dit si `remote` est strictement plus récente que `local`. Faux dès que
    /// l'une des deux est illisible : un client « dev » ou un serveur muet ne
    /// doit jamais provoquer d'invitation à mettre à jour.
    public static func newer(_ remote: String, than local: String) -> Bool {
        guard let r = parse(remote), let l = parse(local) else {
            return false
        }
        for i in 0..<3 where r.parts[i] != l.parts[i] {
            return r.parts[i] > l.parts[i]
        }
        // Même triplet : seule une stable dépasse une pré-release.
        return !r.pre && l.pre
    }
}
