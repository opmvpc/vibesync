---
id: VS-020
titre: Messages OSD dans VLC (« X a mis pause », « X a seek à 12:34 »)
statut: ouvert
priorité: normale
dépend-de: [VS-017]
créé: 2026-08-06
mis-à-jour: 2026-08-06
---

## Contexte

Demande de Thibault, comme dans Syncplay original (qui passe par son script Lua).
Notre driver est HTTP-only : l'interface HTTP n'expose pas d'OSD. Piste : activer
en plus l'interface RC (`--extraintf`) et pousser le filtre marquee (`marq`) par le
socket RC — à valider par une recherche avant d'implémenter.

## Critères d'acceptation

- [ ] Recherche : faisabilité OSD via RC/marq (ou alternative 0-dépendance),
      rapport dans docs/research/
- [ ] Si faisable : spec amendée, implémentation Go de référence + port C (+ Swift),
      messages français sobres, désactivable dans les réglages
- [ ] Si infaisable proprement : décision documentée (ADR court) + repli (toasts app)

## Journal du ticket

- 2026-08-06 : créé.
