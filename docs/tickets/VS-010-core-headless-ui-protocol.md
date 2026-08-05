---
id: VS-010
titre: Cœur headless + spec officielle du canal local /ui
statut: ouvert
priorité: haute
dépend-de: [VS-006]
créé: 2026-08-05
mis-à-jour: 2026-08-05
---

## Contexte

ADR-006 : les apps natives pilotent le cœur Go via le WS local /ui. L'agent B a reçu
consigne d'ajouter `--headless` + annonce `{"uiPort","uiToken"}` sur stdout et de
documenter un draft du canal (`docs/research/2026-08-05-ui-protocol-draft.md`).

## Critères d'acceptation

- [ ] `docs/ui-protocol.md` : spec versionnée du canal /ui (messages bidirectionnels,
      auth par token, exemples JSON, règles d'évolution)
- [ ] `vibesync --headless` fonctionnel (aucun navigateur, annonce stdout, arrêt propre
      quand l'app parente ferme le WS ou envoie `shutdown`)
- [ ] Tests : auth token refusée/acceptée, scénario UI factice complet (connexion →
      choix fichier → ready → play/pause/seek → chat → déconnexion)

## Journal du ticket

- 2026-08-05 : créé (pivot natif ADR-006).
