---
id: VS-033
titre: Core commun phase 4 — bascule protocole/JSON/status VLC/media/version/conn
statut: ouvert
priorité: moyenne
dépend-de: [VS-032]
créé: 2026-08-06
mis-à-jour: 2026-08-06
---

## Contexte

ADR-010. Même méthode que la phase 3, bloc par bloc : ajout parallèle → bascule
→ retrait. Inclut `conn.c` (normalisation URL + politique de retry), jamais
porté en Swift : la connexion macOS gagne l'auto-préfixage d'hôte nu du C.

## Critères d'acceptation

- [ ] Protocol.swift, JSON.swift, VLCStatusParser.swift, MediaLibrary.swift,
      Version.swift remplacés un à un par VSCore (retraits effectifs)
- [ ] WebSocketClient/AppModel utilisent conn (normalisation + retry communs)
- [ ] À chaque bloc : swift test vert, puis à la fin run-real-macos.sh PASS
- [ ] build.bat test + asan verts côté Windows (aucune régression du partage)

## Journal du ticket

- 2026-08-06 : créé (ADR-010).
