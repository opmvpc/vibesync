---
id: VS-024
titre: Robustesse déconnexions — file de chat hors ligne, reconnexion sans écrasement, salle vierge
statut: ouvert
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

- [ ] Client Go de référence : file de chat bornée (20) vidée après re-hello ;
      re-déclaration ready/fichier post-welcome ; aucun control émis pendant
      l'alignement (test dédié) ; reprise « salle vierge » (une fois par
      connexion, toast)
- [ ] Tests e2e : coupure serveur (restart du httptest) pendant lecture → les 2
      clients continuent localement, se reconnectent, la position est reprise
      (salle vierge) sans retour à 0 ; chat composé hors ligne livré au retour
- [ ] Vecteurs : nouveaux scénarios si le moteur pur est concerné (alignement
      sans control, reprise vierge)
- [ ] Port C (engine.c + ui) puis Swift des mêmes règles
- [ ] UI : indicateur clair « hors ligne, reconnexion... » + chat marqué
      « en attente » pour les messages en file

## Journal du ticket

- 2026-08-06 : créé (retours terrain), spec amendée.
