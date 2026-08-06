---
id: VS-017
titre: Faux buffering sur seek → pauses auto intempestives (bug terrain)
statut: ouvert
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

- [ ] Client Go : détection de buffering suspendue 2 s après seek/transition
- [ ] Serveur : pas de pause auto seul en salle ; ≥ 5 s entre deux pauses auto ;
      reports de l'auteur d'un control ignorés 2 s
- [ ] Tests : repro e2e « seek utilisateur ne déclenche pas de pause auto »
- [ ] Port des règles client dans engine.c/vlc.c (12+ vecteurs verts)
- [ ] Client C : log de diagnostic optionnel (%APPDATA%\vibesync.log)

## Journal du ticket

- 2026-08-06 : créé (bug remonté par Thibault).
