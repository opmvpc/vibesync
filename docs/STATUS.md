# STATUS — vibesync

*Dernière mise à jour : 2026-08-05*

## Où on en est

Projet démarré ce jour. Recherche Syncplay terminée (2 rapports dans `docs/research/`),
décisions d'architecture actées (ADR-001 à 004), spec protocole rédigée
(`docs/protocol.md`). Phase de développement : serveur et client délégués à des agents
Opus en parallèle.

## Chantiers ouverts

| Ticket | Titre | Statut |
|---|---|---|
| VS-001 | Bureaucratie + recherche + spec protocole | terminé |
| VS-002 | Package `internal/protocol` | terminé |
| VS-003 | Serveur (salles, WS, état autoritatif) | terminé (review terra) |
| VS-004 | Client réf. : driver VLC + moteur de sync | terminé (review sol, 12 vecteurs) |
| VS-005 | Client réf. : web UI (debug) | terminé |
| VS-006 | Intégration + e2e + test réel double-VLC | **terminé** (7/7 en sandbox, drift 0,25 s) |
| VS-014 | Client Windows C complet (cœur + UI GDI) | **terminé** — 172 Ko, 47 ms, 1 010 checks |
| VS-015 | Client macOS Swift (code complet livré) | en attente du Mac pour build/test |
| VS-007 | Docker + Coolify + CI (lint, tests, releases win/mac) | ouvert |
| VS-008 | Doc utilisateur (README pour les amis) | ouvert |
| VS-009 | Coquille native Wails | abandonné (ADR-006) |
| VS-010 | Cœur headless + canal /ui | abandonné (ADR-008) |
| VS-011 | App Windows WPF net48 | abandonné (ADR-008) |
| VS-012 | App macOS SwiftUI (façade) | abandonné (ADR-008) |
| VS-013 | WebSocket handmade Go stdlib (vire gorilla) | terminé (review sol) — **go.mod : 0 require** |
| VS-014 | Client Windows C pur + Win32 (un exe) | ouvert |
| VS-015 | Client macOS Swift autonome | ouvert (Mac dispo 06-08) |

## Recherches

- `research/2026-08-05-syncplay-architecture.md` — archi/protocole/Docker Syncplay
- `research/2026-08-05-alternatives-et-sync.md` — contrôle VLC, algo anti-drift, alternatives

## Prochaine action

1. **Sur le Mac de Thibault** : `swift test` puis `scripts/build-macos.sh` dans
   ui/macos (suivre docs/build-macos.md ; erreurs de build attendues et localisées,
   voir §6) — dernier livrable manquant.
2. Thibault : déployer le serveur sur Coolify (docs/deploy-coolify.md) et faire une
   vraie soirée test avec un ami.
3. Reliquats : captures macOS dans le guide amis, releases GitHub taguées (la CI
   uploade déjà l'artifact Windows), renommage du module Go thibsix→opmvpc quand
   aucun agent n'écrit.

Pivots du jour (dans l'ordre) : client graphique natif Win+mac ; QA renforcée ;
pas de webview (ADR-006) ; budget < 10 Mo (ADR-007) ; **philosophie handmade
0 dépendance (ADR-008, l'état final)** : clients mono-exe autonomes (C+Win32 /
Swift), serveur Go stdlib pur, moteur de sync porté avec vecteurs de test partagés.
Le client Go devient référence + harnais de test. Installés sur la machine : Go,
staticcheck, SDK .NET 8 (devenu inutile), WinLibs GCC.
