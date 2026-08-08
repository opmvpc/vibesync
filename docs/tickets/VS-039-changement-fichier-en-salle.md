---
id: VS-039
titre: Changement de fichier en cours de salle — re-déclaration + reset de position
statut: terminé
priorité: critique
dépend-de: [VS-026]
créé: 2026-08-08
mis-à-jour: 2026-08-09
---

## Contexte (test manuel Thibault, v0.2.3, 2 clients mac)

1. test1 change de vidéo (Changer de fichier) : les autres ne voient AUCUN
   bandeau — la re-déclaration du nouveau fichier ne déclenche pas la
   proposition de récupération chez test2 (cas « épisode suivant »).
2. Une fois la même nouvelle vidéo choisie des deux côtés, la sync est morte :
   bannière « Pause auto : test2 a 16,6 s de retard » en boucle sur une vidéo
   de 42 s — la position de salle de l ancien média contamine le nouveau.

## Critères

- [x] Spécifier le flux « changement de fichier en cours de salle » dans
      protocol.md (reset de la position de référence ? nouvelle déclaration
      diffusée ? état ready ?) → règle serveur **5bis** + §Chargement de fichier
      + §Proposition de récupération. Le ready-gate n'est PAS rejoué (choix
      documenté, alternative écartée).
- [x] Bandeau de récupération déclenché chez les autres au changement
- [x] Plus de « Pause auto » fantôme après bascule des deux côtés
- [x] Vecteur(s) + séances réelles 2 clients (mac et VM) → vecteur 15
      `15-changement-de-fichier` (les 14 gelés inchangés), séance mac 18/18 et
      séance VM 21/21

## Cause racine

1. `refresh_watch_banner`/`refreshWatchBanner` sortaient dès que NOUS avions un
   fichier : la mécanique VS-026 n'existait que pour quelqu'un sans fichier.
2. La position de salle est média-agnostique. Rien ne la remettait à zéro au
   changement de média côté serveur, et le moteur client gardait sa référence à
   l'ouverture : `expected = clamp(position_ancien_média, durée_nouveau)` = la
   FIN du nouveau fichier → seek fatal, retard infini, `Pause auto` en boucle.
   Symptôme intermittent car c'est une course avec l'aller-retour serveur.

## Correctif

- Serveur `internal/server/room.go` : règle 5bis (reset salle vierge + toast).
  **Change le serveur → auto-deploy prod au push.**
- Moteur `core/src/engine.c` + référence Go : `open_file` invalide l'état de
  salle, l'historique de dérive et la mémoire de séance.
- UI mac + Win32 : bandeau déclenché sur « nom différent du nôtre » ; marqueur de
  refus explicite côté Windows (parité avec `dismissedWatchFile`).
- Harnais : scénario de changement de fichier ajouté aux deux séances réelles
  (mac et VM), plus `watchShow`/`watchFile`/`autoPauseToasts` dans l'état auto.

## Journal du ticket

- 2026-08-08 : créé (retour terrain Thibault, v0.2.3).
- 2026-08-09 : reproduit au harnais réel avant analyse (deux exécutions : la
  course expliquée), spec écrite d'abord, livré et validé des deux côtés.
  Rapport : `docs/research/2026-08-09-vs039-changement-fichier.md`. Terminé.
