# VS-039 — Changement de fichier en cours de salle

*2026-08-09 — analyse, design et validation. Retour terrain de Thibault sur
v0.2.3 (deux clients mac réels).*

## 1. Reproduction (avant toute explication)

Le scénario a d'abord été **ajouté au harnais réel** (`scripts/run-real-macos.sh`,
étape `(h)`) plutôt que raconté : deux vraies instances, deux vrais VLC, un vrai
serveur (image Docker construite depuis le dépôt, `ws://127.0.0.1:8080/ws`).

Déroulé : séance normale jusqu'au seek à 120 s d'un WAV de 600 s, salle en
lecture, puis `open` d'un **second média de 42 s** chez le client 1, puis chez le
client 2, puis `play`.

Deux enseignements dès le premier tour :

- la première exécution n'a fait tomber QUE le bandeau : par chance de timing, le
  client 1 avait émis un `control` qui remettait la salle à 0. **Le bug est une
  course** — c'est pour ça qu'il ne se voit pas à tous les coups, et pourquoi
  Thibault a pu ne le rencontrer qu'une fois sur deux ;
- la seconde exécution l'a montré en entier, avec le message du ticket au mot
  près dans le journal serveur :

```
level=INFO msg="pause automatique" raison="Pause auto : alice a 125.6 s de retard"
level=DEBUG msg="control appliqué" user=u1 action=play positionSec=42
level=INFO msg="pause automatique" raison="Pause auto : bob a 45.1 s de retard"
```

```
FAIL  aucun bandeau chez le client 2 (watchShow=false watchFile=vibesync-test.wav)
FAIL  la position de l'ancien média contamine le nouveau (salle1=125.6 salle2=125.6)
FAIL  la lecture ne repart pas (vlc1=stopped vlc2=stopped)
FAIL  pauses automatiques fantômes : client1 0→2, client2 0→2
```

Un point de contrôle du harnais a été durci en chemin : « les deux lecteurs
avancent ensemble » passait au VERT avec deux VLC **arrêtés à la fin du
fichier** (écart 0 s, positions 0). L'état `playing` fait désormais partie de la
condition — la même garde existait déjà côté VM, elle manquait côté mac.

## 2. Cause racine

### Symptôme 1 — aucun bandeau chez les autres

`refresh_watch_banner` (Win32) et `refreshWatchBanner` (macOS) **sortaient dès
que nous avions un fichier ouvert** :

```c
if (app->engine.have_file) { ui->watch_show = 0; return; }
```

La mécanique VS-026 n'existait donc que pour quelqu'un qui n'avait **aucun**
fichier — le cas « j'arrive dans la salle ». Le cas « épisode suivant » n'était
couvert par rien : le `users` qui porte le nouveau nom de fichier arrivait bien,
personne ne le regardait.

### Symptôme 2 — la position de l'ancien média contamine le nouveau

Deux moitiés, l'une serveur et l'autre client, qui produisent le même désastre.

**Serveur** : `handleSetFile` ne faisait que ranger le nom/la durée et comparer
les durées. L'état autoritatif `{paused, positionSec, refServerMs}` est
**média-agnostique** : rien ne le remettait à zéro quand le média changeait. La
salle continuait donc de courir à 121 s, 125 s, 130 s… dans un fichier de 42 s.

**Client** : `engine_open_file` conservait l'état de salle de référence. Au
premier statut du nouveau média, `plan()` calcule
`expected = clamp(position_salle, durée)` — soit **42, la fin du fichier** — et,
voyant la salle en lecture et VLC en pause, applique la règle « départ de
lecture » : `seek 42` puis `resume`. Le lecteur saute à la fin, VLC s'arrête, la
position rapportée plafonne, le retard vis-à-vis de la salle grandit sans fin →
`Pause auto : … de retard`. La salle gèle alors à 125,6 s, position d'où le
`play` suivant repart : `control play positionSec=42` (raboté), c'est-à-dire la
fin du média, et une seconde pause auto. Séance morte, exactement le rapport
terrain.

Le vecteur 15 sans le correctif le dit en une ligne :

```
15-changement-de-fichier @1000ms : 2 commande(s) VLC, attendu 0
15-changement-de-fichier @1200ms : position VLC = 42.000000, attendu 0.000000
```

## 3. Design retenu

**La position de salle appartient à un média.** Trois pièces, une par étage.

### A. Serveur — règle 5bis (`docs/protocol.md` §Comportements serveur)

Un `setFile` dont le nom diffère (insensible à la casse) de celui que **ce même
membre** avait déjà déclaré est un changement de média : la salle repart de
`{paused: true, positionSec: 0, rate: 1, refServerMs: now, setBy: "server"}`,
diffusé à tous, plus un `toast` info « X a changé de fichier : <nom> — la salle
repart du début ».

