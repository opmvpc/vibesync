---
id: VS-004
titre: Client — driver VLC HTTP + moteur de sync
statut: terminé
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

- [x] Driver VLC derrière une interface Go, localisation de VLC Win/mac, port+password aléatoires
- [x] Position fine via `position × length` ; commandes pause/resume/seek/rate
- [x] Moteur de sync complet (offset médian, nudge 1,05×, seek ≥ 2 s, fenêtre anti-boucle)
- [x] Faux VLC httptest + tests : convergence après seek, détection pause manuelle, rejoin
- [x] `go vet` + `staticcheck` propres

## Journal du ticket

- 2026-08-05 : créé.
- 2026-08-05 : livré par agent Opus B (68 tests) ; 2 trous de spec comblés (hold 2 s,
  seuil pause 0,6 s) gelés dans la spec.
- 2026-08-05 : review codex sol high → 16 findings corrigés (dont hold/écho, phase de
  reconnexion, golden vectors) + 2 bugs bonus trouvés en corrigeant. 12 vecteurs de
  conformité committés dans test/vectors/. Vérifié et intégré par l'orchestrateur.
