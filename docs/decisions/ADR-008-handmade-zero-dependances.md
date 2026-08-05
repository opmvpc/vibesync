---
id: ADR-008
titre: Philosophie handmade — 0 dépendance, clients mono-exe autonomes
statut: accepté
date: 2026-08-05
---

## Contexte

Thibault fixe la barre façon Casey Muratori / Ryan Fleury : zéro dépendance, code
minimal qui exploite la machine, « juste un exe, boom on clic ça se lance ». Le
pattern « core Go headless + façade native » (ADR-006) implique deux process et un
canal local — de la plomberie contraire à cette philosophie. WPF impose .NET,
gorilla/websocket est une lib tierce.

## Décision

- **Clients autonomes, un seul binaire, uniquement les APIs de l'OS** :
  - **Windows (`ui/win32/`)** : C pur + Win32. Fenêtre + UI immediate-mode dessinée
    main (GDI double-buffer), WebSocket client via **WinHTTP** (TLS géré par l'OS,
    Windows 8+), HTTP VLC via Winsock, JSON lu/écrit main, arènes mémoire, dialogue
    fichier natif (IFileOpenDialog). Objectifs : exe < 500 Ko, démarrage < 100 ms,
    CPU idle ~0 %.
  - **macOS (`ui/macos/`)** : Swift + AppKit/SwiftUI, `URLSessionWebSocketTask`,
    `Process` pour lancer VLC, `NSOpenPanel`. Zéro dépendance tierce (SPM sans
    packages externes). Build sur le Mac (dispo 2026-08-06).
- **Le moteur de sync est porté en C et en Swift** depuis `docs/protocol.md`
  (§Comportements client). Pour garantir la convergence des implémentations :
  **vecteurs de test partagés** dans `test/vectors/*.json` (scénarios entrée →
  décisions attendues), générés/validés par l'implémentation Go de référence et
  rejoués par les tests C et Swift.
- **Serveur 0 dépendance** : gorilla/websocket est retiré ; handshake + framing
  RFC 6455 implémentés main dans `internal/ws` (Go stdlib uniquement).
- **Le client Go (internal/client, internal/vlc, webui) n'est plus un livrable** :
  il devient l'implémentation de référence et le harnais de test e2e du serveur.
- Remplace ADR-006 (pattern core+façade) et le volet Windows d'ADR-007 (WPF).
  Le budget ADR-007 (< 10 Mo) reste, largement surpassé (visé : < 1 Mo).

## Alternatives écartées

- WPF/.NET : runtime managé, démarrage lent, à l'opposé de la demande.
- Garder le core headless : deux process, IPC, complexité de cycle de vie.
- Libs WebSocket C tierces : contraire au 0 dépendance ; WinHTTP suffit.

## Conséquences

- Deux ports du moteur de sync à maintenir (C, Swift) — accepté : petit (~500
  lignes), gelé par la spec et les vecteurs de test.
- Toolchain C requise : WinLibs GCC (UCRT) installé le 05-08 sur la machine.
- La qualité UI (« joli ») se fait à la main : thème sombre, rendu GDI propre,
  DPI-aware. Pas de framework pour aider.
- Windows 8+ requis pour l'API WebSocket de WinHTTP (Win10/11 : OK).
