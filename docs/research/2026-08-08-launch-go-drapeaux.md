# Alignement des drapeaux VLC du client Go de référence (suite VS-029)

Date : 2026-08-08 — portée : `internal/vlc/launch.go` (+ `launch_test.go`).
Aucun commit fait par l'agent.

## Constat de départ

Le blindage VS-029 a forcé 15 drapeaux dans le client C
(`core/src/vlc_core.c`, `vlc_build_command`), gelés par `core/tests/test_core.c`.
Le driver Go n'en avait que 6 :

```
--extraintf=http --http-host=127.0.0.1 --http-port=N --http-password=X
--no-video-title-show --no-one-instance
```

Il manquait donc 9 drapeaux : `--lua-intf=http`,
`--no-one-instance-when-started-from-file`, `--no-playlist-enqueue`,
`--playlist-autostart`, `--start-paused`, `--no-random`, `--no-loop`,
`--no-repeat`, `--no-play-and-exit`.

Le client Swift macOS n'a **pas** non plus reçu les drapeaux VS-029 (voir
« reliquats » plus bas) : sa liste est l'ancienne liste Go moins
`--no-one-instance`.

## Ce qui a été fait

`launchArgs(goos, port, password)` — fonction pure, testable sans lancer VLC,
sur le modèle de `locate(goos, …)` déjà en place dans le paquet. `Launch`
l'appelle avec `runtime.GOOS`, puis ajoute `ExtraArgs` et le média en dernier
(inchangé).

Liste Windows (15, ordre identique au client C) :

```
--extraintf=http --lua-intf=http --http-host=127.0.0.1 --http-port=N
--http-password=X --no-one-instance --no-one-instance-when-started-from-file
--no-playlist-enqueue --playlist-autostart --start-paused --no-random
--no-loop --no-repeat --no-play-and-exit --no-video-title-show
```

Liste macOS / autres OS (12) : la même **moins** la famille « instance
unique » — `--no-one-instance`, `--no-one-instance-when-started-from-file`,
`--no-playlist-enqueue`.

## Pourquoi ces trois-là seulement sont exclus hors Windows

Mesuré, pas déduit. Sur le Mac de Thibault (VLC 3.0.23 Vetinari), chacun des
15 drapeaux a été passé isolément à `VLC -I dummy <drapeau> --version` :

- refusés (`unknown option or missing mandatory argument`, VLC ne démarre
  pas) : `--no-one-instance`, `--no-one-instance-when-started-from-file`,
  `--no-playlist-enqueue` ;
- acceptés : les 12 autres.

C'est cohérent avec `libvlc-module.c`, qui déclare ces trois options dans un
bloc conditionnel (Windows, ou Linux avec D-Bus). Les OS non-Windows sont donc
tous traités comme macOS : on ne parie pas sur un VLC Linux compilé avec D-Bus.

Le commentaire du client Swift ne parlait que de `--no-one-instance` ; les deux
autres auraient cassé le lancement macOS de la même façon.

## Vérification en conditions réelles (macOS)

Lancement réel de VLC avec la liste darwin exacte sur un WAV de 30 s :
l'interface HTTP répond bien sur `127.0.0.1` avec le mot de passe tiré,
`length=30`, la lecture démarre. Aucun drapeau ne casse le démarrage.

**Trouvaille : `--start-paused` est accepté mais inopérant sur le VLC macOS.**
Le média part en lecture (`state=playing`, `time` qui avance) malgré le
drapeau — l'interface `macosx` démarre la lecture de son côté. C'est sans
conséquence fonctionnelle : `Prepare` observe l'état et impose la pause à la
position 0 (docs/protocol.md §Chargement de fichier), il reste l'autorité. Mais
le bénéfice « rien ne part en lecture sauvage même si l'attache échoue » n'est
acquis que sur Windows. Noté dans le commentaire de `launchArgs`.

## Gel par test

`internal/vlc/launch_test.go`, miroir du bloc C :

- `TestLaunchArgsGeleWindows` — les 15 drapeaux, dans l'ordre, égalité stricte ;
- `TestLaunchArgsGeleDarwin` — les 12 drapeaux pour `darwin` et pour `linux` ;
- `TestLaunchArgsPasDeFormePositive` — aucune des formes positives pièges
  (`--one-instance`, `--one-instance-when-started-from-file`,
  `--playlist-enqueue`) ne peut apparaître, quel que soit l'OS ;
- `TestLaunchArgsPortEtMotDePasse` — port et mot de passe transmis, `http-host`
  jamais autre chose que la loopback.

## Validation (VM Win11 par SSH, clone jetable, supprimé après coup)

`go build ./...`, `go vet ./...`, `go test ./...` (tous les paquets, y compris
`test/e2e` 64 s), `staticcheck ./...`, `gofmt -l` sur les deux fichiers
touchés : tout vert, aucun avertissement.

Note d'outillage : `gofmt -l` signale tous les fichiers `.go` d'un clone fait
dans la VM — `core.autocrlf=true` y met des CRLF. Ce n'est pas un défaut du
dépôt ; seuls comptent les fichiers copiés en LF.

## Reliquats (hors périmètre de cette passe)

1. **Le client Swift macOS n'a jamais reçu les drapeaux VS-029.** Il lui manque
   `--lua-intf=http`, `--playlist-autostart`, `--start-paused`, `--no-random`,
   `--no-loop`, `--no-repeat`, `--no-play-and-exit` — tous vérifiés acceptés par
   le VLC macOS ci-dessus. `--lua-intf=http` est le plus important : c'est le
   filet contre un vlcrc où Syncplay a fait de `luaintf` l'interface principale,
   exactement le scénario du retour terrain. Son commentaire
   (« Mêmes drapeaux que le driver Go, MOINS `--no-one-instance` ») est
   désormais faux dans l'autre sens.
2. Rien ne gèle la liste Swift côté tests ; un troisième gel serait le pendant
   naturel de celui du C et de celui-ci.
