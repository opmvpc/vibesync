// Version.swift — version applicative du client, et frontière avec la
// comparaison de versions du C commun (ADR-010).
//
// Ce fichier ne décide plus rien. La règle « le serveur est-il plus récent que
// moi ? » vit dans `proto_newer_version` (core/src/protocol.c), portage exact de
// internal/client/version.go, et les deux clients natifs l'appellent — c'était
// le solde du bloc 3 de VS-033, tranché par VS-036. Le port Swift qui vivait
// ici (parse + comparaison) a disparu avec ses 35 cas de test, qui rejouent
// maintenant le chemin C ; le C commun porte les mêmes cas (test_core.c).
//
// Ce qui reste ici est de la PLATEFORME : d'où sort la version de ce binaire.
// Rappel : la comparaison ne sert qu'à proposer un téléchargement, jamais à
// bloquer — le garde-fou dur reste la version de PROTOCOLE, refusée au hello.

import Foundation
import VSCore

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

    /// Dit si `remote` est strictement plus récente que `local`. Faux dès que
    /// l'une des deux est illisible : un client « dev » ou un serveur muet ne
    /// doit jamais provoquer d'invitation à mettre à jour. Aucune arène : la
    /// fonction C ne fait que lire les deux chaînes.
    public static func newer(_ remote: String, than local: String) -> Bool {
        return withStr8(remote) { r in
            withStr8(local) { l in
                proto_newer_version(r, l) != 0
            }
        }
    }
}
