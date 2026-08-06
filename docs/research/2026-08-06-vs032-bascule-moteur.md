---
titre: VS-032 — l'app macOS bascule son moteur sur VSCore (phase 3 d'ADR-010)
date: 2026-08-06
statut: livré (non committé)
auteur: agent Opus (implémentation), périmètre ui/macos/, Package.swift, ticket VS-032
---

## 1. Ce qui a changé

L'exécutable `VibeSync` dépend désormais de la cible `VSCore` (`Package.swift`,
`dependencies: ["VSCore"]`). La machine à états de synchronisation n'existe plus
qu'en un exemplaire pour les deux clients natifs : `core/src/engine.c`. Côté
Swift il ne reste qu'une frontière, `ui/macos/Sources/VibeSync/Engine/
CoreEngine.swift` (519 l.), qui ne décide de rien.

| Fichier | Avant | Après |
|---|---|---|
| `Engine/Engine.swift` | 791 l. (machine à états) | **supprimé** |
| `Engine/Time.swift` | 92 l. (`Nanos`, `VSTime`, `Sync`) | **supprimé** |
| `Engine/CoreEngine.swift` | — | 519 l. (wrapper + `VSTime` sur le C) |
| `Engine/Types.swift` | 156 l. | 162 l. (en-tête réécrit, code inchangé) |

**883 lignes de logique dupliquée retirées** pour 519 lignes qui ne contiennent
aucune règle : conversions, lectures d'état et traduction des sorties.

## 2. Architecture du wrapper

`CoreEngine` est une **classe** (l'`Engine` était un `struct` copié dans
`AppModel`) qui détient un unique `VsEngine` — ~30 Ko de tampons bornés, aucune
allocation, aucune durée de vie à gérer. Trois règles d'interop, tenues par la
totalité du fichier :

1. **Aucun pointeur C ne survit à l'appel.** Une `String` n'est vue comme `Str8`
   que dans le corps de `withStr8` (tampon Swift vivant pour l'appel, adresse
   non nulle même vide) ; les `StrBuf` de l'état C sont **recopiés** en `String`
   à chaque lecture (`selfId`, `room`, `fileName`, `status.fileName`,
   `roomState.setBy`). Le C copie de son côté (`strbuf_set`), donc rien ne
   pointe jamais chez l'autre.
2. **Tout l'état est dans le `VsEngine`.** Le wrapper n'a qu'un seul champ à
   lui : `resumeToastSec`, le signal de toast rendu par le dernier welcome.
3. **Sérialisation inchangée** : tous les rappels réseau, VLC et médias
   reviennent déjà sur la file principale (`OperationQueue.main` pour
   `URLSession`, `DispatchQueue.main` pour `VLCClient`/`MediaLibrary`), et le
   timer de 200 ms y vit. Aucun verrou ajouté, aucune course nouvelle.

L'API `Decision` / `[Decision]` est conservée : le wrapper traduit `VsOutput`
en `VLCCommand`/`ClientMessage`, donc `AppModel.apply` n'a pas bougé. `VsOutput`
range les deux familles dans deux tableaux distincts : on rend les commandes
VLC puis les messages serveur — l'ordre qui compte (celui *dans* chaque famille)
est préservé, et les deux partent de toute façon sur des canaux différents.

`VSTime` reste, mais délègue au C (`vs_now_ns`, `vs_ns_to_unix_ms`,
`vs_ns_seconds`) : même arithmétique que les vecteurs, à la nanoseconde près au
lieu de la milliseconde. `Sync` disparaît entièrement — ses 20 constantes
étaient une copie de `engine.h` ; la seule encore appelée hors moteur,
`nextBackoff`, est maintenant `CoreEngine.nextBackoff` → `engine_next_backoff`.

## 3. Ce qui est conservé, et pourquoi

`Types.swift` **reste** : malgré son emplacement (`Engine/`), ce n'était pas du
moteur mais le vocabulaire de frontière — `PlayState`/`VLCStatus` produits par
`VLCStatusParser`, `RoomState`/`Pong`/`ClientMessage`/`ControlAction` lus et
écrits par `Protocol.swift`, `VLCCommand` consommé par `VLCClient`, `Decision`
par `AppModel`. Le convertir en types C partout aurait fait entrer `Str8` dans
l'interface pour zéro règle partagée. Seul l'en-tête a été réécrit pour dire ce
qu'il est devenu.

## 4. Écarts de contrat absorbés par le wrapper

- **`connecting(room:)`** = `engine_set_room` + `engine_connecting` (le C sépare
  les deux ; c'est `set_room` qui vide la file de chat et oublie la mémoire de
  séance quand la salle change).
- **`onWelcome`** perd son paramètre `room` : le C ne connaît que la salle posée
  au `connecting`, comme le client Windows. `AppModel` garde sa propre `room`
  affichée, corrigée par le welcome — rien n'est perdu (le serveur ne renormalise
  pas le nom de salle).
- **`state: nil`** (welcome sans état) est passé comme `VsRoomState` nul :
  `engine_sanitize_roomstate` le refuse (rate 0), soit exactement ce que voit le
  client Windows. Test dédié.
- **`openFile`** perd son `now:` (le C n'en a pas besoin : `buf_reset` seul, pas
  de suspension — c'est la version C qui fait foi, vecteurs à l'appui).
- **`userControl`** : `positionSec: Double?` devient le couple
  `(position_sec, use_pos)` du C.

Deux comportements du C commun arrivent **gratuitement** sur macOS :
- **file de chat hors ligne** : un message composé sans session est mis en file
  (20 max) et livré dans l'ordre au welcome suivant. Avant, `AppModel.apply` le
  jetait silencieusement. Un départ volontaire vide la file, comme le protocole
  le demande. (`pendingChats` est exposé pour l'affichage « en attente » du
  client Windows — pas encore branché dans l'UI SwiftUI.)
- **toast « Reprise à … »** de la salle vierge : `out.have_resume_toast` est
  relayé dans `AppModel` au welcome, comme dans `ui/win32/src/main.c`.

## 5. Pièges d'interop rencontrés

- Les énumérations C anonymes typedefées s'importent avec leurs constantes
  utilisables en expression (`e.phase == VS_PHASE_CONNECTED`) mais **pas** comme
  motifs qualifiés : les `switch` du wrapper utilisent des motifs d'expression,
  donc un `default:` est obligatoire même quand tous les cas sont couverts.
- Les tableaux de taille fixe de `VsOutput` s'importent en **tuples** : ils se
  relisent via `withUnsafeBytes(of:&out.cmds)` + `bindMemory`, jamais par
  indexation.
- `VsOutput` fait ~30 Ko (28 `VsMsg`, deux `StrBuf` chacun). Il est déclaré en
  **local** à chaque appel plutôt que détenu par la classe : la remise à zéro est
  négligeable (5 appels/s) et un `VsOutput` partagé serait clobbé par le premier
  appel imbriqué.
- `b32` est un `Int32` : tous les booléens traversent en `!= 0` / `? 1 : 0`.

## 6. Résultats (macOS 26, Swift 6.3.3 / clang 2100)

```
swift test                     36 tests, 0 échec (34 avant : 2 tests de frontière ajoutés)
  VectorsTests                 13 vecteurs rejoués PAR LE WRAPPER, 146 pas
  VSCoreVectorsTests           13 vecteurs rejoués par l'API C brute, 146 pas
