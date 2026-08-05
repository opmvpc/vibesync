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
- `internal/server/ratelimit.go` — seau à jetons anti-flood (post-review).
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
2. **chatEvent** : incohérence de la spec (corrigée depuis) ; j'ai suivi `protocol.go`,
   le type serveur→client est bien `chatEvent`.
3. **Pause auto** : `buffering=true` déclenche immédiatement ; le seuil de retard (> 4 s)
   doit persister > 2 s. La salle est figée à la position de **référence** ; `setBy="server"`.
4. **setBy** = identifiant serveur (`u1`), reconnaissable via `welcome.selfId`.
5. **Ready-gate** : refus = `toast` warn **et** `roomState` renvoyés au seul émetteur (pour
   qu'il annule sa lecture locale) ; les autres ne reçoivent rien.
6. Pseudo unique **insensible à la casse** ; nom de salle sensible à la casse.
7. `users` diffusé à chaque changement, mais **throttlé à 1/s** pour les `report`.
8. Avertissement de durée émis à chaque `setFile` créant un écart > 2 s ; chat tronqué à
   500 caractères ; `CheckOrigin` permissif (clients desktop, pas de cookies).

## Post-review (corrections après review croisée codex terra)

- **Anti-abus (spec §6)** — nouveau `internal/server/ratelimit.go` : seau à jetons piloté par
  l'horloge injectée, un par connexion. Budget global 20 msg/s, rafale 40 → épuisement =
  flood → `error` `protocol` + fermeture. Budget chat 5 msg/s, rafale 10 → **choix
  documenté** : le message est rejeté avec un `toast` warn sans fermer la connexion (un
  humain peut légitimement enchaîner quelques messages ; le budget global reste seul juge
  du flood).
- **Plafonds** configurables : `VIBESYNC_MAX_CLIENTS` (200), `VIBESYNC_MAX_ROOMS` (50),
  `VIBESYNC_MAX_ROOM_SIZE` (20) ; valeur absente/illisible/≤ 0 → défaut, loggé en warn.
  **Choix documenté** : le plafond de connexions est refusé *avant* l'upgrade (HTTP 503 +
  `Retry-After`, le client applique son backoff) ; les plafonds de salles/membres sont
  refusés au `hello` par `error` code `protocol` (texte explicite) plutôt qu'un `toast`,
  car le client ne rejoint aucune salle et resterait sinon dans le vide.
- **Mot de passe** : condensats SHA-256 des deux côtés puis `subtle.ConstantTimeCompare` →
  ni le contenu ni la longueur du secret ne fuient. Le condensat attendu est précalculé.
- **Validation** : les caractères de FORMAT Unicode (`unicode.Cf` : zero-width, joiner,
  bidi) sont rejetés en plus des caractères de contrôle, pour pseudos et salles.
- **Durée ≤ 0** = « inconnue » : test dédié (`TestSetFileDureeInconnueNAvertitPas`) dans les
  deux sens, avec renvoi à la spec en commentaire.
- **Tests de salle détruite** : plus d'attente active — le hub expose un hook interne
  `onRoomDestroyed` (nil en production) que le harnais branche sur un canal ; ajout du cas
  join concurrent avec la destruction de la dernière session, répété 200 fois, qui vérifie
  l'invariant « une seule salle vivante, contenant exactement le nouvel arrivant ».
- Nouveaux tests : flood → fermeture, débit normal (10 msg/s sur 6 s simulées) sans effet,
  throttle chat sans fermeture, plafonds salle/salles/connexions, seau à jetons (rafale,
  remplissage, débit soutenu), plafonds d'environnement, hachage du mot de passe.

## Limites connues

- Pas de `-race` (cgo/gcc absent de la machine) ; concurrence couverte par un test de
  charge multi-goroutines.
- Latence rafraîchie toutes les 30 s seulement (cf. interprétation 1) ; un `ping`
  applicatif renvoyant l'écho côté client permettrait mieux — évolution de spec.
- Aucune limite de débit (rate limit) ni de taille de salle.
