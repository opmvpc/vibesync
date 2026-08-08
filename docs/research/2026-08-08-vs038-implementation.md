# VS-038 — Resync 1× constant + micro-seek : rapport d'implémentation (2026-08-08)

Ticket : `docs/tickets/VS-038-sync-1x-micro-seek.md` (design gelé par Thibault).
Contexte : `docs/research/2026-08-08-recherche-sync-plateformes.md`.

## 1. Spec (écrite en premier, `docs/protocol.md` §Comportements client)

Deux puces réécrites/ajoutées, le reste ajusté par ricochet :

- **§Correction (VS-038)** — la vitesse n'est **jamais** modifiée pour corriger
  la dérive ; le `rate` ne sert plus qu'à **restaurer** la vitesse de référence
  de la salle quand celle de VLC en diverge. En lecture : `|drift| ≤ 1,5 s` →
  rien ; `1,5 s < |drift| < 5 s` → micro-seek vers la position attendue **si la
  dérive persiste** ; `|drift| ≥ 5 s` → seek immédiat. En pause : inchangé
  (seek seul, seuil 0,6 s).
- **§Persistance de la dérive (anti-bruit)** — `|drift|` est échantillonné à
  chaque poll où une correction serait permise **en lecture** ; le moteur garde
  les 5 derniers échantillons (1 s). Le micro-seek exige un historique **plein**
  dont la **médiane** dépasse la zone morte. Historique vidé à chaque seek émis,
  dès que la lecture s'interrompt, et à toute invalidation de la référence. Le
  palier des 5 s ne consulte jamais l'historique.
