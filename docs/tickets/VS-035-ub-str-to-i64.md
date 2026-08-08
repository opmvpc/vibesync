---
id: VS-035
titre: UB dans str_to_i64 (négation de 2^63) — révélé par UBSan lors de VS-030
statut: terminé
priorité: basse
dépend-de: []
créé: 2026-08-06
mis-à-jour: 2026-08-08
---

## Contexte

En rejouant `test_core.c` sous UBSan sur macOS (VS-030), un UB préexistant est
apparu dans `str_to_i64` (`core/src/base_core.c:250` depuis VS-031,
ex-`ui/win32/src/base.c:461`) :
`-(i64)acc` quand `acc == 2^63` (INT64_MIN n'a pas d'opposé représentable).
Repris verbatim lors de la scission pour respecter le « diff de logique nul ».

## Critères d'acceptation

- [x] Parsing de `INT64_MIN` sans UB (accumuler en négatif, ou borner avant la
      négation) — comportement observable inchangé pour toutes les autres entrées
- [x] Test dédié (INT64_MIN, INT64_MAX, ±dépassements) dans test_core.c
- [x] UBSan vert sur macOS (sans la suppression, qui a été retirée)
- [ ] `build.bat asan` vert sur Windows — non rejoué faute de VM dans la séance
      du 2026-08-08 ; à passer au prochain aller Windows (C11 portable, pas de
      code plateforme touché)

## Contrat retenu (documenté dans base.h)

`str_to_i64` lit un entier décimal complet, espaces de bord tolérés, signe
`+`/`-` facultatif. Renvoie 1 et écrit `*out` en cas de succès ; renvoie 0
**sans toucher `*out`** si la chaîne est vide, contient autre chose que des
chiffres, ou sort de `[INT64_MIN, INT64_MAX]`. Un dépassement est un refus,
jamais une saturation ni un enroulement. C'est le comportement qu'avait déjà le
code ; il n'était écrit nulle part, il l'est maintenant dans `core/include/base.h`.

## Journal du ticket

- 2026-08-06 : créé (trouvaille UBSan de VS-030).
- 2026-08-08 : corrigé. L'accumulation reste en `u64` (aucun changement de
  logique de parsing, donc aucun risque de régression sur les autres entrées) ;
  seule la conversion finale change : `acc == 2^63` n'est atteignable que pour
  « -9223372036854775808 » — le cas positif est rejeté juste avant — et cette
  valeur est désormais écrite directement (`INT64_MIN`) au lieu d'être obtenue
  par `-(i64)acc`. Suppression `signed-integer-overflow:base_core.c` RETIRÉE de
  `core/tests/ubsan.supp`, qui ne contient plus que son avertissement.
  16 vérifications ajoutées dans `test_core.c` (bornes exactes, `+`/`-`, zéros
  de tête, espaces, `-0`, aller-retour avec `i64_to_str`, dépassements des deux
  côtés et 2^64, signe seul — avec sentinelle vérifiant que `*out` reste intact
  sur échec). Contrôle négatif : recompilé le code d'AVANT avec la suppression
  vidée → UBSan crie bien « negation of -9223372036854775808 cannot be
  represented », donc le test mord.
  Validation macOS : `scripts/test-core-macos.sh` (asan+ubsan) 959
  vérifications / 0 échec, 14/14 vecteurs ; `--fast` (-O2 sans sanitizer) idem ;
  `swift test` 41/41. Windows non rejoué (pas de VM dans cette séance) : le
  correctif est du C11 portable sans API système, et `build.bat asan` n'active
  de toute façon pas `-fsanitize=undefined`.
