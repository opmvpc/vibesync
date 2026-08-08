---
id: VS-040
titre: Double-clic sur le fichier d un participant pour basculer dessus
statut: terminé
priorité: moyenne
dépend-de: [VS-039]
créé: 2026-08-08
mis-à-jour: 2026-08-09
---

Demande Thibault : dans la liste des participants, double-cliquer le nom de
fichier d un autre utilisateur = le rechercher dans mes dossiers médias et
l ouvrir (même mécanique que le bandeau VS-026, déclenchée à la demande).
Utile pour « passer à l épisode suivant » en suivant quelqu un.

## Critères d'acceptation

- [x] macOS : double-clic sur une ligne de la liste des participants → même
      chemin que le bandeau « X regarde … » (recherche bornée hors thread
      principal, jeton de génération, lancement de VLC, bandeau « introuvable »
      + raccourci Réglages en cas d'échec)
- [x] Windows : idem — le déclencheur existait depuis VS-026, il lui manquait
      le cas limite « fichier identique au mien »
- [x] Cas limites, sur les deux clients : double-clic sur soi-même = rien ;
      participant sans fichier déclaré = rien ; fichier identique au mien = rien
- [x] Affordance honnête : la ligne ne s'allume au survol que là où le
      double-clic mène quelque part (surbrillance Windows, surbrillance +
      infobulle macOS)
- [x] Rien de nouveau côté moteur : un déclencheur d'interface de plus vers la
      recherche existante (`core/src/media_core.c`), aucun vecteur touché
- [x] Tests : 6 checks Swift (`ParticipantFileTests`) + 5 checks C
      (`ui_user_openable`, section « ui »)

## Ce qui a été fait

La règle « la ligne de ce participant mène-t-elle quelque part ? » est devenue
UNE fonction pure par client, lue à la fois par l'affordance et par l'action —
et aussi par le bandeau VS-026/VS-039, qui appliquait la même règle en copie :

- macOS — `AppModel.participantFileToOpen(user:selfId:myFile:)`, et le corps de
  `openWatchedFile()` extrait en `searchAndOpenMedia(named:)` partagé par les
  deux déclencheurs. La vue : `ParticipantRow` (RoomView.swift).
- Windows — `ui_user_openable(const UiUser *)` (ui.h/ui.c) plus le nouveau
  champ `UiUser.same_file`, calculé par `refresh_user_files()` depuis le
  MOTEUR (jamais depuis le miroir de la vue : leçon de VS-039) à chaque pas et
  avant chaque calcul de bandeau.

## Journal du ticket

- 2026-08-08 : créé (retour du test manuel v0.2.3).
- 2026-08-09 : livré sur les deux clients. Validé : swift 61/61,
  build-macos.sh, séance réelle macOS 18/18 contre wss://vibesync.choboai.com ;
  VM Win11 (clone jetable) `build.bat test` 1652 checks / vecteurs 15/15,
  release 264 Ko (270 336 o, 53 % du budget), capture de la salle relue.
  Rapport : `docs/research/2026-08-09-vs040-double-clic.md`.
