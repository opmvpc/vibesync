---
id: VS-032
titre: Core commun phase 3 — l'app macOS bascule son moteur sur VSCore
statut: ouvert
priorité: haute
dépend-de: [VS-031]
créé: 2026-08-06
mis-à-jour: 2026-08-06
---

## Contexte

ADR-010. L'exécutable macOS consomme le moteur C (wrapper Swift fin au-dessus de
l'API polling) ; le moteur Swift natif est retiré une fois le binaire vert.

## Critères d'acceptation

- [ ] AppModel branché sur VSCore (wrapper Swift : conversions Str8/String aux
      frontières, aucun pointeur ne survit à l'appel)
- [ ] Retrait de Engine.swift, Types.swift, Time.swift (~1 040 lignes)
- [ ] VectorsTests.swift rejoue les vecteurs contre le wrapper (le chemin
      réellement utilisé par l'app), 13/13
- [ ] `scripts/run-real-macos.sh` PASS 10/10 contre la prod
- [ ] Binaire < 10 Mo (attendu ~1 Mo)

## Journal du ticket

- 2026-08-06 : créé (ADR-010).
