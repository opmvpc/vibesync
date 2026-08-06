---
id: VS-031
titre: Core commun phase 2 — cible C SPM VSCore + rejeu des vecteurs via l'API C
statut: en revue
priorité: haute
dépend-de: [VS-030]
créé: 2026-08-06
mis-à-jour: 2026-08-06
---

## Contexte

ADR-010. Compiler les fichiers portables de VS-030 (`engine.c`, `protocol.c`,
`json.c`, `conn.c` + portions scindées) en cible C SwiftPM consommée d'abord par
les TESTS seulement — double couverture, aucun retrait.

## Décision d'emplacement (critère 1)

**Package.swift est monté à la racine du dépôt**, et chaque cible pointe son
répertoire explicitement (`core`, `ui/macos/Sources/VibeSync`,
`ui/macos/Tests/VibeSyncTests`). C'est la seule racine de paquet qui contienne à
la fois la couche C commune et le client macOS, or SwiftPM refuse qu'une cible
référence des sources hors de la racine du paquet et le lien symbolique est
exclu (fragile sur un checkout Windows). `ui/macos/Package.swift` a donc disparu.

Alternatives écartées :

- **core/ sous ui/macos/** : le client Windows irait chercher son cœur dans le
  répertoire du client macOS — asymétrie absurde, et `ui/macos` cesserait de
  vouloir dire « client macOS ».
- **Duplication ou copie au build** : deux vérités, exactement ce qu'ADR-010
  supprime.
- **Lien symbolique core → ui/macos/core** : explicitement écarté par le ticket
  (checkout Windows).

Structure retenue : `core/include` (en-têtes publics + `module.modulemap`),
`core/src` (C portable, compilé par les DEUX builds), `core/posix`
(implémentation macOS des primitives, ignorée de build.bat), `core/tests`
(`test_core.c`, `test_util.h`, `main_posix.c`, `ubsan.supp`).

## Critères d'acceptation

- [x] Cible `VSCore` (sources C partagées référencées sans duplication de code —
      chemin commun hors ui/win32, ex. `core/`, consommé par les deux builds).
      Contrainte SwiftPM : une cible ne référence pas de sources hors racine du
      paquet → soit déplacer Package.swift à la racine du repo, soit racine du
      paquet remontée — décision à documenter dans le ticket, pas de symlink
      (fragile sur checkout Windows)
- [x] `module.modulemap` + en-têtes propres ; impl. plateforme macOS des
      primitives (arène mmap, aléa, horloge, dir_iter POSIX)
- [x] Nouveau test XCTest rejouant les 13 vecteurs via l'API C, EN PLUS de
      VectorsTests.swift (les deux verts)
- [x] `swift test` vert (34 tests, dont les 2 rejeux de vecteurs) ; budget
      inchangé (bundle 1,1 Mo, plafond 10 Mo)
- [ ] `build.bat test` + `build.bat` + `build.bat asan` verts sous Windows —
      **à confirmer par la CI** (aucune toolchain Windows sur la machine de dev)

## Journal du ticket

- 2026-08-06 : créé (ADR-010).
- 2026-08-06 : livré (non committé). Package.swift à la racine du dépôt, cible
  C `VSCore` (core/src + core/posix, `-Werror` et les mêmes avertissements que
  build.bat), `core/posix/base_posix.c` (arènes mmap/mprotect, journal
  `~/Library/Logs/vibesync.log`, `clock_gettime`, `arc4random_buf`) et
  `core/posix/media_posix.c` (opendir/readdir + lstat, repli de casse ASCII et
  Latin-1). `VSCoreVectorsTests.swift` rejoue les 13 vecteurs à travers l'API C
  (146 pas de trace) en plus de `VectorsTests.swift` resté vert.
  `scripts/test-core-macos.sh` compile et exécute la suite C portable sous
  asan+ubsan : **792 vérifications, 0 échec, 13/13 vecteurs**. build.bat pointe
  les nouveaux chemins (`CORE_DIR`, `INC`, `TESTS`). Rapport :
  `docs/research/2026-08-06-vs031-vscore-spm.md`.
  Reste à jour par l'orchestrateur : `docs/build-macos.md` (les commandes y
  partent encore de `cd ui/macos`).
