---
id: VS-016
titre: Releases GitHub avec l'exe attaché, à partir de v0.1.0
statut: terminé
priorité: haute
dépend-de: [VS-007]
créé: 2026-08-06
mis-à-jour: 2026-08-06
---

## Contexte

Thibault : « c'est normal qu'il n'y a pas l'exe dans les releases ? » — oui : la CI
n'uploadait qu'un artifact de workflow, aucun job de release ni tag n'existait.

## Critères d'acceptation

- [x] Job `release` (on tag v*) : crée la GitHub Release (notes auto) et y attache
      `vibesync.exe` (via `gh` préinstallé sur le runner, pas d'action tierce)
- [x] Tag `v0.1.0` poussé, release visible avec l'exe (176 640 o) téléchargeable
- [x] Guides amis : URL réelle — et le repo est passé PUBLIC (06-08), les amis
      téléchargent sans compte

## Journal du ticket

- 2026-08-06 : créé (orchestrateur).
- 2026-08-06 : release v0.1.0 en ligne, repo public. Terminé.
