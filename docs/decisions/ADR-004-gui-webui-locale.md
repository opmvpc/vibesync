---
id: ADR-004
titre: GUI du client = web UI locale embarquée dans le binaire
statut: remplacé-par ADR-005
date: 2026-08-05
---

## Contexte

Exigence : client graphique « pratique, bien foutu et joli », Windows + macOS Apple
Silicon. Les toolkits Go natifs (Fyne) utilisent cgo/OpenGL → cross-compilation
pénible ; Wails exige un build sur macOS pour la cible mac ; Electron est lourd.
On ne dispose que d'une machine Windows pour builder.

## Décision

Le binaire client embarque une **web UI** (`embed.FS`) servie sur `127.0.0.1:<port>`
et ouvre le navigateur par défaut au lancement. UI single-page soignée (dark, moderne),
vanilla JS + CSS, communication avec le cœur Go par WebSocket local. Sélection du
fichier vidéo via un explorateur de fichiers fourni par le backend local (le navigateur
ne donne pas les chemins complets).

## Alternatives écartées

- Fyne/Wails : impossible de produire la cible macOS depuis Windows sans CI mac ;
  garde-fou : si un jour on veut du natif, le cœur (engine/vlc/protocol) est découplé
  de l'UI.
- App console : rejetée par l'exigence « client graphique joli ».

## Conséquences

- Une seule base UI pour les deux OS, cross-compilée trivialement (pure Go).
- macOS : binaire non signé → doc « clic droit → Ouvrir » (Gatekeeper) dans le README.
- Une CI GitHub Actions pourra plus tard produire des builds signés/notarisés si besoin.
