---
id: VS-027
titre: Icônes in-app mal rasterisées — logo ≠ icône de l'app, roue dentée difforme
statut: terminé
priorité: moyenne
dépend-de: [VS-019]
créé: 2026-08-06
mis-à-jour: 2026-08-06
---

## Contexte

Retour de Thibault (capture v0.2.0) : le petit logo à gauche du titre « vibesync »
est une pastille dessinée à la main qui ne ressemble pas à l'icône de l'app
(assets/vibesync.ico, pourtant embarquée dans l'exe), et la roue dentée du bouton
Réglages est « cheloue » (polygones GDI aliasés à basse résolution).

## Critères d'acceptation

- [ ] Le logo in-app est rendu depuis la ressource icône de l'exe (DrawIconEx,
      taille selon DPI) → toujours identique à l'icône de l'app
- [ ] Roue dentée (et autres glyphes main visiblement crénelés) : rendu net —
      supersampling dans un DIB + AlphaBlend, ou géométrie corrigée
- [ ] Vérification visuelle par captures PNG (mécanisme existant), avant/après,
      100 % de DPI minimum
- [ ] Captures des guides rafraîchies si l'écran de connexion y apparaît
- [ ] build.bat test + asan verts (1 282 checks, 13/13 vecteurs), budget < 500 Ko

## Journal du ticket

- 2026-08-06 : créé (retour v0.2.0), assigné à un agent Opus (avec boucle
  build → capture → inspection visuelle).
- 2026-08-06 : livré (a63f207). Le vrai problème de la roue était la géométrie
  (2 rayons alternés = étoile, pas des dents) EN PLUS du crénelage. Logo rendu
  depuis la ressource icône (moyenne de blocs 256→taille DPI), supersampling ×4
  maison pour roue/coche/play/pastille. Vérifié à 100 % et 150 % de DPI. Terminé.
