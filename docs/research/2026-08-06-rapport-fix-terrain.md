# Rapport — retours terrain VS-017 / VS-021 / VS-023 (2026-08-06)

## VS-017 — faux buffering sur seek

**Client.** `vlc.BufferingDetector` gagne `Suspend`/`Suspended` ; `Reset` oublie
désormais le stall en cours (bug latent : un stall antérieur pouvait déclencher
un buffering dès la deuxième observation). Le moteur suspend 2 s
(`BufferingSuspend`) à chaque seek commandé, transition play/pause
(`armLocked`), action utilisateur détectée, control d'UI et ouverture de
fichier.

**Point non trivial :** la suspension ne lève pas un verdict déjà posé. Le
moteur re-seeke un lecteur bloqué toutes les ~2 s ; si le seek effaçait le
diagnostic, un vrai buffering ne serait jamais remonté (fenêtre de suspension =
période de correction). Le verdict ne retombe qu'à la reprise réelle de la
position.

**Serveur.** `autoPauseAllowedLocked` : jamais seul en salle, cooldown 5 s par
salle, reports de l'auteur d'un control neutres 2 s (`member.lastControlAt`).
Ne concerne que les `report` ; la pause sur déconnexion reste inconditionnelle.

**Repro e2e 09** : rafale de seeks avec position figée 1,2 s (ce que fait un vrai
VLC). Vérifiée rouge avec les deux correctifs neutralisés, verte ensuite.
Deuxième temps : un blocage durable hors fenêtre de seek fige toujours la salle.

## VS-021 — reprise de séance

Salle vide → `markEmpty` gèle la séance (position figée, pause) et note
`emptySince` ; `hub.gc` (période bornée 5 s–1 min, passé aussi avant tout
contrôle de plafond) détruit après `VIBESYNC_ROOM_LINGER` (défaut 30 m, durée
Go, invalide → défaut). **Décision :** une salle en linger compte dans
`maxRooms`, une salle expirée jamais. `RoomLingerDisabled` rétablit la
destruction immédiate — le harnais WS l'utilise pour garder
`waitRoomDestroyed`. Toast « Séance reprise à HH:MM:SS » au revenant. E2e 10 :
crash des deux clients, 2 min simulées (horloge serveur décalable), position
retrouvée à < 1 s.

## VS-023 — versions

`Config.Version` (ldflags `main.appVersion`, défaut « dev ») et `DownloadURL`
(`VIBESYNC_DOWNLOAD_URL`) portés au `welcome` via `buildInfo`. Helper client
`NewerVersion` (semver numérique, `v` et suffixe ignorés, entrée illisible →
jamais de bannière) → toast info. Injection vérifiée (`version=0.2.0`).

**QA** : build, vet, gofmt, staticcheck, `-count=2 -shuffle=on` trois fois verts.
Aucun vecteur impacté.

## VS-024 — robustesse des déconnexions

**File hors ligne.** `Engine.chatQueue` (20 max, plus anciens abandonnés)
alimentée quand `conn == nil || phase != connected`, vidée dans l'ordre au
welcome. Elle survit à `invalidateReferenceLocked`, contrairement à `outbox` —
c'est ce qui garantit que control, ready, report et ping ne sont jamais rejoués.
`Snapshot.PendingChats` expose la file à l'UI.

**Bug trouvé en chemin :** le welcome appelait `readyFromUsersLocked`, or le
serveur vient d'y créer un membre neuf donc « pas prêt » — toute reconnexion
(et donc tout redéploiement) effaçait le ready. Supprimé : au (re)join c'est
l'état local qui est re-déclaré, le broadcast `users` resynchronise ensuite.

**Alignement sans control.** Déjà garanti par la grâce + la mise à jour de
`expect` à chaque poll ; c'était une supposition, c'est désormais un test
(welcome à +1200 s → seek local, zéro control) et c'était déjà visible dans le
golden 12.

**Salle vierge.** `virginResumeLocked` (setBy vide, position 0, VLC > 5 s) émet
UNE reprise `control seek` via `userControlLocked` — donc avec le hold, qui est
exactement ce qui empêche l'alignement sur la position 0. Un client plus lent
voit `setBy` renseigné et se range : testé. Nouveau vecteur
`13-reprise-salle-vierge` (aucun des 12 autres n'a bougé) ; il a fallu un
`eventKeep` pour que la réaction immédiate au welcome entre dans la trace.

**Bug trouvé en chemin (2) :** le gel d'une salle qui se vide se faisait en deux
temps (retrait du membre, puis `markEmpty` sous le verrou du hub). Un saut
d'horloge entre les deux figeait la séance 2 min trop loin — flake reproduit en
`-shuffle=on`. Le gel est maintenant dans `Room.leave`, atomique avec le
retrait ; test de non-régression dédié.

**E2e** : `11-redemarrage-du-serveur` (vérifié rouge sans la reprise vierge),
`12-chat-compose-hors-ligne` (rouge sans la file), `13-coupure-client-sans-
ecrasement`. Le harnais sait désormais redémarrer le serveur : `trackingListener`
coupe les connexions hijackées, que `httptest.Close` oublie, et le dialer résout
l'adresse à chaque tentative.
