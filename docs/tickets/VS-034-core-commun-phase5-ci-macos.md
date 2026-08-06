---
id: VS-034
titre: Core commun phase 5 — job CI macos-latest (test_core + swift test)
statut: ouvert
priorité: moyenne
dépend-de: [VS-031]
créé: 2026-08-06
mis-à-jour: 2026-08-06
---

## Contexte

ADR-010. Non-régression transversale : mêmes vecteurs, même C, deux toolchains
(clang/llvm-mingw sur Windows, clang/Xcode sur macOS). Le commentaire de
`ci.yml` anticipait déjà ce job.

## Critères d'acceptation

- [ ] Job `client-macos` : compile VSCore + `test_core.c`, rejoue les 13
      vecteurs, puis `swift test` et `scripts/build-macos.sh`
- [ ] Budget taille vérifié pour le .app (< 10 Mo)
- [ ] La CI reste verte sur les deux OS ; temps de job raisonnable (< 5 min)

## Journal du ticket

- 2026-08-06 : créé (ADR-010).
