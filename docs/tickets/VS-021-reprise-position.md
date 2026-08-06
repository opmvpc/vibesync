---
id: VS-021
titre: Reprise de séance — la salle garde sa position quand elle se vide
statut: terminé
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

- [x] Serveur : linger implémenté (horloge injectable, GC périodique), env dédiée,
      welcome d'un revenant = position/pause de la séance interrompue
- [x] La reprise de session (jeton) et les plafonds cohabitent proprement
      (review terra : pas de course exploitable, Hub.mu sérialise GC/join)
- [x] Tests unitaires + e2e « tout le monde crash, retour à 2 min → position intacte »
- [x] Toast d'accueil « séance reprise à HH:MM:SS » (client de réf + C)

## Journal du ticket

- 2026-08-06 : créé.
- 2026-08-06 : livré (Go/serveur) puis toast C (1d5a4ad). Complété par la reprise
  « salle vierge » de VS-024 pour le cas où le linger expire. Dans v0.2.0. Terminé.
