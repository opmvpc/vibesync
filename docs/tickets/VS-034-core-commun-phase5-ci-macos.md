---
id: VS-034
titre: Core commun phase 5 — job CI macos-latest (test_core + swift test)
statut: terminé
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

- [x] Job `client-macos` : compile VSCore + `test_core.c`, rejoue les 13
      vecteurs, puis `swift test` et `scripts/build-macos.sh`
- [x] Budget taille vérifié pour le .app (< 10 Mo — garde-fou 10 240 Ko dans
      le job, mesuré 1 124 Ko)
- [x] La CI reste verte sur les deux OS ; temps de job raisonnable (< 5 min)
      — premier run vert le 2026-08-07 (b9dfa78), les 3 jobs verts ensemble

## Journal du ticket

- 2026-08-06 : créé (ADR-010).
- 2026-08-07 : job écrit (agent Sonnet, YAML récupéré après 2 stalls sur les
  vérifications locales ; commandes revalidées par l'orchestrateur : 878
  checks C, 41/41 Swift, bundle 1 124 Ko). Release inchangée (n'embarque
  toujours que l'exe Windows — .app en release = chantier séparé,
  signature/notarisation à trancher). Premier run CI à confirmer.
- 2026-08-07 : premier run VERT (b9dfa78) — qa-go + client-windows +
  client-macos verts ensemble. TERMINÉ : ADR-010 intégralement livré.
