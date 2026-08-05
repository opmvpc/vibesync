---
id: VS-004
titre: Client — driver VLC HTTP + moteur de sync
statut: ouvert
priorité: haute
dépend-de: [VS-002]
créé: 2026-08-05
mis-à-jour: 2026-08-05
---

## Contexte

`internal/vlc` (lancement VLC + poll status.json + commandes) et `internal/client`
(offset d'horloge, drift nudge/seek, détection d'actions utilisateur, reconnexion).
Spec : `docs/protocol.md` §Comportements client, ADR-003. Délégué à un agent Opus.

## Critères d'acceptation

- [ ] Driver VLC derrière une interface Go, localisation de VLC Win/mac, port+password aléatoires
- [ ] Position fine via `position × length` ; commandes pause/resume/seek/rate
- [ ] Moteur de sync complet (offset médian, nudge 1,05×, seek ≥ 2 s, fenêtre anti-boucle)
- [ ] Faux VLC httptest + tests : convergence après seek, détection pause manuelle, rejoin
- [ ] `go vet` + `staticcheck` propres

## Journal du ticket

- 2026-08-05 : créé.
