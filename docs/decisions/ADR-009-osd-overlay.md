---
id: ADR-009
titre: OSD « à la Syncplay » via fenêtre overlay maison, pas via VLC
statut: accepté
date: 2026-08-06
---

## Contexte

Thibault veut des messages sur la vidéo (« X a mis pause », « X a avancé à 12:34 »)
comme Syncplay. Syncplay les affiche via son script Lua (`vlc.osd`), voie exclue par
ADR-003/008. Recherche VS-020 (testée sur VLC 3.0.20 réel) : la piste RC + filtre
marquee est morte depuis VLC 3.x (`oldrc` a une table de commandes fixe, plus de
dispatch `@module var valeur`), et l'interface HTTP n'expose aucun OSD.
Rapport : `research/2026-08-06-recherche-osd-vlc.md`.

## Décision

L'OSD est rendu par **notre propre fenêtre overlay** posée sur celle de VLC :
- Windows : fenêtre `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE`,
  suivie sur le rect de la fenêtre VLC (handle connu : processus lancé par nous ;
  suivi par polling léger ou événement WinEvent), texte GDI avec fondu, clics
  traversants, aucune interaction volée.
- macOS : `NSWindow` sans bordure au niveau au-dessus de la fenêtre VLC, même logique.
- Alimenté par les événements déjà reçus (roomState.setBy, toasts serveur),
  messages sobres en français, désactivable dans les réglages.
- Repli (VLC en plein écran exclusif ou fenêtre introuvable) : toasts de l'app.

## Alternatives écartées

- Script Lua : réintroduit l'installation d'un fichier chez chaque ami (ADR-003).
- RC/marquee et HTTP : infaisables sur VLC 3.x (recherche VS-020).
- Toasts app uniquement : ne répond pas au « sur la vidéo » demandé.

## Conséquences

- Implémentation par plateforme (ui/win32, ui/macos) mais petite et isolée.
- Le plein écran VLC sous Windows reste une fenêtre normale → overlay OK dans la
  plupart des cas ; cas exclusifs documentés avec le repli.
