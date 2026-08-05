# Rapport — VS-003 Serveur (agent Opus, 2026-08-05)

## Fichiers créés

- `cmd/vibesync-server/main.go` — binaire : env (`VIBESYNC_ADDR`, `VIBESYNC_PASSWORD`,
  `VIBESYNC_LOG`), slog texte, arrêt gracieux SIGINT/SIGTERM.
- `internal/server/server.go` — `Config`, `New` + options (`WithClock`, `WithLogger`),
  routes `GET /ws` et `GET /healthz`.
- `internal/server/hub.go` — salles créées à la volée, détruites quand vides.
- `internal/server/room.go` — état autoritatif et toute la logique métier.
- `internal/server/conn.go` — transport WebSocket (read pump / write pump).
- `internal/server/clock.go` — horloge injectable.
- Tests : `room_test.go`, `hub_test.go`, `ws_test.go`, `config_test.go`, `helpers_test.go`
  (≈ 45 tests : unitaires à horloge factice + intégration httptest/gorilla).

## Choix d'implémentation

- **Sérialisation** : un mutex par salle ; toute la logique s'exécute sous ce verrou, les
  envois sont non bloquants. Ordre de verrous strict hub → salle ; `Hub.join` tient le
  verrou du hub pendant l'insertion pour qu'une salle ne soit pas détruite en cours de join.
- **Écritures** : une goroutine d'écriture par connexion, file de 64 messages ; file
  saturée = client trop lent, on ferme (il se reconnecte).
- **Fermeture propre** : après un `error`, la lecture « linger » jusqu'à la trame Close du
  pair (max 2 s) pour que le message d'erreur ne soit pas perdu par un reset TCP.
- **Robustesse** : ping WS 30 s, read deadline 60 s, `SetReadLimit` 64 Kio, types inconnus
  et `data` mal typée ignorés en debug, positions NaN/Inf/négatives/absurdes assainies,
  pseudos/salles validés (non vides, ≤ 32/64 runes, sans caractères de contrôle).

## Interprétations de spec (trous comblés)

1. **Latence** : le `ping` applicatif est client→serveur uniquement, le serveur ne peut pas
   en déduire un RTT. La latence est donc mesurée sur les **trames ping/pong WebSocket**
   (moyenne glissante des 5 dernières), avec un ping immédiat à la connexion.
2. **chatEvent** : le tableau de la spec dit `chat` côté serveur, `protocol.go` définit
   `TypeChatEvent = "chatEvent"` ; j'ai suivi le code (consigne du ticket).
3. **Pause auto** : `buffering=true` déclenche immédiatement ; le seuil de retard (> 4 s)
   doit persister > 2 s. La salle est figée à la position de **référence** ; `setBy="server"`.
4. **setBy** = identifiant serveur (`u1`), reconnaissable via `welcome.selfId`.
5. **Ready-gate** : refus = `toast` warn **et** `roomState` renvoyés au seul émetteur (pour
   qu'il annule sa lecture locale) ; les autres ne reçoivent rien.
6. Pseudo unique **insensible à la casse** ; nom de salle sensible à la casse.
7. `users` diffusé à chaque changement, mais **throttlé à 1/s** pour les `report`.
8. Avertissement de durée émis à chaque `setFile` créant un écart > 2 s ; chat tronqué à
   500 caractères ; `CheckOrigin` permissif (clients desktop, pas de cookies).

## Limites connues

- Pas de `-race` (cgo/gcc absent de la machine) ; concurrence couverte par un test de
  charge multi-goroutines.
- Latence rafraîchie toutes les 30 s seulement (cf. interprétation 1) ; un `ping`
  applicatif renvoyant l'écho côté client permettrait mieux — évolution de spec.
- Aucune limite de débit (rate limit) ni de taille de salle.
