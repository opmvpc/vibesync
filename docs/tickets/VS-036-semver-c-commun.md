---
id: VS-036
titre: proto_semver_cmp diverge de la référence Go — bannière de mise à jour erronée (les 2 clients)
statut: ouvert
priorité: haute
dépend-de: [VS-031]
créé: 2026-08-06
mis-à-jour: 2026-08-06
---

## Contexte

Découvert en basculant Version.swift sur le C commun (VS-033, arrêt volontaire
du bloc) : `proto_semver_cmp` (`core/src/protocol.c`) est plus laxiste que la
référence Go `NewerVersion` (`internal/client/version.go`) — pas de rognage des
espaces, pas de notion d'illisibilité (« dev », vide, texte → 0.0.0), suffixes
de pré-version ignorés. 9 des 35 cas de `testNewerVersion` divergent, dans les
deux sens. Le client Windows (`main.c`, `on_server_message`) appelle
`proto_semver_cmp` sans précaution : **il a ces 9 écarts aujourd'hui**. Le plus
gênant : un build local illisible (« dev ») vaut 0.0.0 → TOUT serveur numéroté
déclenche la bannière « Nouvelle version disponible ». Liste des 9 cas :
`docs/research/2026-08-06-vs033-bascule-reste.md` §4.

## Critères d'acceptation

- [ ] Le C commun expose l'équivalent exact de `NewerVersion` Go (rognage
      TrimSpace, illisible → jamais de bannière, mêmes règles de comparaison) —
      les 35 cas de la suite Swift passent sur le chemin C
- [ ] `main.c` (Windows) utilise la nouvelle fonction (plus d'appel nu à
      `proto_semver_cmp` pour la bannière)
- [ ] macOS : Version.swift bascule sur le chemin C (solde du bloc VS-033)
- [ ] Cas ajoutés à `core/tests/test_core.c` (vert via scripts/test-core-macos.sh
      ET build.bat test en CI)

## Journal du ticket

- 2026-08-06 : créé (trouvaille VS-033).
