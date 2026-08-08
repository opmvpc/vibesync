---
id: VS-039
titre: Changement de fichier en cours de salle — re-déclaration + reset de position
statut: ouvert
priorité: critique
dépend-de: [VS-026]
créé: 2026-08-08
mis-à-jour: 2026-08-08
---

## Contexte (test manuel Thibault, v0.2.3, 2 clients mac)

1. test1 change de vidéo (Changer de fichier) : les autres ne voient AUCUN
   bandeau — la re-déclaration du nouveau fichier ne déclenche pas la
   proposition de récupération chez test2 (cas « épisode suivant »).
2. Une fois la même nouvelle vidéo choisie des deux côtés, la sync est morte :
   bannière « Pause auto : test2 a 16,6 s de retard » en boucle sur une vidéo
   de 42 s — la position de salle de l ancien média contamine le nouveau.

## Critères

- [ ] Spécifier le flux « changement de fichier en cours de salle » dans
      protocol.md (reset de la position de référence ? nouvelle déclaration
      diffusée ? état ready ?)
- [ ] Bandeau de récupération déclenché chez les autres au changement
- [ ] Plus de « Pause auto » fantôme après bascule des deux côtés
- [ ] Vecteur(s) + séances réelles 2 clients (mac et VM)
