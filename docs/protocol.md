# Protocole vibesync — v1

Source de vérité du protocole client↔serveur. Les types Go correspondants vivent dans
`internal/protocol/`. Toute évolution passe par ce document d'abord.

## Transport

- WebSocket sur `GET /ws`, messages **texte JSON** uniquement.
- Enveloppe : `{"type": "<string>", "data": {...}}`.
- Version du protocole : entier `1`, envoyé dans `hello` ; le serveur refuse
  (message `error` code `version_mismatch` puis fermeture) si différent.
- Timestamps : millisecondes epoch, `int64`. `serverMs` = horloge serveur,
  `clientMs` = horloge client. Positions en **secondes flottantes** (`float64`).
- Le serveur expose aussi `GET /healthz` → `200 ok` (healthcheck Coolify).

## Modèle

- **Salle (room)** : créée à la volée au premier `hello` qui la nomme. Quand elle se
  vide, elle est **conservée avec son état** (position, pause) pendant
  `VIBESYNC_ROOM_LINGER` (défaut 30 min) puis détruite — un retour dans la fenêtre
  reprend la séance là où elle en était (crash, coupure, redémarrage du client). État autoritatif : `{paused bool, positionSec float64, rate float64,
  refServerMs int64, setBy string}`. Position courante d'une salle en lecture =
  `positionSec + (nowServerMs - refServerMs)/1000 × rate`.
- **Utilisateur** : id serveur (`u<n>`), pseudo, ready, fichier déclaré, dernière
  position rapportée, latence estimée.
- Mot de passe serveur global optionnel (env `VIBESYNC_PASSWORD`), transmis dans `hello`.
- Démarrage de lecture bloqué tant que tous les membres ne sont pas `ready`
  (le serveur refuse un `control play` avec un `toast` explicatif). Après le premier
  démarrage de la session de salle, play/pause libres.

## Messages client → serveur

| type | data | notes |
|---|---|---|
| `hello` | `{version, name, room, password?, session?}` | premier message obligatoire ; `session` = jeton opaque aléatoire (≥ 16 octets hex) généré par le client à son premier hello et **persisté dans ses réglages** — un redémarrage du client réutilise le même jeton, la reprise de session (règle serveur 6) couvre donc aussi le cas « je ferme l'app et je la relance » sans attendre le timeout du zombie |
| `ping` | `{t}` | `t` = clientMs ; toutes les 2 s |
| `setReady` | `{ready}` | |
| `setFile` | `{name, durationSec, sizeBytes}` | à l'ouverture d'un fichier dans VLC |
| `control` | `{action: "play"\|"pause"\|"seek", positionSec}` | action volontaire de l'utilisateur détectée localement |
| `report` | `{positionSec, paused, buffering}` | état observé de VLC, toutes les 1 s |
| `chat` | `{text}` | |

## Messages serveur → client

| type | data | notes |
|---|---|---|
| `welcome` | `{selfId, room, state, users[], serverVersion, downloadUrl?}` | réponse au `hello` ; `serverVersion` = version applicative du serveur (semver, fichier `VERSION` du repo), `downloadUrl` = où télécharger les clients (env `VIBESYNC_DOWNLOAD_URL`). Client : si `serverVersion` > sa propre version (comparaison semver numérique), afficher une invitation non bloquante à télécharger — le garde-fou dur reste la version de protocole du `hello` |
| `pong` | `{t, serverMs}` | echo du `t` client ; sert à l'offset d'horloge |
| `roomState` | `{paused, positionSec, rate, refServerMs, setBy}` | broadcast à chaque changement |
| `users` | `{users: [{id, name, ready, file?, positionSec, latencyMs}]}` | broadcast à chaque changement |
| `chatEvent` | `{from, text, serverMs}` | |
| `toast` | `{level: "info"\|"warn"\|"error", text}` | notifications (« X a rejoint », « fichiers différents », « pause auto : Y bufferise ») |
| `error` | `{code, text}` | codes : `version_mismatch`, `bad_password`, `name_taken`, `protocol` |

## Comportements serveur

