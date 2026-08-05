---
id: ADR-006
titre: UIs 100 % natives (SwiftUI / WinUI 3) sur un cœur Go headless
statut: accepté
date: 2026-08-05
---

## Contexte

Thibault refuse toute webview : « du vrai natif ». macOS → Swift pour l'UI ;
Windows → « la façon moderne », compatible Windows 10 (sa machine) et Windows 11.
Le cœur client Go (driver VLC, moteur de sync, canal local /ui) est déjà découplé
de l'affichage.

## Décision

- **Pattern « core + façades natives »** (à la Syncthing) : le binaire Go devient un
  **cœur headless** (`--headless`) lancé en sous-processus par l'app native, qui le
  pilote via le canal WebSocket local `/ui` (JSON, token aléatoire), spécifié dans
  `docs/ui-protocol.md`. L'algo de sync n'existe qu'en Go, une seule fois.
- **Windows : WinUI 3 (Windows App SDK, C#/.NET)** — le toolkit natif moderne de
  Microsoft, Fluent Design, supporté Windows 10 ≥ 1809 et Windows 11. Distribution
  self-contained non packagée (exe + dossier) pour éviter la friction MSIX.
  Repli documenté si WinUI 3 se révèle trop rugueux en CLI : WPF + thème Fluent
  (WPF-UI), tout aussi natif.
- **macOS : SwiftUI** (cible Apple Silicon, macOS 13+), projet SPM scriptable en CLI,
  bundle .app fabriqué par script — buildé sur le Mac de Thibault (dispo 2026-08-06).
- La web UI de l'agent B est conservée comme **mode debug** et référence
  d'implémentation du canal /ui, pas comme produit.
- ADR-005 est **remplacé par** le présent ADR.

## Alternatives écartées

- Wails/webview (ADR-005) : rejeté explicitement (« pas des webview »).
- Réécrire le moteur de sync en Swift + C# : duplication du cœur algorithmique
  le plus délicat, double maintenance — non.
- Lier le cœur Go en bibliothèque C (cgo/FFI) : complexité énorme vs un WS local.

## Conséquences

- Trois projets UI dans le repo : `ui/windows/` (WinUI 3), `ui/macos/` (SwiftUI),
  web debug embarquée dans le core.
- Prérequis build Windows : .NET SDK + Windows App SDK (à installer sur la machine).
- `docs/ui-protocol.md` devient une spec versionnée au même titre que `protocol.md`.
- L'app native doit gérer le cycle de vie du core (spawn, crash, arrêt propre).
