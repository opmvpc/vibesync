---
id: VS-019
titre: Icône de l'app — SVG source + .ico généré (outil Go maison)
statut: ouvert
priorité: normale
dépend-de: []
créé: 2026-08-06
mis-à-jour: 2026-08-06
---

## Contexte

Demande de Thibault (« une belle icône en SVG puis transformée en format compatible
exe », par Opus). Zéro dépendance : pas de rasteriseur SVG tiers → le design est
décliné en SVG (source/documentation) ET en générateur Go stdlib (image/draw) qui
produit les PNG multi-tailles et les emballe en .ico (format conteneur trivial).

## Critères d'acceptation

- [ ] `assets/icon.svg` (design final) + `tools/genicon/main.go` (stdlib pur)
- [ ] `assets/vibesync.ico` généré (16/24/32/48/64/128/256, PNG dans l'ico)
- [ ] Intégration .rc + WM_SETICON (orchestrateur ou VS-018), visible barre des
      tâches/alt-tab/explorateur
- [ ] Icône aussi utilisée par le bundle macOS plus tard (PNG exportés)

## Journal du ticket

- 2026-08-06 : créé.
