---
id: VS-021
titre: Reprise de séance — la salle garde sa position quand elle se vide
statut: ouvert
priorité: haute
dépend-de: []
créé: 2026-08-06
mis-à-jour: 2026-08-06
---

## Contexte

Demande de Thibault : ne pas devoir retrouver le timecode à la main si tout plante
pendant une séance. Spec amendée (§Modèle) : salle vide conservée avec son état
pendant `VIBESYNC_ROOM_LINGER` (défaut 30 min) puis détruite.

## Critères d'acceptation

- [ ] Serveur : linger implémenté (horloge injectable, GC périodique), env dédiée,
      welcome d'un revenant = position/pause de la séance interrompue
- [ ] La reprise de session (jeton) et les plafonds cohabitent proprement
- [ ] Tests unitaires + e2e « tout le monde crash, retour à 2 min → position intacte »
- [ ] Toast d'accueil « séance reprise à HH:MM:SS » (client de réf + C)

## Journal du ticket

- 2026-08-06 : créé.
