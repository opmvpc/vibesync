---
id: VS-015
titre: Client macOS handmade — Swift natif, un seul binaire, 0 dépendance tierce
statut: terminé
priorité: haute
dépend-de: [VS-013]
créé: 2026-08-05
mis-à-jour: 2026-08-06
---

## Contexte

ADR-008. Remplace VS-012 (modèle core+façade abandonné). `ui/macos/` : Swift +
AppKit/SwiftUI, SPM sans aucun package externe, `URLSessionWebSocketTask`,
`Process` pour VLC, `NSOpenPanel`. Code écrit sur le PC, build/test sur le Mac
de Thibault (dispo 2026-08-06).

## Critères d'acceptation

- [x] Un binaire/.app unique, cible arm64 macOS 13+, < 10 Mo (visé bien moins)
- [x] Mêmes écrans/fonctions que VS-014, look macOS natif, thème sombre
- [x] Moteur de sync conforme spec, validé par les vecteurs `test/vectors/*.json` (XCTest)
- [x] `scripts/build-macos.sh` : swift build release + bundle .app + signature ad hoc
- [x] `docs/build-macos.md` : prérequis et étapes exactes pour le Mac + note Gatekeeper
- [x] Testé en réel sur le Mac contre le serveur (local ou Coolify) + vrai VLC

## Journal du ticket

- 2026-08-05 : créé (pivot handmade ADR-008). Build en attente du Mac.
- 2026-08-06 : compilé et testé sur le Mac (73499fc). Moteur réaligné sur les
  vecteurs régénérés (keepOutput, reprise salle vierge, suspension buffering
  anti-masquage, re-déclaration setFile) → 13/13 ; ports VS-023/025/026/028 ;
  review croisée terra (1 bloquant + 3 majeurs + 2 mineurs, tous corrigés,
  dont setFile déclaré avant l'attache VLC et un vrai bug de reconnexion
  distante : la boucle testait isOpen et tuait le handshake TLS toutes les
  200 ms). 3 bugs terrain trouvés par le harnais réel (isActive, VLC macOS qui
  refuse --no-one-instance, App Nap). 33 tests XCTest, binaire 1,1 Mo (11 % du
  budget). Test réel : run-real-macos.sh PASS 10/10 contre la prod, écart
  final entre VLC 0,004 s. TERMINÉ. Reliquat cosmétique : toast « Reprise à
  HH:MM:SS » non affiché dans l'UI (moteur OK), captures mac du guide amis.
