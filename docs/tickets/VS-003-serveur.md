---
id: VS-003
titre: Serveur — salles, WebSocket, état autoritatif
statut: terminé
priorité: haute
dépend-de: [VS-002]
créé: 2026-08-05
mis-à-jour: 2026-08-05
---

## Contexte

`cmd/vibesync-server` + `internal/server`. Comportements définis dans
`docs/protocol.md` (§Modèle, §Comportements serveur). Délégué à un agent Opus.

## Critères d'acceptation

- [x] `/ws` (hello, ping/pong, control, report, ready, chat, toasts), `/healthz`
- [x] État de salle autoritatif avec compensation de latence de l'émetteur
- [x] Ready-gate au premier démarrage, pause auto (buffering/retard/déconnexion)
- [x] Config par env : `VIBESYNC_ADDR`, `VIBESYNC_PASSWORD` (+ plafonds anti-abus)
- [x] Tests unitaires (logique de salle) + test d'intégration WS multi-clients (~50 tests)
- [x] `go vet` + `staticcheck` propres

## Journal du ticket

- 2026-08-05 : créé.
- 2026-08-05 : livré par agent Opus A ; review codex terra → anti-flood (token bucket),
  plafonds env, hash SHA-256 constant-time, rejet Unicode Cf, tests déterministes.
  Vérifié et intégré par l'orchestrateur (build/vet/staticcheck/tests ×2 shuffle verts).
