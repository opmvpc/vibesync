---
id: VS-024
titre: Robustesse déconnexions — file de chat hors ligne, reconnexion sans écrasement, salle vierge
statut: terminé
priorité: haute
dépend-de: [VS-017, VS-021]
créé: 2026-08-06
mis-à-jour: 2026-08-06
---

## Contexte

Retours de Thibault : un chat envoyé pendant une coupure est perdu ; questions sur
les désyncs quand le serveur disparaît ou quand la connexion d'un client flanche —
« faut pas qu'il écrase le timecode en se reconnectant ». Design gelé dans la spec
(§Erreurs et robustesse : file hors ligne, reconnexion sans écrasement, salle
vierge). Nota : l'auto-deploy Coolify fait redémarrer le serveur à chaque push →
le cas « salle vierge » est fréquent en pratique.

## Critères d'acceptation

- [x] Client Go de référence : file de chat bornée (20) vidée après re-hello ;
      re-déclaration ready/fichier post-welcome ; aucun control émis pendant
      l'alignement (test dédié) ; reprise « salle vierge » (une fois par
      connexion, toast)
- [x] Tests e2e : coupure serveur (restart du httptest) pendant lecture → les 2
      clients continuent localement, se reconnectent, la position est reprise
      (salle vierge) sans retour à 0 ; chat composé hors ligne livré au retour
- [x] Vecteurs : nouveaux scénarios si le moteur pur est concerné (alignement
      sans control, reprise vierge) — vecteurs 12 et 13 ; le 13 en cours de
      régénération (champ `keepOutput`, état initial complet, écho exact)
- [x] Port C (engine.c + ui) livré (1d5a4ad) puis validé contre le vecteur 13
      régénéré + règles resserrées terra (3a70fa0, 13/13) ; Swift au polissage Mac
- [x] UI : indicateur clair « hors ligne, reconnexion... » + chat marqué
      « en attente » pour les messages en file (C : file rendue en gris sous
      l'historique, jamais dedans — évite les doublons à l'écho serveur)

## Journal du ticket

- 2026-08-06 : créé (retours terrain), spec amendée.
- 2026-08-06 : Go livré (ef13ee7) puis resserré post-review terra (file liée à la
  salle, reprise vierge « précédemment connecté », en cours agent Go). Port C
  livré (1d5a4ad) : file 20 « en attente », re-déclaration post-welcome, reprise
  via emit_user_control, + fix d'un bug miroir du Go (ready effacé à chaque
  reconnexion). Bloquant vecteur 13 identifié : le format golden n'encodait pas
  la convention event/eventKeep → champ `keepOutput` demandé au générateur.
- 2026-08-06 : resserrages Go livrés (c811207), vecteur 13 régénéré (keepOutput,
  scenario, écho exact 1800.8), port C aligné (3a70fa0) — 13/13 des deux côtés,
  CI verte. Dans v0.2.0. Terminé (reliquat Swift → VS-015).
