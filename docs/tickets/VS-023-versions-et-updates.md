---
id: VS-023
titre: Versions visibles + notification de nouvelle release (simple)
statut: terminé
priorité: haute
dépend-de: []
créé: 2026-08-06
mis-à-jour: 2026-08-06
---

## Contexte

Demande de Thibault : voir les versions client/serveur dans l'UI (debug), être
prévenu qu'une release existe, gérer la compat sans usine à gaz. Design : le
protocole v1 reste le garde-fou dur (`version_mismatch`) ; serveur et clients
sortant du même tag, le `welcome` porte `serverVersion` + `downloadUrl` → si
serveur > client (semver), bannière « nouvelle version » cliquable. Source unique :
fichier `VERSION` à la racine, injecté au build (ldflags Go, -D en C, plist Swift).

## Critères d'acceptation

- [x] `VERSION` racine + injection : Dockerfile (ldflags), build.bat (-D), à venir mac
- [x] Spec + protocol.go : `Welcome.serverVersion`, `Welcome.downloadUrl`
      (env `VIBESYNC_DOWNLOAD_URL`, défaut releases GitHub) — champs additifs
- [x] Serveur : version dans le log de démarrage et dans le welcome
- [x] Client C : version affichée (connexion + salle), comparaison semver simple,
      bannière cliquable (ShellExecute) si serveur plus récent, non bloquante
- [x] Client Go de référence : même logique (toast), pour les tests — fix review
      terra : version injectée par ldflags dans cmd/vibesync (+ --version),
      NewerVersion ordonne les pré-releases (1.2.3-rc1 < 1.2.3)
- [x] Question tranchée : repo public → lien des guides = releases GitHub

## Journal du ticket

- 2026-08-06 : créé.
- 2026-08-06 : livré, puis fix terra n°5 (c811207) : le client livré ne recevait
  jamais sa version. Dans v0.2.0. Terminé (injection mac au polissage Swift).
- 2026-08-06 (soir) : injection mac livrée (73499fc) — clé Info.plist
  VibeSyncVersion écrite par scripts/build-macos.sh depuis VERSION, port de
  NewerVersion (Version.swift, aligné strconv.Atoi/TrimSpace), bannière de
  mise à jour dans l'UI.
