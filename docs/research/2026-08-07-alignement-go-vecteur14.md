# Alignement Go du correctif de fenêtre de grâce (VS-029) + vecteur 14

Date : 2026-08-07 — portée : implémentation de référence Go, spec, vecteurs de conformité.
Contexte : le correctif a d'abord été fait dans le C commun (`core/src/engine.c`,
fonction `arm()`) et figé par `core/tests/test_core.c::test_user_action_in_vlc`.
Ce document rend compte du portage vers Go, de la spec et du vecteur 14.

## 1. Le fix, en une phrase

La fenêtre de grâce de 500 ms n'est armée que par les commandes qui changent ce que
la détection compare — `pause`, reprise, `seek`. Un changement de `rate` (nudge) ne
l'arme plus.

Pourquoi : la position rendue par VLC oscille de ±0,15 s autour de la référence.
À ce bruit, le nudge s'engage et se relâche à presque chaque poll de 200 ms
(rate 1 → 1,05 → 0,95 → 1). Chaque `rate` réarmant 500 ms de grâce, la fenêtre ne se
refermait **jamais** en lecture : `detectUserActionLocked` n'était plus appelée du
tout, et une pause faite dans VLC était annulée par la correction du poll suivant
(branche « départ de lecture » : seek + resume) sans jamais partir au serveur.
C'est le point 2 du retour terrain de VS-029.

## 2. Go — `internal/client/sync.go`

`armLocked` : `e.graceUntil = now.Add(GraceWindow)` passe **dans** le `if hold`,
aux côtés de `e.holdUntil`. Portage littéral du C (même découpage, même commentaire).
Rien d'autre ne change : `expect` continue d'absorber le nouveau rate juste
au-dessus, donc `expect.predict` reste juste sans grâce.

Aucun autre point d'armement n'est touché : `adoptRoomStateLocked`
(`internal/client/conn.go`) et `userActionLocked` arment toujours la grâce.

Divergence relevée au passage, **non corrigée** (hors périmètre, aucun effet
observé) : `userControlLocked` (Go) et `emit_user_control` (C) n'arment ni l'un ni
l'autre la grâce — le commentaire du bloc 3 de `test_user_action_in_vlc`
(« arme grâce + hold ») est inexact, l'anti-boucle y tient en réalité à la grâce
armée par le `welcome` du `join_room` juste avant. Les deux implémentations sont
alignées, seul le commentaire C ment.

## 3. Go — test miroir

`internal/client/engine_test.go` :

