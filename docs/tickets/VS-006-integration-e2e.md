---
id: VS-006
titre: Intégration + tests e2e simulés + review croisée
statut: ouvert
priorité: haute
dépend-de: [VS-003, VS-004, VS-005]
créé: 2026-08-05
mis-à-jour: 2026-08-05
---

## Contexte

Assemblage par l'orchestrateur : review croisée du code des agents, test e2e
« 2 clients + serveur + 2 faux VLC » en un seul process Go, puis test réel avec
2 vraies instances VLC sur la machine (VLC présent : `C:\Program Files\VideoLAN\VLC`).

## Critères d'acceptation

- [ ] Review croisée effectuée (codex terra ou juge Opus), remarques traitées
- [ ] Test e2e simulé : play/pause/seek propagés < 500 ms, drift final < 0,5 s, ready-gate, rejoin
- [ ] `go test ./...` + `go vet` + `staticcheck` verts sur tout le repo
- [ ] Test manuel réel : 2 VLC locaux synchronisés (play/pause/seek dans les 2 sens)

## Journal du ticket

- 2026-08-05 : créé.