1. `control` valide → met à jour l'état de salle (ref = now serveur, compensé de la
   latence estimée de l'émetteur) puis broadcast `roomState` à **tous** (y compris
   l'émetteur, qui reconnaît son propre `setBy`).
2. Un `report` avec `buffering=true` ou en retard de > 4 s sur la position de
   référence pendant > 2 s → **pause automatique** de la salle + `toast` warn.
   Garde-fous : jamais de pause auto dans une salle à un seul membre ; au plus une
   pause auto toutes les 5 s par salle ; les reports de l'auteur d'un `control`
   sont ignorés pour la pause auto pendant les 2 s qui suivent ce control (son
   propre seek le fait « bufferiser » mécaniquement).
3. Déconnexion d'un membre en lecture → pause automatique + `toast`.
4. Un nouvel arrivant reçoit `welcome` avec l'état courant → son client se cale dessus.
5. Fichiers : si `durationSec` diffère de > 2 s entre membres → `toast` warn (pas de
   blocage). Une durée ≤ 0 signifie « inconnue » et est exclue de la comparaison.
6. Reprise de session : si un `hello` porte un pseudo déjà présent dans la salle ET
   le même jeton `session` que le détenteur, l'ancienne connexion (zombie après une
   coupure silencieuse) est fermée et remplacée — l'arrivant récupère le pseudo,
   `welcome` normal, pas de toast de départ pour le zombie. Pseudo pris avec un
   jeton absent ou différent → `name_taken` (anti-usurpation au niveau bonne foi :
   pas une authentification).
7. Anti-abus : rate limit par connexion (budget global ~20 msg/s en rafale de 40,
   chat limité à ~5 msg/s), plafonds raisonnables (membres par salle, salles,
   connexions) → dépassement : `error` code `protocol` + fermeture (flood) ou
   `toast` warn (plafonds). Les pseudos/salles rejettent les caractères de
   contrôle ET de format Unicode (zero-width, bidi…).

## Comportements client (moteur de sync)

- **Offset d'horloge** : sur chaque `pong`, `rtt = now - t` ;
  `offset = serverMs + rtt/2 - now`. Garder une médiane glissante des 5 dernières
  mesures. `nowServer() = now + offset`.
- **Position attendue** = `roomState.positionSec + (nowServer() - refServerMs)/1000 × rate`
  (0 si `paused`). Drift = position VLC − position attendue.
