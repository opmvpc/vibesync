---
id: VS-025
titre: Mémoriser le mot de passe serveur, chiffré par l'OS (DPAPI / Keychain)
statut: ouvert
priorité: haute
dépend-de: [VS-022]
créé: 2026-08-06
mis-à-jour: 2026-08-06
---

## Contexte

Thibault : « la flemme de le taper à chaque fois ». Zéro dépendance oblige :
chiffrement par l'OS, jamais de mot de passe en clair sur disque, pas de crypto
maison.

## Critères d'acceptation

- [ ] Windows : DPAPI (`CryptProtectData`/`CryptUnprotectData`, crypt32, entropie
      applicative fixe) → blob hex dans vibesync.ini (`password_enc=`) ; jamais de
      clair écrit ; case « Se souvenir du mot de passe » (cochée par défaut) sur
      l'écran de connexion ; déchiffrement au chargement, champ prérempli masqué ;
      blob invalide/autre machine → champ vide sans erreur bruyante
- [ ] La valeur en mémoire est effacée (SecureZeroMemory) quand elle n'est plus utile
- [ ] Tests : round-trip DPAPI, blob corrompu, ini sans entrée, migration d'un ini
      existant
- [ ] macOS (au polissage Swift) : Keychain Services, même UX

## Journal du ticket

- 2026-08-06 : créé.
