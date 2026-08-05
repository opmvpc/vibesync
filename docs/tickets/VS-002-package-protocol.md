---
id: VS-002
titre: Package Go internal/protocol (types + enveloppe + version)
statut: terminé
priorité: haute
dépend-de: [VS-001]
créé: 2026-08-05
mis-à-jour: 2026-08-05
---

## Contexte

Types partagés serveur/client, miroir exact de `docs/protocol.md`. Écrit par
l'orchestrateur pour débloquer le travail parallèle des deux agents Opus.

## Critères d'acceptation

- [x] Types de tous les messages C→S et S→C + enveloppe `{type,data}`
- [x] Helpers Encode/Decode avec erreurs typées, `Version = 1`
- [x] Tests de round-trip JSON (`go test ./internal/protocol`)

## Journal du ticket

- 2026-08-05 : créé.
- 2026-08-05 : implémenté par l'orchestrateur, tests verts, terminé.
