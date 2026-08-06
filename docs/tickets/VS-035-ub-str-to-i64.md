---
id: VS-035
titre: UB dans str_to_i64 (négation de 2^63) — révélé par UBSan lors de VS-030
statut: ouvert
priorité: basse
dépend-de: []
créé: 2026-08-06
mis-à-jour: 2026-08-06
---

## Contexte

En rejouant `test_core.c` sous UBSan sur macOS (VS-030), un UB préexistant est
apparu dans `str_to_i64` (`core/src/base_core.c:250` depuis VS-031,
ex-`ui/win32/src/base.c:461`) :
`-(i64)acc` quand `acc == 2^63` (INT64_MIN n'a pas d'opposé représentable).
Repris verbatim lors de la scission pour respecter le « diff de logique nul ».

## Critères d'acceptation

- [ ] Parsing de `INT64_MIN` sans UB (accumuler en négatif, ou borner avant la
      négation) — comportement observable inchangé pour toutes les autres entrées
- [ ] Test dédié (INT64_MIN, INT64_MAX, ±dépassements) dans test_core.c
- [ ] UBSan vert sur macOS ET `build.bat asan` vert sur Windows

## Journal du ticket

- 2026-08-06 : créé (trouvaille UBSan de VS-030).
