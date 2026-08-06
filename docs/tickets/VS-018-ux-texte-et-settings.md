---
id: VS-018
titre: UX client Windows — sélection texte à la souris + panneau réglages in-app
statut: ouvert
priorité: haute
dépend-de: [VS-014]
créé: 2026-08-06
mis-à-jour: 2026-08-06
---

## Contexte

Retours de Thibault : (1) impossible de sélectionner du texte dans les champs
(navigation aux flèches uniquement) ; (2) il veut régler les paramètres dans
l'app plutôt qu'en éditant vibesync.ini.

## Critères d'acceptation

- [ ] Champs texte complets : clic place le caret, drag sélectionne, double-clic
      mot, Shift+flèches/Home/End, Ctrl+A/C/X/V, rendu de sélection
- [ ] Panneau Réglages (engrenage) : chemin VLC (avec détection auto affichée),
      serveur par défaut, pseudo, options utiles — lit/écrit le même ini
- [ ] Tests ini/édition étendus ; build.bat test + asan verts ; budget 500 Ko tenu

## Journal du ticket

- 2026-08-06 : créé (retours terrain).