Deux exclusions, et ce sont elles qui rendent la règle sûre :

- **première déclaration d'un membre** → jamais de reset. Un arrivant qui ouvre
  sa copie au milieu du film ne doit ramener personne au début ;
- **re-déclaration à l'identique** → jamais de reset. Chaque `welcome` en produit
  une (§File d'attente hors ligne) ; sans cette exclusion, deux copies aux noms
  différents entre membres (`film.mkv` vs `film-vf.mkv`, situation tolérée
  aujourd'hui) se chasseraient l'une l'autre à chaque reconnexion et tueraient la
  séance.

Le ready-gate n'est pas rejoué (`started` reste levé) : changer d'épisode ne
redemande pas à toute la salle de se déclarer prête.

### B. Client — invalidation de la référence à l'ouverture

`engine_open_file` (C) / `OpenFile` (Go) oublient l'état de salle, l'historique
de dérive et la dernière position de salle connue. Plus aucune correction
jusqu'au `roomState` suivant — que le serveur diffuse immédiatement.

Deux raisons distinctes de le faire **en plus** du serveur :

1. **la course**. Le statut VLC du nouveau média peut arriver avant l'aller-retour
   serveur ; c'est le seek fatal, et c'est un aléa de timing (cf. §1) ;
2. **la mémoire de séance**. `last_room_pos` servait la reprise « salle vierge » :
   sans nettoyage, un serveur perdu juste après un changement de fichier aurait
   fait proposer la position de l'**ancien** média sur le nouveau.

### C. UI — bandeau de récupération élargi

Le bandeau se déclenche dès qu'un autre membre déclare un fichier dont le nom
diffère du nôtre. Sans fichier ouvert, tout nom déclaré diffère du nôtre (vide) :
le comportement VS-026 d'origine est un cas particulier de la nouvelle règle, il
n'a pas eu besoin d'être conservé à part. Fermeture toujours mémorisée par nom de
fichier.

Au passage, le client Windows gagne un marqueur de refus **explicite**
(`watch_dismissed`) : il déduisait le refus de « bandeau caché sur ce nom », ce
qui confondait un refus avec une simple égalité de fichiers et divergeait du
client mac.

### Alternatives écartées

| Alternative | Pourquoi non |
|---|---|
| Reset seulement quand **tous** ont basculé | Entre les deux bascules, la salle continue de courir dans l'ancien média : celui qui a basculé le premier est jugé « en retard » et se fait mettre en pause auto — le symptôme 2, juste décalé. Et la règle ne se déclencherait jamais quand les copies portent des noms différents. |
| Laisser le **client** remettre la salle à zéro (`control seek 0` + `pause`) | C'est ce qui s'est produit par accident au premier tour de repro : dépendant du timing, non déterministe, et deux clients qui basculent émettent des controls concurrents. Le serveur est le seul à connaître l'identité du média de la salle. |
| Rejouer le ready-gate (`started = false`) et remettre tout le monde « pas prêt » | Bloquerait la lecture tant que tout le monde n'a pas rouvert — défendable (Syncplay le fait), mais c'est un changement d'UX, pas un correctif, et ça ne touche aucun des deux symptômes. À rouvrir en ticket propre si Thibault le souhaite. |
| Identifier le média par sa **durée** plutôt que son nom | Deux épisodes de même durée passeraient pour le même média. |
| Ne pas toucher au client (serveur seul) | Laisse la course ouverte et laisse fuir la position de l'ancien média dans la reprise « salle vierge ». |

## 4. Diff des vecteurs

**Les 14 vecteurs gelés sont inchangés au bit près** (`git status` sur
`test/vectors/` ne montre que l'ajout). C'était attendu : aucun d'eux n'ouvre de
fichier après un `welcome`.

Nouveau **`15-changement-de-fichier.json`** — et avec lui un nouveau type
d'événement de rejeu, `openFile`, implémenté dans les trois harnais (Go, C,
Swift ×2). Sa sémantique : *le lecteur a chargé un autre fichier et le driver l'a
laissé en pause à 0* (§Chargement de fichier) ; les commandes de préparation
appartiennent au driver, pas au moteur, et ne figurent donc pas dans la trace.

Trace (salle en lecture à 300 s d'un média de 7200 s, ouverture d'un média de
42 s à 800 ms) :

| t | VLC | attendu | drift | commandes | serveur |
|---|---|---|---|---|---|
| 800 | playing 300,8 | 300,8 | 0 | — | — |
| 1000 | paused 0 | **0** | 0 | **aucune** | setFile ×2 (nom seul, puis durée 42) |
| 1200–1600 | paused 0 | 0 | 0 | aucune | report/ping |
| 1800 | *roomState vierge du serveur (setBy=server)* | | | | |
| 2000–2800 | paused 0 | 0 | 0 | aucune | — |
| 2800 | *roomState en lecture à 0* | | | | |
| 3000 | paused 0 | 0,2 | −0,2 | `resume` | — |
| 3200+ | playing | | −0,2 | aucune | — |

Sans le correctif, la même trace donne `seek 42` + `resume` à 1000 ms et un
lecteur bloqué à la fin du fichier : c'est le contrôle négatif exécuté pour de
vrai (bloc VS-039 retiré de `engine.c`, suite relancée, 5 checks unitaires + 20
lignes de vecteur au rouge, puis restauration).

## 5. Validations

| Suite | Résultat |
|---|---|
| C portable, macOS, asan+ubsan (`scripts/test-core-macos.sh`) | **1048 vérifications, 0 échec**, 15/15 vecteurs |
| Swift (`swift test`) | **55/55**, les deux rejeux de vecteurs à 15 |
| Go (mac, via image `golang:1.26-alpine`) | `go build` / `go vet` / `go test ./...` verts |
| Go (VM Win11 ARM64, clone jetable `work-vs039`) | build + vet + `go test ./...` verts, `staticcheck ./...` vert |
| C Win32 (VM, `build.bat test`) | **1647 vérifications, 0 échec**, 15/15 vecteurs |
| Release Windows (VM, `build.bat`) | `vibesync.exe` 269 312 octets (53 % du budget) |
| Séance réelle mac, 2 clients + 2 VLC + serveur Docker | **18/18 PASS, 0 échec** — dont les 6 points du changement de fichier |
| Séance réelle VM, 2 clients C + 2 VLC (tâche planifiée interactive) | **21/21 PASS, 0 échec** — mêmes 6 points du changement de fichier |

Détail des nouveaux points de la séance réelle mac (étape `(h)`), tous au rouge
avant correctif :

```
PASS  le client 1 a basculé sur le nouveau média (durée 42 s)
PASS  le client 2 reçoit la proposition d'ouvrir « vibesync-test2.wav »
PASS  la salle est repartie de zéro, en pause, chez les deux clients
PASS  les deux clients sont sur le nouveau média
PASS  la lecture repart sur le nouveau média
PASS  les deux lecteurs avancent ensemble dans le nouveau média (écart 0.451 s)
PASS  aucune pause automatique déclenchée par le changement de fichier
```

Journal serveur de la séance verte : deux `changement de média` (un par client),
zéro pause automatique en dehors de celle du départ volontaire final.

## 6. Points de review

1. **Le serveur change → déploiement prod.** `internal/server/room.go` porte la
   règle 5bis ; un push déclenche l'auto-deploy Coolify. Compatibilité :
   - vieux client + nouveau serveur : le reset arrive en `roomState`, le client
     s'aligne — corrigé sans rien faire ;
   - nouveau client + vieux serveur : le client n'ira plus jeter son lecteur à la
     fin du fichier (moitié B), mais la salle restera calée sur l'ancienne
     position tant que personne n'agit. Dégradé, jamais pire qu'avant.
2. **Double reset dans le flux normal.** Quand les deux participants basculent,
   chacun déclenche son reset (deux `roomState` + deux toasts). C'est idempotent
   (0/pause dans les deux cas) et assumé : la garde « ne rien faire si la salle
   est déjà vierge » ajoutait un état sans supprimer un problème.
3. **Bandeau sur copies aux noms différents.** Deux membres dont les copies
   s'appellent `film.mkv` et `film-vf.mkv` verront désormais chacun un bandeau
   proposant le fichier de l'autre. Fermable, non ressuscité pour ce nom, et le
   serveur avertissait déjà sur les durées. C'est le prix de la règle simple ;
   si Thibault le trouve bruyant, la piste est de ne proposer que les noms
   **apparus après** notre propre déclaration (état supplémentaire côté UI).
4. **`refresh_watch_banner` (Win32) n'est couvert par aucun test unitaire.** Elle
   est validée par la compilation et la séance réelle VM (le point « le client 2
   reçoit la proposition » y est passé au vert), pas par la suite C — comme le
   reste de `main.c`, qui n'est pas dans le périmètre de `core/`.
5. **Le harnais mac publie trois champs de plus** (`watchShow`, `watchFile`,
   `autoPauseToasts`), le harnais Windows aussi. Ce sont des champs d'état du
   mode auto, inertes hors harnais.
6. **Go absent du Mac** : Go a tourné via l'image `golang:1.26-alpine` avec le
   dépôt monté (`docker run -v $PWD:/src`). C'est plus court que le détour par la
   VM pour régénérer les vecteurs, et strictement la même chaîne d'outils que la
   CI. À noter dans STATUS : le Mac n'est plus bloqué pour Go.
