// Preferences.swift — réglages persistants du client (équivalent du vibesync.ini
// du client Windows).
//
// Support : UserDefaults, sauf le mot de passe du serveur qui va au trousseau
// (Keychain.swift). Le magasin passe par un protocole pour que les tests
// n'écrivent rien dans le profil de l'utilisateur.

import Foundation
import VSCore

/// Le peu qu'on demande à un magasin de réglages. UserDefaults le remplit tel
/// quel ; les tests fournissent une version en mémoire.
public protocol PrefStore: AnyObject {
    func string(forKey key: String) -> String?
    func stringArray(forKey key: String) -> [String]?
    func object(forKey key: String) -> Any?
    func set(_ value: Any?, forKey key: String)
}

extension UserDefaults: PrefStore {}

public enum Preferences {

    public static let keyServer = "vibesync.server"
    public static let keyName = "vibesync.name"
    public static let keyRoom = "vibesync.room"
    /// Jeton de reprise de session, persisté depuis VS-028 : sans lui, relancer
    /// l'app donne un jeton neuf, le serveur ne reconnaît plus le détenteur du
    /// pseudo et le refuse (`name_taken`) tant que la connexion zombie vit.
    public static let keySession = "vibesync.session"
    public static let keyRememberPassword = "vibesync.rememberPassword"
    public static let keyMediaDirs = "vibesync.mediaDirs"

    /// Nombre de dossiers médias retenus (même borne que le client Windows).
    public static let maxMediaDirs = 8

    /// Suite UserDefaults alternative. Deux instances lancées sur la MÊME
    /// machine (harnais de test réel) partageraient sinon jeton de session et
    /// réglages : le serveur verrait deux fois le même jeton et la reprise de
    /// session de VS-028 se retournerait contre nous. `HOME` ne suffit pas —
    /// les préférences passent par cfprefsd, qui résout le dossier de
    /// l'utilisateur lui-même.
    public static let suiteEnv = "VIBESYNC_SUITE"

    /// Magasin de réglages à utiliser pour ce processus.
    public static func store(
        _ env: [String: String] = ProcessInfo.processInfo.environment
    ) -> PrefStore {
        let suite = (env[suiteEnv] ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        if !suite.isEmpty, let alternate = UserDefaults(suiteName: suite) {
            return alternate
        }
        return UserDefaults.standard
    }

    // MARK: - Jeton de session

    /// Rend le jeton de reprise de session, en le tirant et en l'enregistrant
    /// au premier lancement. Un jeton illisible ou tronqué est remplacé : mieux
    /// vaut un jeton neuf qu'un jeton que le serveur refusera.
    public static func sessionToken(_ store: PrefStore) -> String {
        if let existing = store.string(forKey: keySession), validSessionToken(existing) {
            return existing
        }
        let token = Proto.sessionToken()
        store.set(token, forKey: keySession)
        return token
    }

    /// Borne haute du jeton relu (même valeur que le client Go) : le serveur
    /// refuse plus long, inutile de le conserver.
    public static let maxSessionTokenLength = Int(VS_SESSION_TOKEN_MAX)

    /// Forme attendue par la spec : hexadécimal, au moins 16 octets. La règle
    /// est celle du C commun (proto_session_token_valid), pas une copie.
    public static func validSessionToken(_ token: String) -> Bool {
        return Proto.isValidSessionToken(token)
    }

    // MARK: - Mot de passe mémorisé (VS-025)

    /// Cochée par défaut, comme sur Windows : la demande d'origine est « la
    /// flemme de le taper à chaque fois ».
    public static func rememberPassword(_ store: PrefStore) -> Bool {
        guard let raw = store.object(forKey: keyRememberPassword) as? Bool else {
            return true
        }
        return raw
    }

    public static func setRememberPassword(_ value: Bool, _ store: PrefStore) {
        store.set(value, forKey: keyRememberPassword)
    }

    // MARK: - Dossiers médias (VS-026)

    /// Dossiers où chercher le fichier déclaré par un participant. Défaut :
    /// le dossier Téléchargements de l'utilisateur.
    public static func mediaDirs(_ store: PrefStore) -> [String] {
        guard let raw = store.stringArray(forKey: keyMediaDirs) else {
            return defaultMediaDirs()
        }
        return normalizeMediaDirs(raw)
    }

    public static func setMediaDirs(_ dirs: [String], _ store: PrefStore) {
        store.set(normalizeMediaDirs(dirs), forKey: keyMediaDirs)
    }

    /// Nettoie une liste de dossiers : rognage, vides écartés, doublons
    /// écartés, liste bornée.
    public static func normalizeMediaDirs(_ dirs: [String]) -> [String] {
        var out: [String] = []
        for raw in dirs {
            let dir = raw.trimmingCharacters(in: .whitespacesAndNewlines)
            if dir.isEmpty || out.contains(dir) {
                continue
            }
            out.append(dir)
            if out.count >= maxMediaDirs {
                break
            }
        }
        return out
    }

    public static func defaultMediaDirs() -> [String] {
        let urls = FileManager.default.urls(for: .downloadsDirectory, in: .userDomainMask)
        guard let downloads = urls.first else {
            return []
        }
        return [downloads.path]
    }
}
