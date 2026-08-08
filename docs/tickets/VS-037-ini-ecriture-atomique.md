---
id: VS-037
titre: vibesync.ini — écriture atomique (temporaire + MoveFileExW)
statut: ouvert
priorité: basse
dépend-de: []
créé: 2026-08-08
mis-à-jour: 2026-08-08
---

## Contexte

Le durcissement ini_flush (f73eae9) rend les échecs d'écriture VISIBLES
(journal + toast une fois par session) mais ne les évite pas : `ini_save_file`
fait `CreateFileW(CREATE_ALWAYS)` puis `WriteFile` — un crash, une coupure ou
un disque plein au milieu laisse un `vibesync.ini` TRONQUÉ (le CREATE_ALWAYS a
déjà vidé le fichier). Réglages, jeton de session et mot de passe chiffré
perdus d'un coup.

## Critères d'acceptation

- [ ] Écriture dans un fichier temporaire du même répertoire puis bascule
      `MoveFileExW(MOVEFILE_REPLACE_EXISTING)` — le pattern existe déjà dans
      `ui/win32/src/auto.c` (publication d'état, avec retry sur course) :
      s'aligner dessus.
- [ ] En cas d'échec de la bascule : l'ancien ini reste intact (c'est tout
      l'intérêt), l'échec passe par `ini_flush_notify` comme aujourd'hui.
- [ ] Test : simuler l'échec (répertoire du temporaire en lecture seule ou
      chemin invalide) et vérifier que le fichier d'origine n'est pas modifié.
- [ ] `build.bat test` vert (VM ou CI), pas de régression des 6 checks du
      durcissement.

## Journal du ticket

- 2026-08-08 : créé sur recommandation du rendu ini_flush
  (docs/research/2026-08-08-ini-flush-durcissement.md, point de review n°1).
