---
id: VS-038
titre: Resync 1× constant + micro-seek — suppression du nudge ±5 %
statut: terminé
priorité: haute
dépend-de: []
créé: 2026-08-08
mis-à-jour: 2026-08-08
---

## Contexte

Retour terrain de Thibault (test manuel v0.2.2) : la sync est excellente mais
le nudge ±5 % est désagréable — la vitesse yo-yote en permanence (zone morte
0,1 s SOUS le bruit de mesure VLC ±0,15 s) et 5 % s'entendent. « Une vidéo se
regarde à vitesse constante 1×. » Recherche plateformes
(docs/research/2026-08-08-recherche-sync-plateformes.md) : Syncplay n'agit
qu'à partir de 1,5 s ; Jellyfin a désactivé sa correction auto par défaut en
2023. Décision Thibault : option B (1× + micro-seek), le ralenti doux (option
A) reste une évolution possible si les micro-seeks s'avéraient trop fréquents.

## Design retenu

- La vitesse de lecture n'est PLUS JAMAIS modifiée pour corriger la dérive
  (le moteur continue de RESTAURER la vitesse de référence si VLC n'y est pas,
  p.ex. utilisateur qui a touché la vitesse dans VLC).
- Zone morte : 1,5 s (au-dessus du bruit ±0,15 s et du perceptible).
- Persistance anti-bruit : la dérive doit dépasser le seuil en MÉDIANE sur les
  ~5 derniers polls (1 s) avant de déclencher le micro-seek de recalage sur la
  position de référence.
- Écart énorme (≥ 5 s, réveil de veille, etc.) : seek immédiat sans attendre
  la médiane.
- Inchangés : seek de départ (VS_START_SEEK_SEC), recalage en pause
  (VS_PAUSED_SEEK_SEC), grâce armée par pause/reprise/seek (fix VS-029),
  suspension buffering, détection d'action utilisateur.

## Critères d'acceptation

- [x] docs/protocol.md §Comportements client réécrit (source de vérité) :
      §Correction (VS-038) + §Persistance de la dérive, zone morte 1,5 s,
      seek immédiat 5 s, plus aucune mention de nudge/hystérésis
- [x] Référence Go alignée + tests (plus AUCUNE commande rate corrective ;
      restauration de la vitesse de référence conservée et prouvée par
      TestVitesseDeReferenceRestauree). Le faux VLC compte désormais les
      commandes `rate` reçues : c'est ce compteur qui prouve le zéro.
- [x] Vecteurs regénérés par le générateur Go — 02, 11 et 14 redessinés et
      renommés (02-micro-seek-avance, 11-zone-morte-micro-seek,
      14-action-utilisateur-sous-bruit) ; 03/06/12 perdent leur `rate` 1,05
      d'affinage post-seek, les 8 autres sont inchangés
- [x] Moteur C porté, 14/14 vecteurs des deux côtés, asan+ubsan mac verts
      (995 checks mac, 1594 checks VM)
- [x] Séance réelle macOS PASS 11/11 + séance réelle VM 2 clients C PASS 14/14
      (dont vlcrc façon Syncplay et VLC préexistant)
- [x] Confort vérifié : 0 commande rate côté client 1 comme client 2 dans les
      DEUX séances réelles (compteurs `rateCmds` publiés par les deux modes
      auto), harnais réels réalignés sur la zone morte (écart toléré 1,5 s)

## Journal du ticket

- 2026-08-08 : créé (retour terrain v0.2.2, recherche plateformes, décision B).
- 2026-08-08 : implémenté et validé. Spec d'abord, puis Go (référence), puis
  vecteurs, puis C, puis Swift/UI. Le badge « ajustement de vitesse » disparaît
  des deux UI (VS_CORRECT_NUDGE retiré de l'enum ; le badge Swift devient
  « recalage », l'UI Win32 n'affichait qu'un booléen). Deux e2e Go ont dû être
  réalignés sur la zone morte : ils exigeaient une convergence fine (0,2 s /
  0,3 s) que le moteur ne cherche plus — seuils portés à `client.DeadZoneSec`,
  comme les deux harnais de séance réelle (0,5 → 1,5 s). Rapport :
  docs/research/2026-08-08-vs038-implementation.md.