- `(*harness).churn(polls int) int` : place VLC à « position attendue ± 0,15 s » au
  poll suivant (le `SeekTo` retranche ce que VLC lira d'ici là), et rend le nombre de
  changements de rate observés. C'est le régime de churn, reproduit sans réglage
  caché du faux VLC.
- `TestActionUtilisateurDansVLCSousChurnDeNudge`, miroir de
  `test_user_action_in_vlc`, en trois blocs :
  1. 20 polls de churn (assertion : ≥ 5 changements de rate, sinon le test ne
     prouverait rien), puis pause faite dans VLC → `control pause` attendu ;
  2. même régime, seek de +300 s dans VLC → `control seek` portant la position VLC ;
  3. anti-boucle : sous churn, `roomState` de pause distante → le moteur pause et
     seek VLC lui-même, et n'en renvoie **aucun** `control` sur 12 polls.

Vérifié discriminant : avec l'ancien `armLocked`, le bloc 1 échoue
(`controls = []` — la pause n'est jamais propagée).

## 4. Spec — `docs/protocol.md` §Comportements client

Le tiret « Détection d'action utilisateur » précise désormais que la grâce de 500 ms
est armée par l'application d'un `roomState` **et** par `pause`/reprise/`seek`, et
explicitement **pas** par un ajustement de `rate`, avec la justification (le nudge
churne en régime permanent, armer sur `rate` rendrait la détection inopérante en
lecture).

## 5. Vecteur 14

`test/vectors/14-action-utilisateur-sous-churn.json`, produit par le générateur
`internal/client/vectors_test.go::TestVectors` (`go test ./internal/client
-run TestVectors -update`).

Le churn est exprimé **uniquement** avec le vocabulaire que les trois harnais de
rejeu comprennent : des événements `userSeek` de ±0,15 s (bien sous le seuil de 3 s,
donc jamais vus comme une action utilisateur) intercalés entre les polls. Aucun
réglage privé du faux VLC n'est introduit — sans quoi les fakes C et Swift, qui
rejouent `initialVLC` + `events`, ne pourraient pas reproduire la trace.

Nouvel outil du générateur : `(*vecBuilder).churn(polls int)`.

Déroulé (30 pas, 25 événements) : welcome/pong salle en lecture à 1000 s → 10 polls
de churn → `userPause` → `control pause` (@2200 ms, 1001,85 s) → écho serveur
`setBy=u1` → `userPlay` → `control play` (@3400 ms) → écho → 8 polls de churn →
`userSeek` +300 s → `control seek` (@5600 ms, 1303,71 s).
19 commandes `rate` dans la trace : le régime churne bien.

Discriminant lui aussi : avec l'ancien `armLocked`, le scénario ne produit aucun
`control` (`lastControlPosition` échoue).

## 6. Enregistrement dans les harnais

Les trois harnais découvrent les vecteurs par listing de répertoire — aucune liste
en dur. Seuls les **planchers** de comptage ont été relevés, pour que l'absence du
vecteur 14 soit une erreur et non un silence :

- `internal/client/vectors_test.go::TestVectorsGoldenComplets` : 13 → 14
- `core/tests/test_core.c::test_core_vectors` : 12 → 14
- `ui/macos/Tests/VibeSyncTests/{VectorsTests,VSCoreVectorsTests}.swift` : 13 → 14
  (+ commentaire « 14 vecteurs, 176 pas »)

Le moteur Swift n'a pas de logique de grâce propre : `CoreEngine.swift` est un
enrobage du `VsEngine` C (`engine_on_vlc_status` / `engine_on_tick`). Rien à porter.

## 7. Validation

Go (VM Windows, `go1.26.5 windows/arm64`, clone `C:\Users\OPMVPC\vibesync` aligné sur
le même HEAD, fichiers modifiés copiés par scp) :

| commande | résultat |
| --- | --- |
| `go build ./...` | exit 0 |
| `go vet ./...` | exit 0 |
| `go test ./...` | ok (8 paquets, dont `test/e2e` 63 s) |
| `staticcheck ./...` (v0.7.0, installé pour l'occasion) | exit 0, aucun signalement |

Vecteurs 1-13 : régénérés puis comparés à HEAD — **aucune différence de contenu**
(`git diff --ignore-cr-at-eol test/vectors` vide ; les 13 fichiers n'apparaissaient
modifiés que par les fins de ligne CRLF du checkout Windows). Seul le 14 a été
rapatrié sur le Mac (LF).

macOS :

- `bash scripts/test-core-macos.sh` (asan + ubsan) : **943 vérifications, 0 échec**,
  14/14 vecteurs `ok`.
- `cd ui/macos && swift test` : 41 tests, 0 échec — `VectorsTests` (moteur Swift) et
  `VSCoreVectorsTests` (à travers l'API C) rejouent bien les 14.

## 8. Points de review

1. Le `churn` du générateur lit `expectedPositionLocked` du moteur pour viser sa
   cible : le vecteur est donc auto-cohérent par construction, mais la trace reste
   ce qui fait foi (le rejeu ne rejoue que `initialVLC` + `events`).
2. Les cibles de `userSeek` sont arrondies au millième (`round3`) : nombres courts,
   sûrs à reparser par le JSON maison du C, et bien au-delà de la tolérance de
   comparaison (1e-3).
3. Le bloc 3 du test Go (anti-boucle) passe aussi avant le fix — il garde le filet,
   il ne le tend pas. Ce sont les blocs 1 et 2, et le vecteur 14, qui discriminent.
4. Le commentaire trompeur de `test_user_action_in_vlc` (§2) mérite une correction
   cosmétique côté C.
