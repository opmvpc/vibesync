# VS-037 — vibesync.ini écrit atomiquement (temporaire + MoveFileExW)

Date : 2026-08-08 · Fichiers : `ui/win32/src/ini_win32.c`, `ui/win32/src/test_win32.c`

## Le problème

`ini_save_file` faisait `CreateFileW(CREATE_ALWAYS)` + `WriteFile`. Le
`CREATE_ALWAYS` tronque AVANT la première écriture : un crash, une coupure de
courant ou un disque plein au milieu laissait un `vibesync.ini` vide ou
tronqué, c'est-à-dire réglages, jeton de session et mot de passe chiffré perdus
d'un coup. Le durcissement f73eae9 rendait l'échec visible ; il ne l'évitait
pas.

## Approche retenue

Même pattern que `auto_write_atomic()` (auto.c, commit d12450d) :

1. **Temporaire `<path>.tmp-<pid>`, dans le MÊME répertoire que la cible.**
   Même répertoire parce que `MoveFileExW` n'est atomique qu'à l'intérieur d'un
   volume (entre volumes il copie). Suffixe `pid` pour que deux vibesync
   lancés en parallèle — ou la suite de tests pendant que l'appli tourne — ne
   se marchent pas dessus.
2. **`FlushFileBuffers` avant `CloseHandle`.** Pour : sans lui le renommage
   peut être validé par le système de fichiers alors que le contenu du
   temporaire est encore en cache — une coupure de courant laisse alors un
   `vibesync.ini` de zéros, exactement le désastre que le ticket supprime.
   Contre : un aller-retour disque (~ms). Arbitrage : le fichier fait 2 Ko et
   n'est écrit qu'aux gestes de l'utilisateur (connexion, jeton, changement de
   dossier), jamais en boucle — la durabilité gagne. Un échec de flush est
   traité comme un échec d'écriture (le disque plein se manifeste souvent
   là) : on ne bascule pas.
3. **Bascule `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`, 5 tentatives espacées de
   20 ms.** Le remplacement échoue si quelqu'un tient l'ancien fichier ouvert
   sans `FILE_SHARE_DELETE` (antivirus qui analyse `%APPDATA%`, Bloc-notes
   resté ouvert). C'est une course, pas une erreur : on réessaie brièvement,
   exactement comme `auto_write_atomic`.
4. **Nettoyage.** `DeleteFileW` du temporaire sur CHAQUE chemin d'échec
   (écriture partielle, flush refusé, bascule impossible) : aucun déchet ne
   s'accumule dans `%APPDATA%`.
5. **Retour inchangé** : `0` en cas d'échec, remonté tel quel à
   `ini_flush_notify` (main.c) qui journalise et affiche le toast une fois par
   session. `main.c` n'a pas été touché.

La journalisation de f73eae9 est conservée intégralement, découpée par étape :
création du temporaire refusée (code 3 / 5), écriture incomplète (112), vidage
sur disque refusé, bascule refusée (5 / 32) — avec dans les trois derniers cas
la mention explicite « laissé intact » / « conservé intact » sur le chemin de
la cible.

## Tests (7 checks ajoutés, section `ini` de test_win32.c)

L'échec est forcé au dernier moment, sur la bascule : on écrit un ini de
référence, puis on ouvre la cible en `GENERIC_READ | FILE_SHARE_READ` (donc
sans `FILE_SHARE_DELETE`) — ce que fait un antivirus. Vérifié :

- l'ini de référence s'écrit ;
- `ini_save_file` renvoie 0 quand la bascule est impossible ;
- le fichier sur disque est **identique octet pour octet** à la référence
  (relu brut via `auto_read_text`, pas via le parseur) ;
- aucun temporaire orphelin (`FindFirstFileW` sur `<path>.tmp*`, sans dépendre
  de la forme du suffixe) ;
- verrou levé, l'écriture repasse (aucun état collant) et c'est bien le nouveau
  contenu qui atterrit.

Les 6 checks du durcissement sont inchangés ; le commentaire du cas
« répertoire en guise de fichier » a été corrigé (depuis VS-037 c'est la
bascule qui refuse, plus l'ouverture — code Win32 5, vérifié dans le journal de
la suite).

## Validation (VM Win11 ARM64, clone jetable, supprimé après coup)

- `ui\win32\build.bat test` : **1542 vérifications, 0 échec** (1535 avant + 7),
  **14/14 vecteurs** de conformité. Deux exécutions consécutives, identiques.
- `ui\win32\build.bat` (release, `main.c` sous `-Werror`) : OK,
  **265 216 octets** — budget 500 Ko respecté.
- Journal de la suite conforme aux nouveaux messages :
  `erreur Win32 3` sur répertoire absent (création du temporaire),
  `erreur Win32 5` sur les deux bascules impossibles.
- Aucun `vibesync*` résiduel dans `%TEMP%` après les runs.
- ASan non rejoué : documenté impossible sur Windows ARM64 (0c0cd16).

## Points de review

1. **ACL de la cible.** `MOVEFILE_REPLACE_EXISTING` fait hériter au fichier
   final l'ACL du temporaire (héritée du répertoire), pas celle de l'ancien
   `vibesync.ini`. Pour `%APPDATA%` c'est identique à ce qu'un `CREATE_ALWAYS`
   sur fichier neuf donnait ; seul un utilisateur ayant durci l'ACL du fichier
   à la main la perdrait. Jugé acceptable ; à savoir si un jour on veut
   restreindre explicitement l'accès au jeton.
2. **`MOVEFILE_WRITE_THROUGH` non utilisé** (volontaire). Il rendrait le
   renommage lui-même durable avant retour ; sans lui, le pire cas après une
   coupure reste « l'ancien fichier est toujours là », jamais un fichier
   corrompu. Un flush de plus pour rien, et `auto_write_atomic` ne le met pas
   non plus.
3. **Suffixe `pid` et non `pid+horodatage`** : deux écritures simultanées du
   MÊME processus sont impossibles (ini_flush est mono-thread UI, seul point
   d'écriture). Si cela changeait, le suffixe devrait le suivre.
4. **Le retry de 5 × 20 ms bloque l'UI jusqu'à 80 ms** dans le cas dégradé
   (antivirus). Identique à `auto_write_atomic`, jamais atteint en pratique ;
   c'est le prix pour ne pas perdre l'écriture sur une course.
5. **macOS non concerné** : `ini_save_file`/`ini_path` n'ont d'implémentation
   que dans `ini_win32.c` (rien dans `core/posix`), le client Swift persiste
   par `UserDefaults` + trousseau (`ui/macos/Sources/VibeSync/UI/Preferences.swift`).
   Si un pendant POSIX apparaît un jour, le même contrat (temporaire +
   `rename(2)` + `fsync`) devra y être écrit.
