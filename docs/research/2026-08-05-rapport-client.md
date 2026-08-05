# Rapport : client desktop vibesync (VS-004 + VS-005, agent Opus)

## Livré

- `internal/vlc/` — `Controller` (interface), `HTTPClient` (status.json, position
  fine `position × length`, `pl_forcepause`/`pl_forceresume`/`seek`/`rate`,
  basic auth user vide), `Launch` (port + mot de passe aléatoires, `WaitReady`),
  `Locate` (env `VIBESYNC_VLC` prioritaire, chemins Win/mac/Linux, PATH),
  `BufferingDetector`. `internal/vlc/vlctest/` — faux VLC httptest stateful +
  horloge manuelle, réutilisables par tous les tests.
- `internal/client/` — moteur : offset médian (5 pongs), position attendue,
  correction (zone morte 0,1 s / nudge 1,05×–0,95× / seek dur ≥ 2 s + affinage,
  jamais de nudge en pause), détection d'action utilisateur, grâce 500 ms,
  `setFile`/`report` 1 s/`ping` 2 s, reconnexion 1→10 s avec re-hello.
  Horloge, transport (`Dialer`/`Conn`) et lanceur VLC injectables.
- `internal/webui/` — serveur 127.0.0.1, assets `embed.FS`, canal `/ui`
  (enveloppe `{type,data}`, token), explorateur de fichiers, normalisation
  d'adresse, ouverture navigateur. UI single-page FR, sombre.
- `cmd/vibesync/main.go` — câblage, `--headless`/`--ui-addr`/`--keep-vlc`,
  ligne stdout `{"uiPort","uiToken","uiURL"}`, arrêt propre.
- `docs/research/2026-08-05-ui-protocol-draft.md` — contrat `/ui` documenté.
- `test/vectors/*.json` — 6 vecteurs de conformité générés (entrées horodatées →
  décisions attendues), pour les futurs portages C/Swift.

## Interprétations de spec (trous comblés, depuis gelés dans la spec)

1. **Hold après action volontaire** (désormais §Hold post-action) : voir
   post-review ci-dessous.
2. **Seek en pause** : seuil 0,6 s (désormais dans la spec).
3. Corrections pilotées uniquement par le poll 200 ms (latence ≤ 200 ms sur un
   `roomState`) ; VLC n'est jamais commandé hors de la boucle.
4. `report` envoyé seulement quand un fichier est chargé (sinon le serveur
   verrait un membre figé à 0 et pauserait la salle).
5. Adresse sans schéma → `wss`, sauf hôte local (`localhost`/`127.0.0.1`) → `ws`.
6. Erreurs `version_mismatch`/`bad_password`/`name_taken` = fatales, pas de
   reconnexion ; les autres sont réessayées.
7. Le serveur→client `chat` est accepté sous `chatEvent` (constante Go) **et**
   `chat` (tableau de la spec).

## Post-review sol

Les 3 blocs gelés dans `docs/protocol.md` (Hold post-action, Conditions de
correction, Assainissement) sont implémentés et couverts. Corrections :

- **Corrections gelées hors session** : `planLocked` exige phase connectée,
  état de référence valide **et** une première mesure d'offset. `onSessionEnd`,
  `Disconnect` et toute transition hors « connecté » invalident la référence
  (`invalidateReferenceLocked`) — plus de pilotage de VLC avec un état périmé
  pendant un backoff.
- **Hold post-action** : deux horloges distinctes — `graceUntil` (500 ms,
  détection) et `userHoldUntil` (2 s, corrections). Seul l'écho (`setBy` = soi)
  ou l'expiration lève le hold ; un `roomState` d'autrui reçu pendant le hold
  est mémorisé (dernier gagne) et appliqué à l'expiration faute d'écho.
- **Vecteurs = golden files** : `TestVectors` compare octet à octet les 12
  fichiers committés ; régénération uniquement via
  `go test ./internal/client -run TestVectors -update`. Chaque vecteur encode
  son `initialVLC` complet. Vérifié : une variation de seuil fait échouer le test.
- **Génération de connexion** : compteur `connGen` vérifié avant toute
  transition de phase et avant traitement d'un message — une boucle annulée ne
  peut plus toucher l'état de la suivante.
- **Assainissement** : `roomState` validé (finitude, `rate` ∈ [0,25 ; 4],
  position ≥ 0, `refServerMs` présent en lecture) ; fraction VLC bornée à [0,1]
  avant `× length` ; seek UI borné à [0, durée] et positions non finies
  rejetées ; erreur d'écriture → fermeture de la connexion (reconnexion +
  re-déclaration au `welcome`) au lieu d'un drop silencieux.
- **Seuil pause / hystérésis** : seek dès `|drift| ≥ 0,6 s` (condition « mêmes
  secondes arrondies » supprimée : le cas 10,51 vs 11,49 est bien corrigé) ;
  nudge engagé au-delà de 0,1 s, relâché sous 0,03 s.
- **Bug trouvé en corrigeant #8** : si la toute première observation de VLC
  tombait dans une fenêtre de grâce, l'attendu n'était jamais initialisé et la
  détection d'action utilisateur restait muette pour toute la session. L'attendu
  est désormais initialisé même pendant une grâce, mais jamais *remplacé*.
- **Piège corrigé au passage** : `Seek(NaN)` était interprété comme « position
  courante » (sentinelle interne partagée avec `Play`/`Pause`) ; la sentinelle
  est maintenant un booléen explicite.

QA : `go build ./...`, `go vet`, `gofmt` propres ; `go test -count=5 -shuffle=on`
vert sur `internal/{client,vlc,vlctest,webui}` ; cross-compilation darwin/arm64 OK.

## Limites connues

- Buffering détecté à l'heuristique (position figée en lecture), l'API HTTP de
  VLC ne l'expose pas.
- Précision finale bornée par le seek entier de VLC : convergence en douceur au
  rate (~10 s pour 0,5 s d'écart), conforme à ADR-003.
- Aucun test avec un vrai VLC (interdit dans mon périmètre) ; le driver a été
  validé contre le faux VLC + smoke test du binaire (`--headless`).
