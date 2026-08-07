# VS-029 — validation réelle dans la VM Windows 11 (2026-08-07)

Clôture des critères Windows-only de `docs/tickets/VS-029-attache-vlc-et-controles.md`
dans la VM Win11 ARM64 pilotée par SSH. Trois choses en sont sorties : la repro du
vlcrc « à la Syncplay » (le blindage tient), un mode auto pour le client C (le
trou « aucun harnais ne peut piloter le client Windows » est comblé) et **deux
vrais bugs**, dont celui du retour terrain de Thibault — *pause faite dans VLC,
non propagée* — qui était bien réel et n'était pas là où on le croyait.

Verdict : **critères 1 à 4 tenus, séance à deux clients C PASS 13/13** contre la
prod, avec un vlcrc Syncplay installé et un VLC déjà ouvert.

---

## Méthode

- VM : `ssh -i ~/.ssh/vibesync_vm_ed25519 OPMVPC@192.168.64.2`, Windows 11 FR
  ARM64, VLC 3.x ARM64 dans `C:\Users\OPMVPC\tools\vlc`, llvm-mingw x86_64
  (émulation).
- **Un processus lancé depuis la session sshd n'a pas de bureau : VLC ne démarre
  pas du tout** (aucun journal, aucune fenêtre, le process meurt). Tout ce qui
  touche à VLC passe donc par une tâche planifiée interactive, qui s'exécute
  dans la session console de l'utilisateur connecté :

  ```
  schtasks /create /tn vs_real /tr "C:\Users\OPMVPC\run-real.bat" /sc once /st 00:00 /f
  schtasks /run    /tn vs_real
  ```

  Le `.bat` pose les variables d'environnement et redirige la sortie dans un
  fichier qu'on relit par SSH. (Piège vu au passage : `echo X=%errorlevel%>>f`
  avale le code — le chiffre colle au `>>` et devient un handle de redirection.
  Il faut une espace.)
- Les sources ont été déposées par `scp` dans le clone de la VM ; **la vérité
  reste le dépôt du Mac**, où tous les diffs sont recopiés. Le clone de la VM
  est laissé modifié (pour rejouer) : un `git checkout -- .` y sera nécessaire
  après le commit de l'orchestrateur.

---

## Critère 1 — repro vlcrc « à la Syncplay » : le blindage tient

### Le vlcrc hostile

Posé dans `%APPDATA%\vlc\vlcrc` (et, dans le harnais, aussi dans le profil isolé
de chaque instance) :

```
[core]
extraintf=luaintf
one-instance=1
one-instance-when-started-from-file=1
playlist-enqueue=1
started-from-file=1
random=1
loop=1
repeat=1
play-and-exit=1
video-title-show=1
[lua]
lua-intf=syncplay
lua-config=syncplay={port="4123"}
http-password=motdepassefigeparsyncplay
http-host=127.0.0.1
http-port=4123
```

### Preuve n°1 : ce vlcrc est bien lu (sinon le test ne vaut rien)

VLC lancé avec `--extraintf=http` pour **seul** drapeau — ni hôte, ni port, ni
mot de passe sur la ligne de commande :

```
==> port 4123 : reponse 200 avec le mot de passe DU VLCRC -> vlcrc LU
--- ports en ecoute par ce pid ---
127.0.0.1    4123
```

Le port, l'hôte et le mot de passe ne pouvaient venir que du fichier : la
configuration est honorée, le terrain de jeu est le bon.

### Preuve n°2 : nos 15 drapeaux reprennent la main

VLC lancé avec exactement la ligne de `vlc_build_command` (port 8999, mot de
passe `vibesynctest`) :

```
==> interface HTTP OK (code 200) avec NOTRE mot de passe
    etat=paused position=0 longueur=600
==> mot de passe du vlcrc refuse (ligne de commande prioritaire) : (401) Non autorisé
```

`--start-paused` tient également : l'état observé au démarrage est `paused` à la
position 0, avant même toute intervention du moteur — l'autoplay est dompté
*avant* l'attache, comme voulu.

### Preuve n°3 : en conditions réelles

