---
id: VS-017
titre: Faux buffering sur seek → pauses auto intempestives (bug terrain)
statut: terminé
priorité: haute
dépend-de: []
créé: 2026-08-06
mis-à-jour: 2026-08-06
---

## Contexte

Test réel de Thibault sur le serveur Coolify : seek/pause/retour depuis VLC →
« commandes pas toujours prises en compte » + 5 « Pause auto : opmvpc bufferise »
dans les logs serveur, seul dans la salle. Cause : le seek fige la position → le
détecteur (700 ms) crie buffering → pause auto qui écrase l'action. Règles
correctives gelées dans la spec (§Comportements serveur 2, §Comportements client
Buffering).

## Critères d'acceptation

- [x] Client Go : détection de buffering suspendue 2 s après seek/transition
- [x] Serveur : pas de pause auto seul en salle ; ≥ 5 s entre deux pauses auto ;
      reports de l'auteur d'un control ignorés 2 s
- [x] Tests : repro e2e « seek utilisateur ne déclenche pas de pause auto »
- [x] Port des règles client dans engine.c/vlc.c (12+ vecteurs verts)
- [x] Client C : log de diagnostic optionnel (%APPDATA%\vibesync.log)

## Journal du ticket

- 2026-08-06 : créé (bug remonté par Thibault).
- 2026-08-06 : Go+serveur livrés et déployés ; port C livré (1d5a4ad, suspension
  2 s alignée sur Suspend/Reset du Go). Anti-masquage resserré post-review terra
  (spec af4c9f8) : battement 1 s entre suspensions, figé diagnostiqué ≤ 5 s —
  vérifié des deux côtés au rendu de l'agent Go. Terminé.
