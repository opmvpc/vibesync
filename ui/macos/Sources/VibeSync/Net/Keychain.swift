// Keychain.swift — stockage du mot de passe serveur (VS-025).
//
// Équivalent macOS du DPAPI du client Windows : c'est le système qui chiffre,
// on n'écrit jamais de clair sur disque et on n'écrit pas de crypto maison
// (ADR-008 : zéro dépendance, mais les frameworks du système sont à nous).
// API C de Security.framework, aucun paquet SPM.
//
// Le trousseau est faillible (session sans trousseau déverrouillé, binaire non
// signé refusé par l'ACL…) : toute opération peut échouer, et un échec n'est
// jamais fatal — on retombe simplement sur « pas de mot de passe mémorisé ».

import Foundation
import Security

public enum Keychain {

    /// Service commun à toutes nos entrées (identifiant du bundle).
    public static let service = "org.vibesync.client"
    /// Compte de l'unique secret d'aujourd'hui : le mot de passe du serveur.
    /// Comme le client Windows, il n'y en a qu'un, pas un par serveur.
    public static let serverPasswordAccount = "server-password"

    private static func query(_ account: String) -> [String: Any] {
        return [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ]
    }

    /// Lit un secret. `nil` si absent, illisible ou non UTF-8.
    public static func read(account: String) -> String? {
        var q = query(account)
        q[kSecReturnData as String] = true
        q[kSecMatchLimit as String] = kSecMatchLimitOne
        var item: CFTypeRef?
        let status = SecItemCopyMatching(q as CFDictionary, &item)
        guard status == errSecSuccess, let data = item as? Data else {
            return nil
        }
        return String(data: data, encoding: .utf8)
    }

    /// Écrit (ou remplace) un secret. Faux si le trousseau refuse.
    @discardableResult
    public static func write(_ value: String, account: String) -> Bool {
        let data = Data(value.utf8)
        // Mise à jour d'abord : SecItemAdd échouerait en doublon.
        let update: [String: Any] = [kSecValueData as String: data]
        let status = SecItemUpdate(query(account) as CFDictionary, update as CFDictionary)
        if status == errSecSuccess {
            return true
        }
        if status != errSecItemNotFound {
            return false
        }
        var add = query(account)
        add[kSecValueData as String] = data
        // Accessible dès le premier déverrouillage : l'app peut être relancée
        // par la session sans que l'utilisateur ait retapé quoi que ce soit.
        add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlock
        return SecItemAdd(add as CFDictionary, nil) == errSecSuccess
    }

    /// Supprime un secret (absent = succès).
    @discardableResult
    public static func delete(account: String) -> Bool {
        let status = SecItemDelete(query(account) as CFDictionary)
        return status == errSecSuccess || status == errSecItemNotFound
    }
}
