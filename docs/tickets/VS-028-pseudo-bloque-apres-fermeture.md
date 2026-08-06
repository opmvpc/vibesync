---
id: VS-028
titre: Pseudo bloqué (name_taken) après fermeture de l'app — départ non signalé au serveur
statut: ouvert
priorité: haute
dépend-de: []
créé: 2026-08-06
mis-à-jour: 2026-08-06
---

## Contexte

Retour terrain de Thibault (première vraie séance C↔C) : « quand on se déconnecte,
le serveur est pas au courant ; on peut pas se reconnecter juste après avec le même
pseudo ». Diagnostic : (a) le jeton de session est généré PAR PROCESSUS
(main.c:1866) — relancer l'exe = nouveau jeton = `name_taken` tant que le zombie
n'a pas expiré (timeout lecture serveur 60 s) ; (b) le chemin de fermeture de
l'app n'envoie peut-être pas la close WS proprement. Spec amendée : jeton persisté
dans l'ini + départ volontaire = close 1000 envoyée.

## Critères d'acceptation

- [ ] Jeton de session persisté dans vibesync.ini (généré au premier lancement,
      réutilisé ensuite) — un relancement de l'exe récupère le pseudo immédiatement
      via la reprise de session serveur (règle 6), même si le zombie vit encore
- [ ] Fermeture de fenêtre et « Quitter la salle » : close WebSocket 1000 envoyée
      (et un court délai pour qu'elle parte) avant destruction — vérifier que le
      serveur retire le membre immédiatement (test : reconnexion instantanée avec
      un jeton DIFFÉRENT passe aussi)
- [ ] Client Go de référence : même persistance du jeton (fichier d'état) pour que
      le comportement de référence colle à la spec
- [ ] Tests : ini avec/sans jeton, round-trip ; e2e reprise immédiate après
      fermeture volontaire et après kill brutal (jeton persistant)
- [ ] macOS : à porter au polissage Swift (UserDefaults)

## Journal du ticket

- 2026-08-06 : créé (retour terrain), spec amendée (jeton persisté, close 1000).
