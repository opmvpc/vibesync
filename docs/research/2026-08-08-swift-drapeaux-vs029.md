# Drapeaux VS-029 du client Swift macOS

Date : 2026-08-08 — portée : `ui/macos/Sources/VibeSync/VLC/VLCLauncher.swift`
(+ `ui/macos/Tests/VibeSyncTests/VLCLaunchArgsTests.swift`, nouveau).
Aucun commit fait par l'agent.

Suite directe de `docs/research/2026-08-08-launch-go-drapeaux.md`, reliquats 1
et 2 : le blindage VS-029 avait touché le client C puis le driver Go, jamais le
client Swift.

## Constat

`VLCLauncher.launch` construisait la ligne en dur, 5 drapeaux :

```
--extraintf=http --http-host=127.0.0.1 --http-port=N --http-password=X
--no-video-title-show
```

Il en manquait 7 sur les 12 de la liste `darwin` du Go. Le commentaire
(« Mêmes drapeaux que le driver Go, MOINS `--no-one-instance` ») était devenu
faux dans l'autre sens : c'était le Swift qui était en retard, et de bien plus
qu'un drapeau.

## Ce qui a été fait

`VLCLauncher.launchArgs(port:password:)` — fonction pure (statique, interne),
sans le média, sur le modèle de `launchArgs(goos, port, password)` côté Go.
`launch` l'appelle puis ajoute `filePath` en dernier (VLC prendrait le reste de
la ligne pour des MRL).

Les 7 drapeaux ajoutés, dans l'ordre du Go :

```
--lua-intf=http --playlist-autostart --start-paused --no-random --no-loop
--no-repeat --no-play-and-exit
```

Le plus important est `--lua-intf=http` : c'est le filet contre un vlcrc où
Syncplay a fait de `luaintf` l'interface PRINCIPALE — le scénario exact du
retour terrain. Sans lui, `--extraintf=http` ne suffit pas : c'est
`syncplay.lua` qui s'exécute et notre port n'est écouté par personne.

Liste finale (12, identique à `launchArgs("darwin", …)` du Go, même ordre) :

```
--extraintf=http --lua-intf=http --http-host=127.0.0.1 --http-port=N
--http-password=X --playlist-autostart --start-paused --no-random --no-loop
--no-repeat --no-play-and-exit --no-video-title-show
```

Le commentaire du fichier a été refait : il porte maintenant le raisonnement
drapeau par drapeau (miroir de celui du Go et de `vlc_build_command`), l'exclusion
motivée de la famille « instance unique » (3 drapeaux refusés par le VLC macOS,
vérifiés un par un sur 3.0.23) et celle de `--intf=<module>`.

Un seul site de lancement VLC dans tout le Swift : vérifié par `grep` sur
`extraintf|http-port`, plus rien en dur ailleurs.

## Gel par test

`VLCLaunchArgsTests`, troisième exemplaire du gel après
`internal/vlc/launch_test.go` et le bloc `vlc_build_command` de
`core/tests/test_core.c` :

- `testLaunchArgsGele` — les 12, dans l'ordre, `XCTAssertEqual` sur le tableau
  entier (égalité stricte et ordonnée) ;
- `testLaunchArgsPasDeFormeInstanceUnique` — ni les formes positives pièges
  (`--one-instance`, `--one-instance-when-started-from-file`,
  `--playlist-enqueue`) ni leurs formes négatives (qui, elles, empêcheraient le
  VLC macOS de démarrer) ;
- `testLaunchArgsPortEtMotDePasse` — port et mot de passe transmis, `http-host`
  jamais autre chose que la loopback ;
- `testLaunchArgsNeContientPasLeMedia` — tout élément de la liste commence par
  `--` : le média est ajouté par `launch`, après les options.

## Validation

- `swift test` : **45 tests, 0 échec** (41 avant + 4 nouveaux).
- `bash scripts/build-macos.sh` : build release OK, bundle 1,1 Mo, signature
  ad hoc OK.
- Séance réelle deux clients contre la prod
  (`VIBESYNC_PASSWORD=… ./scripts/run-real-macos.sh "" wss://vibesync.choboai.com/ws`)
  : **PASS 10/10**, aucun échec. Deux VLC réels lancés avec les 12 drapeaux, les
  deux attachés, fichier déclaré, play/pause/seek synchronisés (seek à 120 s :
  écart 0,000 s ; drift final 0,299 s < 0,5 s), fermeture propre, aucun VLC
  orphelin.

**Aucun drapeau n'a dû être retiré** : les 12 passent en conditions réelles,
comme le laissait attendre le test un-par-un du rapport précédent.

## Confirmation du point `--start-paused`

La séance réelle reconfirme la trouvaille du rapport Go : au point (b), juste
après l'attache, les deux clients rapportent `vlc=playing` malgré
`--start-paused`. Le drapeau est accepté par le VLC macOS mais inopérant
(l'interface `macosx` démarre la lecture de son côté). Sans conséquence :
`prepare` observe l'état et impose la pause à la position 0 — les deux clients
affichent bien `paused=true` côté moteur au même instant. Le bénéfice « rien ne
part en lecture sauvage même si l'attache échoue » reste acquis sur Windows
seulement. C'est écrit dans le commentaire de `launchArgs`, aux trois endroits.

## Points de review

1. `launchArgs` est **interne** (pas `public`), comme `freePort` : le test y
   accède par `@testable import VibeSync`. Si l'API publique du launcher doit
   rester le seul point d'entrée, c'est le bon niveau ; à confirmer.
2. Les trois listes (C 15 / Go 15+12 / Swift 12) sont maintenant gelées
   séparément, chacune par son propre harnais. Rien ne les compare
   automatiquement entre elles : une divergence future ne serait vue qu'à la
   relecture. C'est le même compromis que pour les vecteurs, mais eux au moins
   ont un fichier commun. Un vecteur `test/vectors/launch-args.json` rejoué par
   les trois est possible si on veut fermer ce trou.
3. La famille « instance unique » reste absente sur macOS pour une bonne
   raison (VLC refuse de démarrer), mais rien ne protège d'un futur VLC macOS
   qui l'accepterait — c'est un manque de blindage assumé, pas un bug.
