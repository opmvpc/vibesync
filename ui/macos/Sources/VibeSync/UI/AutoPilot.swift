// AutoPilot.swift — pilotage de l'application par variables d'environnement,
// pour le harnais de test réel (scripts/run-real-macos.sh).
//
// Le client macOS n'a pas de ligne de commande : c'est une app AppKit. Pour
// qu'un script puisse mener une vraie séance à deux clients sur une seule
// machine, on lui donne trois choses et rien de plus :
//
//   1. de quoi se connecter sans interaction (URL, pseudo, salle, mot de passe,
//      fichier à ouvrir) ;
//   2. un fichier d'état JSON réécrit chaque seconde, sur lequel le script
//      fonde ses assertions ;
//   3. un fichier de commandes que le script remplit ligne à ligne
//      (`play`, `pause`, `seek 42`, `ready`, `chat coucou`, `quit`) et que
//      l'app consomme au fil de sa boucle.
//
// Un fichier de commandes plutôt qu'un scénario timé en dur : le script décide
// QUAND passer à l'étape suivante, une fois son point de contrôle vérifié. Un
// scénario minuté ferait échouer le test au premier VLC un peu lent.
//
// **Rien de tout cela n'est actif hors mode auto** : sans `VIBESYNC_AUTO_URL`,
// `AutoPilot.fromEnvironment` rend `nil` et l'application se comporte comme
// d'habitude.

import Foundation

public struct AutoPilot {

    /// Préfixe commun des variables du pilote.
    public static let prefix = "VIBESYNC_AUTO_"

    /// Serveur (`ws://` ou `wss://`). Sa seule présence active le mode auto.
    public var url: String = ""
    public var name: String = "auto"
    public var room: String = "salon"
    public var password: String = ""
    /// Média à ouvrir dans VLC au démarrage (vide = aucun).
    public var file: String = ""
    /// Fichier d'état JSON réécrit périodiquement (vide = pas d'état).
    public var statusPath: String = ""
    /// Fichier de commandes lu au fil de l'eau (vide = pas de pilotage).
    public var commandsPath: String = ""
    /// Étiquette libre, recopiée dans l'état : sert au script à distinguer ses
    /// instances dans les journaux.
    public var scenario: String = ""

    /// Période de réécriture de l'état.
    public static let statusEverySec: Double = 1.0

    public init() {}

    /// Lit la configuration du pilote. `nil` = mode normal, aucun changement de
    /// comportement.
    public static func fromEnvironment(
        _ env: [String: String] = ProcessInfo.processInfo.environment
    ) -> AutoPilot? {
        func value(_ key: String) -> String {
            return (env[prefix + key] ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        }
        let url = value("URL")
        if url.isEmpty {
            return nil
        }
        var out = AutoPilot()
        out.url = url
        let name = value("NAME")
        if !name.isEmpty {
            out.name = name
        }
        let room = value("ROOM")
        if !room.isEmpty {
            out.room = room
        }
        // Le mot de passe n'est pas rogné à droite : un espace final est un
        // caractère du secret comme un autre.
        out.password = env[prefix + "PASSWORD"] ?? ""
        out.file = value("FILE")
        out.statusPath = value("STATUS")
        out.commandsPath = value("CMDS")
        out.scenario = value("SCENARIO")
        return out
    }

    // MARK: - Commandes

    /// Une commande reconnue dans le fichier de commandes.
    public enum Command: Equatable {
        case play
        case pause
        case seek(Double)
        case ready(Bool)
        case chat(String)
        case openFile(String)
        case quit
    }

    /// Analyse une ligne de commande. `nil` : ligne vide, commentaire (`#`) ou
    /// verbe inconnu — on ignore plutôt que d'échouer, le fichier est écrit par
    /// un script shell.
    public static func parse(_ raw: String) -> Command? {
        let line = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if line.isEmpty || line.hasPrefix("#") {
            return nil
        }
        let verb: String
        var rest = ""
        if let space = line.firstIndex(of: " ") {
            verb = String(line[line.startIndex..<space]).lowercased()
            rest = String(line[line.index(after: space)...]).trimmingCharacters(in: .whitespaces)
        } else {
            verb = line.lowercased()
        }
        switch verb {
        case "play":
            return .play
        case "pause":
            return .pause
        case "seek":
            guard let sec = Double(rest), sec.isFinite else {
                return nil
            }
            return .seek(sec)
        case "ready":
            return .ready(rest != "0" && rest.lowercased() != "false")
        case "unready":
            return .ready(false)
        case "chat":
            return rest.isEmpty ? nil : .chat(rest)
        case "open":
            return rest.isEmpty ? nil : .openFile(rest)
        case "quit":
            return .quit
        default:
            return nil
        }
    }
}
