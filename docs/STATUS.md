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
| VS-003 | Serveur (salles, WS, état autoritatif) | en-cours (agent Opus A) |
| VS-004 | Client : driver VLC + moteur de sync | en-cours (agent Opus B) |
| VS-005 | Client : web UI (GUI) | en-cours (agent Opus B) |
| VS-006 | Intégration + tests e2e simulés | ouvert |
| VS-007 | Docker + Coolify + CI (lint, tests, releases win/mac) | ouvert |
| VS-008 | Doc utilisateur (README pour les amis) | ouvert |
| VS-009 | Coquille native Wails | abandonné (ADR-006) |
| VS-010 | Cœur headless + spec canal /ui | ouvert |
| VS-011 | App Windows WPF net48 | ouvert |
| VS-012 | App macOS SwiftUI | ouvert (Mac dispo 06-08) |

## Recherches

- `research/2026-08-05-syncplay-architecture.md` — archi/protocole/Docker Syncplay
- `research/2026-08-05-alternatives-et-sync.md` — contrôle VLC, algo anti-drift, alternatives

## Prochaine action

1. Attendre les agents Opus (VS-003, VS-004/005), review croisée, intégration (VS-006)
2. VS-009 : coquille Wails Windows + script/doc de build macOS pour le Mac de demain
3. VS-007 (Docker/Coolify/CI) puis VS-008 (doc amis)

Pivots du jour (dans l'ordre) : client graphique natif Win+mac ; QA renforcée
(tests partout + analyse statique) ; **pas de webview** → UIs 100 % natives
SwiftUI/WPF sur cœur Go headless (ADR-006) ; **budget client < 10 Mo** → Windows
en WPF net48 plutôt que WinUI 3 (ADR-007). SDK .NET 8 installé sur la machine.
