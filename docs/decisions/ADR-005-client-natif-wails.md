---
id: ADR-005
titre: Client natif — coquille Wails autour de la web UI embarquée
statut: accepté
date: 2026-08-05
---

## Contexte

Thibault veut finalement un **client natif** (pas un onglet de navigateur). Nouvelle
donnée : un Mac est disponible (dès demain) pour builder la cible macOS, ce qui lève
la contrainte qui avait motivé ADR-004. Le cœur du client (internal/vlc,
internal/client, internal/webui) est découplé de la façon dont l'UI est affichée.

## Décision

- L'UI reste la web UI embarquée (HTML/CSS/JS via embed.FS), mais elle est affichée
  dans une **fenêtre native Wails v2** (WebView2 sous Windows, WKWebView sous macOS)
  au lieu du navigateur : vraie app avec icône, fenêtre, pas de chrome de navigateur.
- Le mode navigateur est conservé en secours derrière un flag `--browser`.
- Builds par OS : Windows sur la machine de dev, macOS (arm64) sur le Mac via un
  script `scripts/build-macos.sh` + doc pas-à-pas.
- ADR-004 est **remplacé par** le présent ADR (le contenu UI/UX reste valable, seul
  le conteneur change).

## Alternatives écartées

- Fyne : rendu non natif, moins « joli », cgo pénible.
- Electron : ~150 Mo pour afficher une page.
- Rester navigateur : rejeté explicitement par Thibault.

## Conséquences

- Nouvelle dépendance `wails/v2` (limitée à cmd/ + un package shell) ; l'engine
  reste stdlib+gorilla et testable sans UI.
- Windows : nécessite le runtime WebView2 (préinstallé sur les Windows récents).
- macOS : build .app + signature ad hoc sur le Mac ; doc Gatekeeper pour les amis.