- Ajustements : §Départ et reprise (l'écart de départ n'est plus « résorbé par
  le nudge », il est sous la zone morte — d'où le calage avant lecture),
  §Hold post-action (« nudge/seek » → « seek »), §Conditions de correction
  (l'hystérésis du nudge disparaît ; l'historique est vidé à l'invalidation et
  aucun échantillon n'est pris pendant un hold), §Détection d'action utilisateur
  (la règle « un `rate` n'arme pas la grâce » est **conservée**, sa justification
  historique VS-029 est reformulée au passé — le churn n'existe plus).

Constantes : `DeadZoneSec/VS_DEAD_ZONE_SEC` 0,1 → **1,5** ;
`HardSeekSec/VS_HARD_SEEK_SEC` 2,0 → **5,0** ; `NudgeFast/NudgeSlow/NudgeExitSec`
et `VS_NUDGE_*` **supprimées** ; `driftSamples/VS_DRIFT_SAMPLES = 5` ajoutée.
`StartAlignSec` (0,3), `PausedSeekSec` (0,6), `GraceWindow`, `UserHold`,
`BufferingSuspend` : inchangées.

## 2. Implémentations

| Couche | Fichiers | Ce qui change |
|---|---|---|
| Go (référence) | `internal/client/{types,engine,sync,conn}.go` | `nudging bool` → `drifts []float64` ; `driftPersistsLocked` + `medianFloat` ; `planLocked` : hard-seek OU persistance → seek, puis `rateAction(base)` **hors** du switch (restauration seule) |
| C (commun) | `core/include/engine.h`, `core/src/engine.c` | `drifts[VS_DRIFT_SAMPLES] + drift_count` ; `drift_persists()` (décalage + tri par insertion, calqué sur la médiane d'offset) ; `VS_CORRECT_NUDGE` retiré de l'enum |
| Swift | `Engine/CoreEngine.swift`, `UI/AppModel.swift` | `Correction.nudge` et `nudging` supprimés ; badge « ajustement de vitesse » → **« recalage »** sur `.seek` |
| Win32 | `ui/win32/src/main.c` | l'UI n'exposait qu'un booléen `correcting` : rien à renommer. Ajout des compteurs de commandes VLC (voir §4) |

Go et C sont volontairement écrits avec le **même court-circuit** :
`abs >= HardSeek || drift_persists(...)` — au-delà de 5 s aucun échantillon
n'est poussé et l'historique est vidé, identique des deux côtés.

## 3. Vecteurs — diff justifié, un par un

Régénérés par le générateur Go (`go test ./internal/client -run TestVectors
-update`) dans un clone jetable de la VM (Go absent du Mac), puis rapatriés.

| Vecteur | Diff | Justification |
|---|---|---|
| `01-zone-morte` | **aucun** | drift ≈ 0, aucun palier concerné |
| `02-nudge-avance` → **`02-micro-seek-avance`** | redessiné | testait le nudge, qui n'existe plus. Nouveau scénario : drift +2,5 s → **rien pendant 4 polls**, UN seek au 5e (historique plein, médiane 2,5 > 1,5), résidu 0 ensuite, **zéro `rate`** |
| `03-seek-dur` | modifié | après le seek immédiat, le résidu de −0,2 s déclenchait un `rate` 1,05 d'affinage ; il est maintenant dans la zone morte → commande supprimée, et la position avance à 1× (300,8 au lieu de 300,81…). Description mise à jour (« ≥ 5 s, sans attendre la persistance ») |
| `04-pause-manuelle` | **aucun** | |
| `05-pause-distante` | **aucun** | |
| `06-rejoin` | modifié | **exactement le même motif que 03** : `rate` 1,05 post-seek supprimé, positions à 1× |
| `07-hold-avec-echo` | **aucun** | |
| `08-hold-sans-echo` | **aucun** | |
| `09-seuil-pause` | **aucun** | branche pause inchangée |
| `10-offset-median` | **aucun** | |
| `11-hysteresis` → **`11-zone-morte-micro-seek`** | redessiné | testait l'hystérésis du nudge. Nouveau scénario : drift 1,2 s (sous la zone morte) → rien sur 6 polls ; un `roomState` porte la dérive à 2,2 s → le seek part au **3e** poll qui suit, l'instant où la médiane des 5 bascule (2 anciens 1,2 encore dans la fenêtre). Vecteur discriminant de la règle de médiane |
| `12-coupure-rejoin` | modifié | **même motif que 03/06** |
| `13-reprise-salle-vierge` | **aucun** | |
| `14-action-utilisateur-sous-churn` → **`14-action-utilisateur-sous-bruit`** | redessiné | le régime de churn n'existe plus ; le bruit de position ±0,15 s est conservé tel quel (helper `noise`, même vocabulaire userSeek+poll). Le vecteur prouve désormais **deux** choses : ce bruit ne produit **aucune** commande, et pause/reprise/seek faits DANS VLC sont toujours détectés et remontés en `control` |

Aucun autre diff : les 8 vecteurs « aucun » sont identiques octet pour octet.
Les planchers de comptage des 3 harnais (Go `< 14`, C `< 14`, Swift ×2 `>= 14`
et `> 100` pas rejoués) restent satisfaits — 14 vecteurs, et le total de pas
augmente (11 passe de 8 à 14 pas, 14 de 26 à 30).

## 4. Tests ajoutés / mutés

**Go** (`internal/client/engine_test.go`) — les 4 tests de nudge deviennent :
`TestMicroSeekApresPersistance` (rien avant 5 polls, exactement 1 seek, 0 rate),
`TestZoneMorteLargeAucuneCorrection` (1,2 s pendant 15 polls : rien, historique
plein), `TestMicroSeekSiEnRetard`, `TestSeekImmediatSiGrosDrift` (1 seek, puis
plus rien : le résidu est dans la zone morte), plus
**`TestVitesseDeReferenceRestauree`** (l'utilisateur passe VLC en 1,5× → le
moteur le ramène à 1×, la restauration n'a pas disparu avec le nudge).
`TestJamaisDeNudgeEnPause` → `TestJamaisDeChangementDeVitesseEnPause`.
`TestActionUtilisateurDansVLCSousChurnDeNudge` →
**`TestActionUtilisateurDansVLCSousBruitDePosition`** : le helper `churn` devient
`noisyPlayback`, et l'assertion « ≥ 5 changements de rate » devient
« **0 rate, 0 seek** » — la détection de la pause, du seek et l'anti-boucle
restent prouvées sous le même bruit. `start_test.go` : commentaires et seuil de
convergence réalignés (`TestDepartConvergeParLeCalage`).

**Outillage de preuve** : le faux VLC (`internal/vlc/vlctest/fake.go`) compte
désormais les commandes `rate` reçues (`Rates()`) et sait changer la vitesse
« comme l'utilisateur » (`SetRate`). Sans ce compteur, « zéro commande rate »
n'était pas prouvable — seule la valeur finale du rate l'était.

**C** (`core/tests/test_core.c`) — nouvelle section
**« correction 1× + micro-seek »** (miroir des tests Go) : zone morte 1,2 s,
persistance 2,5 s (rien à 4 polls, 1 seek au 5e, historique vidé), **pic isolé
de 4 s absorbé par la médiane**, seek immédiat à 8 s, restauration de la vitesse
de référence. `test_user_action_in_vlc` : `rate_cmds >= 5` → `rate_cmds == 0 &&
seek_cmds == 0`, le reste (détection pause/seek, anti-boucle) inchangé.

**e2e Go** — deux sous-tests exigeaient une convergence que le moteur ne cherche
plus (`01-nominal` : `|drift| < 0,2 s` ; `09-seeks-utilisateur` : `< 0,3 s`).
Introduction de `playDriftMax = client.DeadZoneSec` pour les assertions **en
lecture** ; `driftMax = 0,5` reste pour la pause (seuil de recalage 0,6 s) et les
tolérances de position. Le moteur n'a pas été affaibli, la borne de test a été
mise en accord avec la spec.

## 5. Validations

| Validation | Résultat |
|---|---|
| `bash scripts/test-core-macos.sh` (asan+ubsan) | **995 vérifications, 0 échec**, 14/14 vecteurs (943 avant) |
| `swift build` + `swift test` | **55/55** |
| VM : `go build ./...`, `go vet ./...`, `staticcheck ./...` | verts (rien à signaler) |
| VM : `go test ./...` (`-count=1`) | tous les paquets `ok`, e2e compris (47 s) |
| VM : `ui\win32\build.bat test` | **1594 vérifications, 0 échec**, 14/14 vecteurs |
| VM : `ui\win32\build.bat` (release) | vibesync.exe 268 800 octets |
| Séance réelle macOS (prod `wss://vibesync.choboai.com/ws`) | **PASS 11/11**, écart final 0,252 s, **rateCmds 0/0** |
| Séance réelle VM, 2 clients C (prod) | **PASS 14/14**, écart final 0,004 s, **rateCmds 0/0**, vlcrc façon Syncplay + VLC préexistant compris |

Le clone jetable `C:\Users\OPMVPC\work-vs038` et la tâche planifiée `vs_real038`
ont été supprimés. (Note : `git fetch` a été lancé dans le dépôt de travail de la
VM, qui était resté sur `769aca1` — ses refs `origin/*` sont à jour, son arbre de
travail n'a pas été touché.)

## 6. Points de review

1. **Choix de conception à valider** : le micro-seek exige un historique **plein**
   (5 échantillons). Conséquence visible dans le vecteur 11 — quand la référence
   bouge alors que l'historique contient encore d'anciennes valeurs basses, le
   recalage part au **3e** poll (majorité), pas au 1er. C'est le prix de
   l'anti-bruit ; l'alternative (vider l'historique à chaque `roomState` adopté)
   a été écartée : elle rendrait la persistance dépendante du trafic serveur.
2. **Écart toléré entre deux lecteurs** : porté de 0,5 à 1,5 s dans les deux
   harnais de séance réelle et dans les e2e en lecture. C'est la conséquence
   assumée du design (rien ne rapproche deux lecteurs dans la zone morte) — mais
   c'est un affaiblissement réel de la garde. En pratique les deux séances ont
   mesuré 0,252 s et 0,004 s.
3. **Nouvelles surfaces d'observation** : `rateCmds`/`seekCmds`/`pauseCmds`/
   `resumeCmds` dans l'état JSON des deux modes auto, et `Fake.Rates()`/
   `Fake.SetRate()` dans le faux VLC Go. Le compteur C est incrémenté dans
   `dispatch_output` (avant `worker_push_cmds`), donc il compte les commandes
   **décidées**, pas les réponses HTTP de VLC — suffisant pour le critère
   « le moteur n'ordonne jamais de rate », à ne pas confondre avec une preuve
   côté lecteur.
4. **Badge UI** : côté Swift, `.seek` s'affiche « recalage » (auparavant
   « resynchronisation » pour le seek et « ajustement de vitesse » pour le
   nudge). Un micro-seek dure un poll : le badge clignotera brièvement. Si c'est
   jugé bruyant, le supprimer est un one-liner.
5. **Non traité (hors périmètre du ticket)** : le ralenti doux (option A) reste
   la porte de sortie si les micro-seeks s'avéraient trop fréquents en usage
   réel ; aucune télémétrie ne compte aujourd'hui les micro-seeks d'une séance
   (les compteurs `seekCmds` du mode auto agrègent seek de départ, recalage de
   pause et micro-seeks).
