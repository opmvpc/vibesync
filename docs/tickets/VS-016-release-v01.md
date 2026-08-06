---
id: VS-016
titre: Releases GitHub avec l'exe attaché, à partir de v0.1.0
statut: en-cours
priorité: haute
dépend-de: [VS-007]
créé: 2026-08-06
mis-à-jour: 2026-08-06
---

## Contexte

Thibault : « c'est normal qu'il n'y a pas l'exe dans les releases ? » — oui : la CI
n'uploadait qu'un artifact de workflow, aucun job de release ni tag n'existait.

## Critères d'acceptation

- [ ] Job `release` (on tag v*) : crée la GitHub Release (notes auto) et y attache
      `vibesync.exe` (via `gh` préinstallé sur le runner, pas d'action tierce)
- [ ] Tag `v0.1.0` poussé, release visible avec l'exe téléchargeable
- [ ] Guides amis : URL de release réelle à la place du placeholder

## Journal du ticket

- 2026-08-06 : créé (orchestrateur).
