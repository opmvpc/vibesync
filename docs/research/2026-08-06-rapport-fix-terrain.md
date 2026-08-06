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
