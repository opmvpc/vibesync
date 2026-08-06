---
id: VS-006
titre: Intégration + tests e2e simulés + review croisée
statut: terminé
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

- [x] Review croisée effectuée (terra sur serveur, sol high sur client/ws), remarques traitées
- [x] Test e2e simulé : 7 scénarios + reprise de session, drift < 0,5 s, ready-gate, rejoin
- [x] `go test ./...` + `go vet` + `staticcheck` verts sur tout le repo
- [x] Test réel : 2 vrais VLC synchronisés en Windows Sandbox, 7/7 étapes (drift 0,25 s)
- [x] Vecteurs de test du moteur exportés dans `test/vectors/*.json` (12 golden files,
      rejoués par les ports C et Swift)

## Journal du ticket

- 2026-08-05 : créé.
- 2026-08-05 : e2e 7 scénarios verts ; bug rejoin silencieux trouvé → fix jeton de session.
- 2026-08-06 : 1er run réel sandbox ÉCHEC (course autoplay VLC + nudge trop lent au
  départ) → 2 règles gelées dans la spec, fixes driver/moteur, 2e run réel : 7/7 PASS
  (drift 0,25 s). Harnais sandbox de Thibault adopté comme outil de validation standard.
