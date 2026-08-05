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
| VS-001 | Bureaucratie + recherche + spec protocole | en-cours |
| VS-002 | Package `internal/protocol` | ouvert |
| VS-003 | Serveur (salles, WS, état autoritatif) | ouvert |
| VS-004 | Client : driver VLC + moteur de sync | ouvert |
| VS-005 | Client : web UI locale (GUI) | ouvert |
| VS-006 | Intégration + tests e2e simulés | ouvert |
| VS-007 | Docker + Coolify + CI (lint, tests, releases win/mac) | ouvert |
| VS-008 | Doc utilisateur (README pour les amis) | ouvert |

## Recherches

- `research/2026-08-05-syncplay-architecture.md` — archi/protocole/Docker Syncplay
- `research/2026-08-05-alternatives-et-sync.md` — contrôle VLC, algo anti-drift, alternatives

## Prochaine action

1. Finir VS-002 (protocole Go) — orchestrateur
2. Lancer les agents Opus sur VS-003 et VS-004/005 en parallèle
3. Review croisée, intégration, VS-006