La séance à deux clients (ci-dessous) tourne **avec ce vlcrc en place et un VLC
déjà ouvert sur le même média** (le piège `one-instance`) :

```
== (b) les deux VLC sont attaches et le fichier declare
  PASS  VLC lance et fichier declare des deux cotes MALGRE le vlcrc facon Syncplay
```

**Aucun correctif n'a été nécessaire sur le lancement de VLC** : le blindage
écrit à l'aveugle en 8b2982d est correct.

### Réserve honnête

Le piège `one-instance` n'a **pas** pu être reproduit franchement dans cette VM :
deux VLC nus lancés coup sur coup avec `one-instance=1` ne se comportent pas
comme attendu (le second survit, c'est le premier qui disparaît). On ne peut donc
pas affirmer « ce drapeau-là, précisément, sauve la mise » ; on affirme seulement
que l'attache réussit dans l'environnement complet. Le seul verdict qui compte
pour l'utilisateur est celui-là, mais si le PC de Thibault redevient disponible,
c'est le point à revérifier chez lui.

---

## Critère 2 — mode auto du client C (le trou est comblé)

Le client Windows n'avait aucun mode pilotable : rien ne pouvait prouver qu'il
détecte une action faite *dans* VLC. Le mode auto du client macOS
(`AutoPilot.swift`) est porté à l'identique.

- `ui/win32/src/auto.{c,h}` — lecture de `VIBESYNC_AUTO_*`, analyse des
  commandes, E/S des deux fichiers du harnais. Mêmes variables, mêmes verbes
  (`play`, `pause`, `seek <s>`, `ready`/`unready`, `chat …`, `open …`, `quit`),
  même sémantique qu'en Swift (URL vide = mode normal, mot de passe non rogné,
  seules les lignes terminées par un saut de ligne sont exécutées, écriture de
  l'état par fichier temporaire + renommage).
- `ui/win32/src/main.c` — câblage : `auto_start` (connexion + ouverture du
  média), `auto_pump` sur `TIMER_AUTO` (250 ms), `auto_write_status` à 1 Hz. Les
  commandes passent par **les mêmes entrées du moteur que les boutons**
  (`engine_user_control`, `engine_set_ready`, `engine_chat`) et `quit` par le
  chemin normal de fermeture (donc close 1000 + VLC arrêté).
- L'état publié reprend les clés de l'état macOS (`phase`, `connected`, `users`,
  `ready`, `fileDeclared`, `vlcState`, `positionSec`, `roomPositionSec`,
  `driftSec`, `paused`, `buffering`, `latencyMs`, `error`, `connection`,
  `lastError`, `media`, `pid`…) — le même vocabulaire des deux côtés.
- **Deux clés en plus, à discuter** : `vlcPort` et `vlcPassword`. Sans elles, un
  tiers ne peut pas jouer le rôle de l'utilisateur DANS VLC, puisque le port et
  le mot de passe de l'interface locale sont tirés au hasard à chaque
  lancement — et c'est exactement ce que le ticket demande de prouver. Elles ne
  sont écrites qu'en mode auto (donc jamais chez un utilisateur), dans le
  fichier d'état du harnais (dossier temporaire), et **jamais** dans
  `vibesync.log`.
- Tests : 27 vérifications ajoutées dans `test_win32.c` (analyse des commandes,
  lecture d'environnement, écriture atomique, pas de `.tmp` laissé derrière).

---

## Critère 3 — séance à deux clients C, dans les deux sens

`scripts/run-real-vm.ps1` : pendant Windows de `run-real-macos.sh`. Deux
instances avec `%APPDATA%` isolé (donc ini, jeton de session et journal
distincts — VS-028), deux vrais VLC, un WAV silencieux de 10 minutes généré par
le script (même recette qu'en Go et sur mac), le vlcrc hostile installé et
restauré en fin de course, un VLC ouvert *avant* la séance, et 13 points de
contrôle dont **trois où le script parle directement à l'interface HTTP de VLC**
(le client n'a pas émis ces requêtes : il ne peut que les constater).

### Résultat final (3e passage, prod `wss://vibesync.choboai.com/ws`)

```
PASS  connectes a wss://vibesync.choboai.com/ws (salle vs029-vm)
PASS  les deux membres se voient dans la salle
PASS  VLC lance et fichier declare des deux cotes MALGRE le vlcrc facon Syncplay
PASS  play depuis l'UI du client 1 : la position avance chez les deux
PASS  synchronisation stable en lecture (ecart 0,149 s)
PASS  pause depuis l'UI du client 2 : les deux sont en pause et y restent
PASS  seek a 120 s depuis l'UI : les deux positions y sont (ecart 0,000 s)
PASS  play fait dans VLC (client 1) : detecte et propage au client 2
PASS  pause faite dans VLC (client 2) : detectee et propagee au client 1
PASS  seek a 300 s fait dans VLC (client 2) : detecte et propage (ecart 0,000 s)
PASS  ecart entre les deux VLC = 0,261 s (< 0.5 s) -- drift salle -0,047/-0,318
PASS  les deux clients se sont arretes d'eux-memes (close 1000 envoyee)
PASS  aucun VLC orphelin lance par les clients

13 point(s) OK, 0 echec(s)
RESULTAT : PASS
```

Les trois passages, dans l'ordre : **5/13** (bug n°1), **11/13** (bug n°2),
**13/13**. Le harnais a donc payé son écriture deux fois.

---

## Bugs trouvés

### Bug n°1 — `%VIBESYNC_VLC%` était effacé au démarrage (client Windows)

`settings_load()` appelait `apply_vlc_path(app, ini_get(&ini, "vlc", ""))`, et
`apply_vlc_path` faisait `SetEnvironmentVariableW(L"VIBESYNC_VLC", NULL)` quand
le réglage était vide. Conséquence : **la variable d'environnement documentée
comme LE moyen d'indiquer un VLC hors des emplacements standards ne marchait
jamais** — elle était effacée avant d'avoir servi. C'est aussi la piste affichée
à l'utilisateur (« installez VLC ou renseignez VIBESYNC_VLC ») ; la suivre ne
donnait rien.

Symptôme dans la VM (VLC installé dans `%USERPROFILE%\tools\vlc`) :

```
vlc: exécutable introuvable (réglage VIBESYNC_VLC et emplacements standards)
```

Correctif : la valeur héritée est mémorisée au démarrage (`App.vlc_env`) et
`apply_vlc_path` y **revient** quand le réglage est vide, au lieu d'effacer.
Priorité inchangée : réglage de l'ini > environnement > emplacements standards.

### Bug n°2 — la détection d'action utilisateur était MORTE en lecture (moteur commun)

C'est le point 2 du retour terrain, et il est réel sur les trois clients (le
moteur est commun ; le Go de référence a exactement le même code).

`detect_user_action` n'est appelée que hors fenêtre de grâce :

```c
b32 in_grace = now < e->grace_until;
if (e->have_status && !in_grace) detect_user_action(e, now, st, out);
```

et `arm()` armait cette grâce de 500 ms **pour n'importe quelle commande, rate
compris**. Or la position rendue par un vrai VLC oscille de ±0,15 s autour de la
référence : le nudge s'engage et se relâche à presque chaque tour. Trace à 5 Hz
d'un client seul en lecture nominale (colonne `rate` = ce que rend VLC) :

```
15:40:22.102 VLC[etat=playing t=36 rate=1,050420165062]
15:40:22.345 VLC[etat=playing t=37 rate=1           ]
15:40:22.583 VLC[etat=playing t=37 rate=1,050420165062]
15:40:25.268 VLC[etat=playing t=40 rate=0,95057034492493]
```

Une commande `rate` à presque chaque poll de 200 ms, une grâce de 500 ms : **la
fenêtre ne se refermait jamais**. Résultat, mesuré en direct — on met VLC en
pause de l'extérieur, VLC obéit, et le client le remet en lecture 250 ms plus
tard sans que rien ne parte au serveur :

```
--- ON MET VLC EN PAUSE (requete directe, hors du client) ---
requete acceptee, etat renvoye par VLC = paused
15:40:25.791 VLC[etat=paused  t=40 ...] client[vlc=playing ...]
15:40:26.046 VLC[etat=playing t=40 ...] client[vlc=playing ...]   <- le client a resume
```

Correctif (`core/src/engine.c`, fin de `arm()`) : la grâce n'est armée que par
les commandes qui changent ce que la détection compare — pause, reprise, seek.
Un changement de rate ne touche ni l'état lecture/pause ni la position, et
l'attendu absorbe déjà le nouveau rate juste au-dessus, donc `expect_predict`
reste juste.

```c
if (hold) {
    e->grace_until = now + VS_GRACE_NS;
    e->hold_until = now + VS_GRACE_NS;
}
```

Validation : **13/13 vecteurs toujours verts** (le comportement gelé n'est pas
touché — le trou n'était couvert par aucun vecteur), et un test de non-régression
portable ajouté dans `core/tests/test_core.c` (`test_user_action_in_vlc`) qui
reproduit le régime de churn puis exige la détection de la pause et du seek, tout
en vérifiant que l'anti-boucle tient toujours (ce que le moteur vient d'ordonner
ne doit jamais revenir comme action utilisateur). **Test vérifié dans les deux
sens** : sans le correctif il échoue en 3 points, avec il passe.

### Bug n°3 (mineur, mode auto) — publication d'état perdue sur course de renommage

`MoveFileExW` échoue si le script a le fichier d'état ouvert au même instant
(Windows refuse de remplacer un fichier ouvert sans `FILE_SHARE_DELETE`). Vu une
fois en séance (`auto: état non écrit dans …`). Corrigé par 5 tentatives
espacées de 20 ms.

---

## Critère 4 — suite et budget

| | avant | après |
|---|---|---|
| `build.bat test` | 1437 vérifications | **1469, 0 échec** |
| vecteurs | 13/13 | **13/13** |
| `build.bat` (release) | 262 656 o | **264 192 o (258 Ko, 52 % du budget)** |

ASan reste impossible sur Windows ARM64 (documenté dans le rapport de
provisionnement) ; la couverture sanitizer du C commun est le job CI
`client-macos`. Comme le correctif du bug n°2 touche le **cœur commun**, le côté
mac a été revalidé depuis le Mac : `scripts/test-core-macos.sh` → **883
vérifications, 0 échec** (asan+ubsan), 13/13 vecteurs ; `swift test` → **41
tests, 0 échec**.

---

## Limitations connues

1. **Le Go de référence n'est pas aligné** sur le correctif du bug n°2
   (`internal/client/sync.go` ligne ~309 a exactement le code d'origine). Le C
   et le Go divergent tant que ce n'est pas fait. Le Swift, lui, tourne sur le C
   commun : il hérite du correctif tel quel — mais la suite mac
   (`scripts/test-core-macos.sh`) et la CI `client-macos` doivent être relancées.
2. Le driver Go `internal/vlc/launch.go` n'a toujours pas les 9 drapeaux VS-029
   (reliquat déjà noté dans le ticket).
3. `vlcPassword` dans l'état auto : choix assumé, à valider par l'orchestrateur
   (voir critère 2).
4. L'interface graphique n'est pas exercée au clic : le mode auto appelle les
   mêmes entrées du moteur que les boutons et la timeline, pas les boutons
   eux-mêmes. Le chemin `ui.c` → `handle_actions` reste couvert par les tests
   d'UI de `test_win32.c` uniquement.
5. Le piège `one-instance` n'a pas été reproduit isolément (cf. critère 1).
6. La séance tourne contre la **prod** : elle crée une salle `vs029-vm` réelle.

## Rejouer

```powershell
$env:VIBESYNC_PASSWORD = "..."
powershell -ExecutionPolicy Bypass -File scripts\run-real-vm.ps1 -Url wss://vibesync.choboai.com/ws
```

Options : `-PlainVlcrc` (ne pas installer le vlcrc hostile), `-NoPreexistingVlc`
(ne pas ouvrir de VLC avant), `-Media <fichier>`, `-Keep`. En SSH, passer par une
tâche planifiée (cf. Méthode).

État de la VM après la campagne : tâches planifiées supprimées, `vlcrc` retiré,
dossiers de travail et scripts de sonde effacés, aucun `vlc.exe`/`vibesync.exe`
résiduel.
