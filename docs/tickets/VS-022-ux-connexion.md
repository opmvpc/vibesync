---
id: VS-022
titre: UX de connexion — adresses intelligentes, erreurs actionnables, pas de retry sur mdp
statut: ouvert
priorité: haute
dépend-de: [VS-018]
créé: 2026-08-06
mis-à-jour: 2026-08-06
---

## Contexte

Retour terrain de Thibault (capture à l'appui) : il a tapé `ws://` au lieu de
`wss://` → boucle « Serveur injoignable. Nouvelle tentative... » sans explication ;
un mauvais mot de passe relancerait pareillement les tentatives ; impossible de
corriger les champs sans relancer l'exe.

## Critères d'acceptation

- [ ] Normalisation d'adresse comme la référence Go (internal/webui/address.go) :
      « vibesync.choboai.com » → `wss://…/ws` ; schéma explicite respecté ;
      si `ws://` échoue mais que `https://hote/healthz` répond → message
      « Le serveur répond en chiffré : utilise wss:// » (ou bascule proposée)
- [ ] Indicateur de joignabilité en direct sur l'écran de connexion : au blur du
      champ serveur (ou bouton « Tester »), GET healthz → « ● Serveur en ligne
      (113 ms) » / « ● Injoignable » avec la raison (DNS, TLS, timeout)
- [ ] Erreurs FATALES du protocole (`bad_password`, `name_taken`,
      `version_mismatch`) : AUCUNE reconnexion auto, retour immédiat au
      formulaire éditable, message spécifique en clair (« Mot de passe incorrect »,
      focus sur le champ concerné), bouton Connecter réactivé
- [ ] Le backoff de reconnexion ne concerne que les pannes réseau, et un bouton
      Annuler permet de l'interrompre pour modifier les champs
- [ ] Chaque état d'erreur dit QUOI corriger (messages français concrets)
- [ ] Tests : normalisation (cas de webui/address_test.go portés), machine à états
      connexion (fatal vs réseau), build.bat test + asan verts

## Journal du ticket

- 2026-08-06 : créé (retour terrain avec capture ws:// en boucle).
