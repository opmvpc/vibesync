---
id: VS-015
titre: Client macOS handmade — Swift natif, un seul binaire, 0 dépendance tierce
statut: ouvert
priorité: haute
dépend-de: [VS-013]
créé: 2026-08-05
mis-à-jour: 2026-08-05
---

## Contexte

ADR-008. Remplace VS-012 (modèle core+façade abandonné). `ui/macos/` : Swift +
AppKit/SwiftUI, SPM sans aucun package externe, `URLSessionWebSocketTask`,
`Process` pour VLC, `NSOpenPanel`. Code écrit sur le PC, build/test sur le Mac
de Thibault (dispo 2026-08-06).

## Critères d'acceptation

- [ ] Un binaire/.app unique, cible arm64 macOS 13+, < 10 Mo (visé bien moins)
- [ ] Mêmes écrans/fonctions que VS-014, look macOS natif, thème sombre
- [ ] Moteur de sync conforme spec, validé par les vecteurs `test/vectors/*.json` (XCTest)
- [ ] `scripts/build-macos.sh` : swift build release + bundle .app + signature ad hoc
- [ ] `docs/build-macos.md` : prérequis et étapes exactes pour le Mac + note Gatekeeper
- [ ] Testé en réel sur le Mac contre le serveur (local ou Coolify) + vrai VLC

## Journal du ticket

- 2026-08-05 : créé (pivot handmade ADR-008). Build en attente du Mac.
