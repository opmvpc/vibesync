---
id: VS-012
titre: App macOS native — SwiftUI (Apple Silicon, macOS 13+)
statut: ouvert
priorité: haute
dépend-de: [VS-010]
créé: 2026-08-05
mis-à-jour: 2026-08-05
---

## Contexte

ADR-006. App SwiftUI dans `ui/macos/`, même façade que VS-011. Le code peut être
écrit sur le PC Windows, mais build/test uniquement sur le Mac de Thibault
(disponible à partir du 2026-08-06).

## Critères d'acceptation

- [ ] Projet SPM buildable en CLI (`swift build`) + script de fabrication du bundle
      .app (Info.plist, icône, signature ad hoc) : `scripts/build-macos.sh`
- [ ] Mêmes écrans/fonctions que VS-011, look macOS natif (sidebar, SF Symbols)
- [ ] Spawn du core darwin/arm64 embarqué dans le bundle (Resources), uiPort/uiToken
- [ ] Doc `docs/build-macos.md` : prérequis (Xcode CLT, Go), étapes exactes pour builder
      sur le Mac, contournement Gatekeeper pour les amis
- [ ] Tests unitaires du ViewModel (XCTest) exécutables sur le Mac
- [ ] **Taille : bundle .app (UI + core) < 10 Mo** (ADR-007)

## Journal du ticket

- 2026-08-05 : créé (pivot natif ADR-006). En attente du Mac pour build/test.
