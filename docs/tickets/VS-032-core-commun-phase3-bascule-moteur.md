---
id: VS-032
titre: Core commun phase 3 — l'app macOS bascule son moteur sur VSCore
statut: livré
priorité: haute
dépend-de: [VS-031]
créé: 2026-08-06
mis-à-jour: 2026-08-06
---

## Contexte

ADR-010. L'exécutable macOS consomme le moteur C (wrapper Swift fin au-dessus de
l'API polling) ; le moteur Swift natif est retiré une fois le binaire vert.

## Critères d'acceptation

- [x] AppModel branché sur VSCore (wrapper Swift : conversions Str8/String aux
      frontières, aucun pointeur ne survit à l'appel)
- [x] Retrait de Engine.swift (791 l.) et Time.swift (92 l.). Types.swift est
      CONSERVÉ : ce n'était pas du moteur mais le vocabulaire de frontière
      (VLCStatus, RoomState, Pong, VLCCommand, ClientMessage, Decision) que
      s'échangent le parseur VLC, le protocole et l'interface — justifié dans
      le rapport.
- [x] VectorsTests.swift rejoue les vecteurs contre le wrapper (le chemin
      réellement utilisé par l'app), 13/13 — VSCoreVectorsTests (API C brute)
      reste, 13/13 aussi
- [x] `scripts/run-real-macos.sh` PASS 10/10 contre la prod
- [x] Binaire < 10 Mo (1 118 992 o, bundle 1,1 Mo)

## Journal du ticket

- 2026-08-06 : créé (ADR-010).
- 2026-08-06 : livré. `CoreEngine.swift` (519 l.) remplace le moteur Swift
  natif ; l'exécutable dépend désormais de la cible `VSCore`. 883 lignes de
  logique dupliquée supprimées pour 519 lignes de frontière sans décision.
  `swift test` 36/36 (34 avant : deux tests de frontière ajoutés), contre-épreuve
  faite (wrapper perturbé → 91 échecs). `swift build -c release` et
  `scripts/build-macos.sh` verts, séance réelle à deux clients contre
  wss://vibesync.choboai.com/ws : PASS 10/10. Deux comportements du C commun
  arrivent gratuitement sur macOS : la file de chat hors ligne et le toast
  « Reprise à … » de la salle vierge. Rapport :
  `docs/research/2026-08-06-vs032-bascule-moteur.md`.