swift build -c release         VibeSync 1 118 992 o  (1 114 248 o avant : +4,6 Ko)
scripts/build-macos.sh         VibeSync.app 1,1 Mo   (budget ADR-007 : 10 Mo)
run-real-macos.sh (prod wss)   PASS 10/10, 0 échec
```

Séance réelle : deux clients, deux VLC, salle `vibesync-test-1240` sur
`wss://vibesync.choboai.com/ws` — connexion, fichier déclaré, play, 5 s de
maintien (écart 0,251 s), pause, seek 120 s (écart 0,000 s), drift final 0,312 s
(< 0,5 s), fermeture propre sans VLC orphelin.

**Contre-épreuve** : en perturbant la seule valeur de `seek` dans la traduction
`VsCmd → VLCCommand`, `VectorsTests` remonte 91 échecs — le rejeu par le wrapper
compare bien, et il compare bien *le wrapper*.

## 7. Risques résiduels / suites

- `pendingChats` n'a pas de rendu SwiftUI (le client Windows affiche « en
  attente ») : à traiter avec l'UI, pas ici.
- `VIBESYNC_VERSION_RAW` n'est toujours pas injecté côté SwiftPM (`VS_VERSION`
  vaut « dev » dans le C lié à macOS). Sans conséquence : la version affichée
  par le client mac vient de l'`Info.plist`, et aucun chemin C consommé ici ne
  lit `VS_VERSION`.
- Go n'est pas installé sur ce Mac : `go test ./...` n'a pas été rejoué. Aucun
  fichier Go, aucun fichier de `core/` ni de `ui/win32/` n'a été touché — la CI
  Windows n'a rien de nouveau à confirmer pour ce lot.
- `docs/build-macos.md` et `docs/STATUS.md` restent à mettre à jour (hors
  périmètre d'écriture de cette tâche).
- Phase 4/5 (VS-033/034) : reste natif Swift ce qui doit l'être (transport
  WebSocket, lancement de VLC, trousseau, préférences) — mais `conn.c` et la
  politique de reconnexion sont encore ré-implémentés dans `AppModel`
  (`backoff`/`nextAttempt`), c'est le prochain morceau à faire descendre.
