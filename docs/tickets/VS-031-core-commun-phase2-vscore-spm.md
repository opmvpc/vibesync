---
id: VS-031
titre: Core commun phase 2 — cible C SPM VSCore + rejeu des vecteurs via l'API C
statut: ouvert
priorité: haute
dépend-de: [VS-030]
créé: 2026-08-06
mis-à-jour: 2026-08-06
---

## Contexte

ADR-010. Compiler les fichiers portables de VS-030 (`engine.c`, `protocol.c`,
`json.c`, `conn.c` + portions scindées) en cible C SwiftPM consommée d'abord par
les TESTS seulement — double couverture, aucun retrait.

## Critères d'acceptation

- [ ] Cible `VSCore` (sources C partagées référencées sans duplication de code —
      chemin commun hors ui/win32, ex. `core/`, consommé par les deux builds).
      Contrainte SwiftPM : une cible ne référence pas de sources hors racine du
      paquet → soit déplacer Package.swift à la racine du repo, soit racine du
      paquet remontée — décision à documenter dans le ticket, pas de symlink
      (fragile sur checkout Windows)
- [ ] `module.modulemap` + en-têtes propres ; impl. plateforme macOS des
      primitives (arène mmap, aléa, horloge, dir_iter POSIX)
- [ ] Nouveau test XCTest rejouant les 13 vecteurs via l'API C, EN PLUS de
      VectorsTests.swift (les deux verts)
- [ ] `swift test` et `build.bat test` verts des deux côtés ; budget inchangé

## Journal du ticket

- 2026-08-06 : créé (ADR-010).