- **Correction** : `|drift| ≤ 0,1 s` → rien. `0,1 < |drift| < 2 s` → rate nudge
  (jouer à 1,05× ou 0,95× jusqu'à convergence, puis rate 1). `≥ 2 s` → seek dur vers
  la position attendue puis affinage par nudge. Jamais de nudge en pause : seek
  uniquement, et seulement si `|drift| ≥ 0,6 s` (le seek HTTP de VLC est arrondi à
  la seconde ; sous ce seuil il serait un no-op ou une oscillation).
- **Chargement de fichier** : au lancement de VLC sur un fichier, le driver force
  immédiatement pause + position 0 et ne déclare le fichier chargé (`setFile`)
  qu'une fois l'état « en pause » effectivement observé — VLC démarre la lecture
  automatiquement à l'ouverture, cette course est réelle.
- **Départ et reprise de lecture** : quand un `roomState` fait passer de pause à
  lecture, le client cale d'abord VLC sur la position de référence (seek si
  l'écart est ≥ 0,3 s) puis lance la lecture — on ne compte pas sur le nudge pour
  résorber un écart de départ (5 %/s : 0,5 s coûterait 10 s de convergence).
- **Hold post-action** : après l'envoi d'un `control` issu d'une action utilisateur
  locale, le moteur suspend toute correction (nudge/seek) pendant 2 s. Le hold n'est
  levé que par l'**écho** du serveur (`roomState` avec `setBy` = soi) ou par
  l'expiration des 2 s. Les `roomState` d'autrui reçus pendant le hold ne sont PAS
  appliqués immédiatement (le transport étant ordonné, ils précèdent forcément le
  traitement de notre `control` côté serveur) : le dernier est mémorisé et ne
  s'applique qu'à l'expiration du hold si aucun écho n'est arrivé (control perdu).
- **Conditions de correction** : aucune correction (nudge ou seek) tant que le
  premier `pong` n'a pas fourni une mesure d'offset, ni hors de l'état connecté —
  pendant une reconnexion, toute correction est suspendue et l'état de référence
  est invalidé jusqu'au `welcome` suivant. Hystérésis du nudge : engagé quand
  `|drift| > 0,1 s`, il ne se désengage que quand `|drift| < 0,03 s`.
- **Buffering** : la détection (position figée > 700 ms en lecture) est suspendue
  pendant 2 s après tout seek (commandé ou utilisateur) et après chaque transition
  play/pause — un seek fige mécaniquement la position le temps que VLC cherche.
  Anti-masquage : une nouvelle suspension ne peut pas démarrer moins de 1 s après
  la fin de la précédente (les seeks de correction en boucle ne doivent pas
  empêcher le diagnostic) ; propriété exigée : un VLC durablement figé est
  diagnostiqué bufferisant en ≤ 5 s malgré des corrections répétées.
- **Assainissement** : toute donnée entrante est validée — valeurs non finies
  rejetées ; fraction VLC bornée à [0,1] ; positions bornées à [0, durée] (durée
  connue) ; `rate` serveur hors [0,25, 4] rejeté ; seek utilisateur borné à
  [0, durée]. Une erreur d'écriture vers le serveur ferme la connexion et
  déclenche la reconnexion (pas de perte silencieuse de `control`).
- **Détection d'action utilisateur** : le client garde la dernière commande qu'il a
  lui-même envoyée à VLC (avec tolérance) ; tout changement observé non provoqué par
  lui (pause/play, saut de position > 3 s) = action utilisateur → `control` au serveur.
  Anti-boucle : après application d'un `roomState`, fenêtre de grâce de 500 ms.
- **Ready** : bouton dans l'UI ; un utilisateur qui met pause pendant la lecture ne
  perd pas son ready (le ready ne gate que le premier démarrage).

## Erreurs et robustesse

- Reconnexion WS automatique du client (backoff 1 s → 10 s), re-`hello` avec le même
  pseudo, resync via `welcome`.
- **Départ volontaire** : une déconnexion voulue (bouton Quitter la salle, fermeture
  de l'app) envoie la fermeture WebSocket (close 1000) avant de couper — le serveur
  retire le membre immédiatement, le pseudo est libéré sans délai.
- **File d'attente hors ligne** : seuls les messages `chat` composés pendant une
  déconnexion sont mis en file (bornée à 20, les plus anciens sont abandonnés) et
  envoyés après le re-hello. La file est liée à la salle visée : elle est vidée
  sans envoi si l'utilisateur change de salle ou se déconnecte volontairement —
  elle ne survit qu'aux reconnexions automatiques vers la même salle. `setReady` et `setFile` ne sont pas mis en file :
  l'état courant est re-déclaré après chaque `welcome`. Les `control` ne sont
  JAMAIS rejoués (une action périmée écraserait la salle) ; `ping` et `report`
  jamais mis en file.
- **Reconnexion sans écrasement** : après un `welcome`, le moteur adopte l'état de
  la salle (seek local si nécessaire) et n'émet aucun `control` de rattrapage —
  la détection d'action utilisateur est inhibée pendant cet alignement.
- **Salle vierge (reprise après perte du serveur)** : conditions cumulatives —
  le `welcome` montre une salle sans aucun control (`setBy` vide, position 0),
  ET le client était **précédemment connecté à cette même salle dans ce même
  processus** avec une position de salle connue > 5 s. Alors il émet UNE reprise
  `control seek` à cette dernière position connue (pas la position VLC brute),
  toast « Reprise à HH:MM:SS ». Un premier join dans une salle neuve ne déclenche
  jamais de reprise, quel que soit l'état de VLC. Si plusieurs clients reviennent
  ensemble, chacun peut émettre sa reprise : le dernier gagne, positions quasi
  identiques par construction (comportement assumé).
- Serveur : ping WS de transport toutes les 30 s, timeout de lecture 60 s.
- Tout message inconnu est ignoré (forward-compat) mais loggé en debug.
