---
id: VS-023
titre: Versions visibles + notification de nouvelle release (simple)
statut: ouvert
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

- [ ] `VERSION` racine + injection : Dockerfile (ldflags), build.bat (-D), à venir mac
- [ ] Spec + protocol.go : `Welcome.serverVersion`, `Welcome.downloadUrl`
      (env `VIBESYNC_DOWNLOAD_URL`, défaut releases GitHub) — champs additifs
- [ ] Serveur : version dans le log de démarrage et dans le welcome
- [ ] Client C : version affichée (connexion + salle), comparaison semver simple,
      bannière cliquable (ShellExecute) si serveur plus récent, non bloquante
- [ ] Client Go de référence : même logique (toast), pour les tests
- [ ] Question ouverte à trancher par Thibault : repo public vs distribution de
      l'exe par le serveur (`/download`) — le lien des guides dépend de la réponse

## Journal du ticket

- 2026-08-06 : créé.
