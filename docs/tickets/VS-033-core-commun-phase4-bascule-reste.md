---
id: VS-033
titre: Core commun phase 4 — bascule protocole/JSON/status VLC/media/version/conn
statut: livré (partiel — bloc « version » volontairement non basculé)
priorité: moyenne
dépend-de: [VS-032]
créé: 2026-08-06
mis-à-jour: 2026-08-06
---

## Contexte

ADR-010. Même méthode que la phase 3, bloc par bloc : ajout parallèle → bascule
→ retrait. Inclut `conn.c` (normalisation URL + politique de retry), jamais
porté en Swift : la connexion macOS gagne l'auto-préfixage d'hôte nu du C.

## Point dur identifié en phase 2 (à trancher AVANT de basculer MediaLibrary)

`name_eq_ci` POSIX (VS-031) compare point de code par point de code (ASCII +
Latin-1) alors que APFS/HFS+ tient `é` NFC et `e`+`◌́` NFD pour le même nom —
cas le plus probable en réel sur des fichiers accentués. Sens de l'écart :
« macOS trouve moins », jamais de faux positif. Avant de remplacer
MediaLibrary.swift : soit normaliser (NFC) les deux côtés de la comparaison
dans l'impl. POSIX, soit documenter l'écart comme assumé.

### Décision (2026-08-06) : NORMALISER, dans `core/posix/media_posix.c`

Les deux côtés de la comparaison sont composés (NFC) **au vol**, avant le repli
de casse : une lettre ASCII suivie d'une marque combinante devient la
précomposée Latin-1 correspondante (table de 54 paires — les cinq accents, le
tilde, le rond en chef, la cédille — soit exactement la plage que `fold_cp`
sait déjà replier). Aucune table Unicode importée, aucun `CFStringNormalize` :
ADR-008 est tenu et `scripts/test-core-macos.sh` n'a besoin d'aucun framework.

Ce qui n'est pas dans la table n'est pas composé et reste comparé tel quel :
jamais de faux positif. La normalisation est propre à POSIX (c'est le système
de fichiers qui l'impose, et `name_eq_ci` est justement la primitive que
`platform.h` laisse à la plateforme) ; Windows garde `CompareStringOrdinal`,
qui ne normalise pas non plus — NTFS stocke les noms tels quels.

Vérifié en pratique : sur ce Mac (APFS), Foundation écrit les noms de fichiers
sous forme **décomposée** même quand on lui passe une chaîne composée. Sans
cette normalisation, un fichier accentué reçu d'un participant Windows était
donc réellement introuvable — la contre-épreuve (composition désactivée) fait
tomber 9 assertions.

## Critères d'acceptation

- [x] Protocol.swift, JSON.swift, VLCStatusParser.swift, MediaLibrary.swift
      remplacés par VSCore (retraits effectifs)
- [ ] Version.swift : **non basculé**, volontairement — voir ci-dessous
- [x] WebSocketClient/AppModel utilisent conn (normalisation + retry communs)
- [x] À chaque bloc : swift test vert, puis à la fin run-real-macos.sh PASS
      (41 tests, 0 échec ; séance réelle prod wss : PASS 10/10)
- [ ] build.bat test + asan verts côté Windows : **rien à confirmer pour ce
      lot** — aucun fichier de `core/src`, `core/include` ni `ui/win32` n'a été
      touché ; le seul fichier C modifié est `core/posix/media_posix.c`, que le
      client Windows ne compile pas. `scripts/test-core-macos.sh` (asan+ubsan,
      -Werror) est vert : 792 vérifications, 0 échec.

## Version.swift : bloc arrêté, écart à trancher en amont

`proto_semver_cmp` (core/src/protocol.c) est plus SIMPLE que le port Go de
`Version.swift` : pas de rognage des espaces, pas de notion d'illisibilité
(« dev », vide, texte → 0.0.0), suffixes de pré-version ignorés. Sur les 35 cas
de `testNewerVersion`, **9 divergent**, dans les deux sens. L'écart est celui
du **client Windows**, qui appelle `proto_semver_cmp` sans autre précaution
(`main.c`, `on_server_message`) : c'est donc le C commun qu'il faut trancher,
pas macOS. Détail et liste des 9 cas dans
`docs/research/2026-08-06-vs033-bascule-reste.md` §4. Le plus gênant : une
version locale illisible (`dev`, build non versionné) vaut 0.0.0, donc TOUT
serveur numéroté déclenche la bannière « Nouvelle version disponible ».

## Journal du ticket

- 2026-08-06 : créé (ADR-010).
- 2026-08-06 : blocs protocole (protocol.h/json.h), statut VLC (vlc_parse_status),
  médias (media_find_with + VsDirOps posix) et connexion (conn_normalize_url +
  politique de réessai) basculés ; JSON.swift retiré du produit (migré en décor
  de test). Point NFC/NFD tranché et testé. Bloc « version » arrêté sur
  divergence du C commun. 41 tests verts, séance réelle PASS 10/10 sur
  `wss://vibesync.choboai.com/ws`.
