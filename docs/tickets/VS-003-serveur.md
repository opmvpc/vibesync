---
id: VS-003
titre: Serveur — salles, WebSocket, état autoritatif
statut: ouvert
priorité: haute
dépend-de: [VS-002]
créé: 2026-08-05
mis-à-jour: 2026-08-05
---

## Contexte

`cmd/vibesync-server` + `internal/server`. Comportements définis dans
`docs/protocol.md` (§Modèle, §Comportements serveur). Délégué à un agent Opus.

## Critères d'acceptation

- [ ] `/ws` (hello, ping/pong, control, report, ready, chat, toasts), `/healthz`
- [ ] État de salle autoritatif avec compensation de latence de l'émetteur
- [ ] Ready-gate au premier démarrage, pause auto (buffering/retard/déconnexion)
- [ ] Config par env : `VIBESYNC_ADDR`, `VIBESYNC_PASSWORD`
- [ ] Tests unitaires (logique de salle) + test d'intégration WS multi-clients
- [ ] `go vet` + `staticcheck` propres

## Journal du ticket

- 2026-08-05 : créé.
