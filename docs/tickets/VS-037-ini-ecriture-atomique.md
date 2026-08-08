---
id: VS-037
titre: vibesync.ini — écriture atomique (temporaire + MoveFileExW)
statut: terminé
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

- [x] Écriture dans un fichier temporaire du même répertoire puis bascule
      `MoveFileExW(MOVEFILE_REPLACE_EXISTING)` — le pattern existe déjà dans
      `ui/win32/src/auto.c` (publication d'état, avec retry sur course) :
      s'aligner dessus.
- [x] En cas d'échec de la bascule : l'ancien ini reste intact (c'est tout
      l'intérêt), l'échec passe par `ini_flush_notify` comme aujourd'hui.
- [x] Test : simuler l'échec (répertoire du temporaire en lecture seule ou
      chemin invalide) et vérifier que le fichier d'origine n'est pas modifié.
- [x] `build.bat test` vert (VM ou CI), pas de régression des 6 checks du
      durcissement.

## Journal du ticket

- 2026-08-08 : créé sur recommandation du rendu ini_flush
  (docs/research/2026-08-08-ini-flush-durcissement.md, point de review n°1).
- 2026-08-08 : terminé. `ini_save_file` écrit désormais dans
  `<path>.tmp-<pid>` (même répertoire — MoveFileExW n'est atomique que dans un
  volume), `FlushFileBuffers` avant fermeture, puis bascule
  `MOVEFILE_REPLACE_EXISTING` avec 5 tentatives espacées de 20 ms (course
  antivirus/éditeur, comme `auto_write_atomic`). Le temporaire est supprimé sur
  chaque chemin d'échec ; la journalisation du durcissement f73eae9 est
  conservée et découpée par étape (création du temporaire / écriture
  incomplète / vidage disque / bascule refusée). 7 checks ajoutés dans
  `test_win32.c` : cible verrouillée sans FILE_SHARE_DELETE → échec, fichier
  d'origine identique octet pour octet, aucun temporaire orphelin, écriture de
  nouveau possible après le déverrouillage. Validé dans la VM Win11 sur un
  clone jetable : `build.bat test` 1542 vérifications / 0 échec (1535 + 7),
  14/14 vecteurs, deux exécutions ; release `-Werror` OK, 265 216 octets
  (< 500 Ko). Rendu : `docs/research/2026-08-08-vs037-ini-atomique.md`.
