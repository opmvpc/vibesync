---
titre: VS-033 — le reste du client macOS bascule sur VSCore (phase 4 d'ADR-010)
date: 2026-08-06
statut: livré (non committé) — bloc « version » volontairement non basculé
auteur: agent Opus (implémentation), périmètre ui/macos/, core/posix/, ticket VS-033
---

## 1. Ce qui a basculé

| Bloc | Swift avant | Swift après | Ce qui décide, désormais |
|---|---|---|---|
| 1. Protocole | `Protocol.swift` 257 l. | 274 l. de frontière | `core/src/protocol.c` + `json.c` |
| 2. Statut VLC | `VLCStatusParser.swift` 77 l. | 45 l. | `vlc_parse_status` (`vlc_core.c`) |
| 3. Versions | `Version.swift` 98 l. | **inchangé** (+16 l. de doc) | — voir §4, arrêt volontaire |
| 4. Médias | `MediaLibrary.swift` 103 l. | 99 l. | `media_find_with` + `vs_dir_ops` posix |
| 5. JSON | `Net/JSON.swift` 149 l. | **hors produit** | `json.c` (parseur ET écrivain) |
| 6. Connexion | `parseURL` 16 l. + `backoff`/`nextAttempt` dans AppModel | `ConnPolicy.swift` 101 l. | `core/src/conn.c` |
| — | — | `CoreBridge.swift` 132 l. (nouveau) | les 3 gestes d'interop, écrits une fois |

Le produit passe de 3 958 à 4 015 lignes Swift : **cette phase ne supprime pas
des lignes, elle supprime des DÉCISIONS**. Environ 420 lignes de règles (lecture
stricte du protocole, assainissement du status.json, parcours borné des
dossiers, courbe de réessai, écriture JSON) ont quitté Swift ; il reste des
conversions, deux fichiers de frontière (233 l.) et beaucoup d'en-têtes qui
disent pourquoi. Les 149 l. de `JSON.swift` ont migré dans la cible de tests
(`JSONDecor.swift`) : le décor de rejeu des vecteurs et le faux VLC en ont
encore besoin, l'application non — plus une ligne.

`CoreBridge.swift` tient les trois gestes que tous les blocs partagent, et
`CoreEngine.swift` (VS-032) s'en sert maintenant au lieu de ses copies privées :
`withStr8` (une chaîne Swift vue comme `Str8` le temps d'un appel), `coreString`
(recopie d'un `Str8`/`StrBuf`), et `Scratch` — l'arène de travail, encadrée d'un
`temp_begin`/`temp_end` par appel et protégée par un verrou récursif (le statut
VLC est analysé sur la file d'`URLSession`, pas sur la principale). La recherche
de médias, elle, se crée **sa propre arène** : elle peut durer des secondes sur
un volume réseau et n'a pas à retenir celle des appels courts.

## 2. Le point dur : NFC/NFD (bloc 4)

**Tranché : on normalise, dans `core/posix/media_posix.c`.** Les deux côtés de
`name_eq_ci` sont composés au vol — une lettre ASCII suivie d'une marque
combinante devient la précomposée Latin-1 — puis repliés en casse. Table de 54
paires (les cinq accents, tilde, rond en chef, cédille) : exactement la plage
que `fold_cp` savait déjà replier, aucune table Unicode importée, aucun
`CFStringNormalize`. ADR-008 est tenu et `scripts/test-core-macos.sh` n'a besoin
d'aucun framework supplémentaire. Ce qui n'est pas dans la table n'est pas
composé : jamais de faux positif (`z`+U+0301 ≠ `ź`).

C'est une décision **de plateforme**, pas de la couche commune : `platform.h`
confie `name_eq_ci` au système « avec la sémantique de son système de
fichiers », et c'est APFS/HFS+ qui impose le problème. Windows garde
`CompareStringOrdinal`, qui ne normalise pas davantage — NTFS stocke les noms
tels quels, en pratique composés.

Ce n'était pas théorique : **sur ce Mac (APFS), Foundation écrit les noms sous
forme décomposée même quand on lui passe une chaîne composée**. Un fichier
accentué annoncé par un participant Windows (donc en NFC) était réellement
introuvable chez soi. Tests ajoutés (`PreferencesTests`) :
`testNameComparisonNormalizesAccents` appelle la primitive directement (NFC↔NFD
dans les deux sens, combiné au repli de casse, plus les cas qui doivent RESTER
différents) et `testMediaSearchFindsBothCanonicalForms` crée les fichiers sur le
disque et les cherche sous l'autre forme. Contre-épreuve : composition
désactivée → **9 assertions tombent**.

## 3. Écarts trouvés entre le Swift retiré et le C commun

Chacun est un comportement que **Windows avait déjà** ; la bascule les fait
arriver sur macOS.

1. **Lecture stricte du protocole.** Un type connu dont un champ obligatoire
   manque ou est mal typé est INVALIDE, donc ignoré. Le décodage Swift le
   remplissait de zéros : un `pong` vide devenait `{t:0, serverMs:0}` et
   empoisonnait l'offset d'horloge. Concerne `welcome` (sans `selfId` ou sans
   état de salle recevable), `pong`, `roomState` **en lecture** sans référence,
   `toast` sans texte, `error` sans code, `chatEvent` sans `from`/`text`.
   `AppModel` les ignorait déjà silencieusement (`Proto.decode` → `nil`), donc
   aucun chemin d'interface ne change. Test dédié :
   `testDecodeRejectsIncompleteMessages`.
2. **`users` tolérant mais assaini** : une entrée sans `id` utilisable est
   écartée (Swift la gardait avec `id: ""`), `latencyMs` est borné à
   [0 ; 600 000] et une `positionSec` non finie retombe à 0.
3. **Dossiers cachés explorés.** `FileManager.enumerator` était appelé avec
   `.skipsHiddenFiles` ; `readdir` ne saute rien (sauf `.` et `..`), comme
   `FindFirstFileW`. Un média rangé sous un dossier commençant par un point se
   retrouve maintenant — et le compteur `visited` est enfin comparable entre les
   deux clients.
4. **Chemin du résultat borné à 512 octets** (`StrBuf` de `MediaFind`) : un
   chemin plus long est tronqué au lieu d'être rendu entier. Borne du C commun,
   déjà celle de Windows ; sans effet réel (les dossiers médias de
   l'utilisateur), mais c'est un écart, il est dit.
5. **Hôte nu accepté.** `wss://` est ajouté d'office, `/ws` aussi, `http`/`https`
   sont traduits, un hôte local passe en `ws://` (pas de TLS), fragment et
   userinfo sont retirés, l'hôte est mis en minuscules. macOS répondait
   « Adresse invalide » à `vibesync.exemple.fr` ; c'est le seul écart de ce lot
   qui se voit tout de suite à l'usage. Le message d'erreur affiché est
   maintenant celui du C (français, précis) au lieu d'un texte générique.
6. **Politique de réessai commune.** `conn_should_attempt` / `conn_on_refused`
   remplacent `backoff`/`nextAttempt` : la règle « un refus du serveur ne relance
   JAMAIS de tentative » est désormais tenue par le même code des deux côtés, en
   plus du `wantConnection` de l'interface (ceinture et bretelles assumées).
   Test : `testRetryPolicy`.
7. **Jeton de session** : `Preferences.validSessionToken` appelle
   `proto_session_token_valid` (mêmes bornes, `VS_SESSION_TOKEN_MAX` au lieu
   d'un 128 recopié), et `Proto.sessionToken` tire ses octets de
   `vs_random_bytes` (`arc4random_buf`) au lieu de `SecRandomCopyBytes`.

Aucun écart trouvé sur le **statut VLC** : les 5 cas d'assainissement, l'entrée
hostile et le repli de métadonnées de `VLCStatusTests` passent sans qu'une
virgule ait bougé — le port Swift en était la copie exacte.

## 4. Bloc 3 arrêté : `proto_semver_cmp` diverge, et c'est Windows qui diverge

Consigne du ticket respectée : STOP et documentation. `proto_semver_cmp` est
plus simple que le port Go de `Version.swift` — il ne rogne pas les espaces, n'a
pas de notion d'illisibilité (« dev », vide, texte → 0.0.0) et ignore les
suffixes de pré-version (c'est écrit dans `test_core.c`). Sur les 35 cas de
`testNewerVersion`, **9 divergent** (mesuré, harnais C jetable) :

| remote | local | Swift | C |
|---|---|---|---|
| `9.9.9` | `dev` | pas de bannière | **bannière** |
| `1.0.0` | `` (vide) | pas de bannière | **bannière** |
| `1.2.3.4` | `1.0.0` | pas de bannière | **bannière** |
| `1..3` | `1.0.0` | pas de bannière | **bannière** |
| `99999999999999999999.0.0` | `1.0.0` | pas de bannière | **bannière** |
| `  1.2.4  ` | `1.2.3` | bannière | **rien** |
| `\t1.2.4\r\n` | `1.2.3` | bannière | **rien** |
| `1.2.3` | `1.2.3-rc1` | bannière | **rien** |
| `1.2.3+b` | `1.2.3-rc1` | bannière | **rien** |

`ui/win32/src/main.c` (`on_server_message`) appelle
`proto_semver_cmp(m->server_version, str8_lit(VS_VERSION))` **sans rognage ni
contrôle de lisibilité** : les neuf écarts sont donc ceux du client Windows
d'aujourd'hui. Le plus visible : un build non versionné (`VS_VERSION` = « dev »,
c'est-à-dire toute compilation à la main) vaut 0.0.0, donc **tout** serveur
numéroté y affiche « Nouvelle version disponible ». Rien de dangereux — la
bannière est informative, la compatibilité dure reste la version de PROTOCOLE
refusée au hello — mais basculer macOS dessus aurait consisté à réécrire 9
attentes de test pour un comportement moins bon. À traiter par un ticket sur le
C commun (rognage + « version illisible → pas de comparaison »), qui corrigera
les deux clients d'un coup. `AppVersion.current` (lecture de l'`Info.plist`)
resterait de toute façon côté plateforme.

## 5. Pièges d'interop rencontrés

- `Arena *` est **opaque** : Swift l'importe en `OpaquePointer`, pas en
  `UnsafeMutablePointer<Arena>`.
- Tout ce que rend le C pointe dans l'arène (`VsInMsg`, `MediaFind.path`,
  `jw_result`) : chaque conversion se fait **à l'intérieur** de la portée
  `Scratch.use`, jamais après.
- `const char **err` de `conn_normalize_url` s'importe en
  `UnsafeMutablePointer<UnsafePointer<CChar>?>` : `var err: UnsafePointer<CChar>?`
  puis `&err`.
- Un paramètre `inout` peut être capturé par une closure **non échappante**
  (`withStr8 { jw_kv_str(&w, …) }`) — c'est ce qui rend l'écriture du JSON de
  l'auto-pilote lisible.
- `String` se convertit automatiquement en `const char *` pour les clés de
  `jw_kv_*` : seules les VALEURS ont besoin d'un `Str8`.

## 6. Résultats (macOS 26, Swift 6.3.3 / clang 2100)

```
swift test                     41 tests, 0 échec  (36 avant : +5 tests de frontière)
  VectorsTests                 13 vecteurs rejoués par le wrapper, décodés par proto_decode
  VSCoreVectorsTests           13 vecteurs rejoués par l'API C brute
scripts/test-core-macos.sh     792 vérifications, 0 échec (asan+ubsan, -Werror)
swift build -c release         VibeSync 1 149 112 o (1 118 992 avant : +30 Ko)
scripts/build-macos.sh         VibeSync.app 1,1 Mo  (budget ADR-007 : 10 Mo)
run-real-macos.sh (prod wss)   PASS 10/10, 0 échec
```

Séance réelle : deux clients, deux VLC, salle `vibesync-test-14863` sur
`wss://vibesync.choboai.com/ws` — connexion, fichier déclaré, play, 5 s de
maintien (écart 0,351 s), pause, seek 120 s (écart 0,000 s), drift final 0,198 s
(< 0,5 s), fermeture propre sans VLC orphelin. L'état publié par l'auto-pilote
est désormais écrit par `json.c` : la séance réelle est aussi la preuve que
l'écrivain C traverse la frontière correctement.

**Contre-épreuves** : composition NFC désactivée → 9 assertions tombent ; les
vecteurs passent désormais par `proto_decode` (l'événement du vecteur est
ré-enveloppé et relu comme un vrai message serveur), donc un welcome refusé par
le C ferait échouer le rejeu au lieu de passer inaperçu.

## 7. Risques résiduels / suites

- **`proto_semver_cmp` à trancher** (§4) : ticket à ouvrir sur le C commun, il
  concerne Windows autant que macOS.
- La normalisation NFC ne couvre que le supplément Latin-1, comme le repli de
  casse : grec, cyrillique et Latin étendu ne sont ni composés ni repliés
  (VS-031 §3, écart inchangé et toujours dans le sens « macOS trouve moins »).
  `Ÿ`(U+0178)/`ÿ` en particulier se composent mais ne se replient pas.
- `Version.swift` reste le seul port Swift d'une règle qui existe aussi en C.
- Go n'est pas installé sur ce Mac : `go test ./...` n'a pas été rejoué. Aucun
  fichier Go n'a été touché, ni `core/src`, `core/include`, `ui/win32` — la CI
  Windows n'a rien de nouveau à confirmer pour ce lot (`core/posix` n'est pas
  compilé par `build.bat`).
- `docs/build-macos.md` et `docs/STATUS.md` restent à mettre à jour (hors
  périmètre d'écriture de cette tâche).
- `pendingChats` n'a toujours pas de rendu SwiftUI (héritage VS-032).
