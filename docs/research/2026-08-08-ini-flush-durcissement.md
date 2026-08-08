# Durcissement : retours de `ini_flush` ignorés (client Windows)

2026-08-08 — mission de durcissement listée dans `docs/STATUS.md`.

## Symptôme visé

Une écriture de `%APPDATA%\vibesync.ini` qui échoue (disque plein, ACL d'un profil
bricolé, antivirus ou éditeur qui verrouille le fichier) était **silencieuse** :
réglages et jeton de session perdus sans une ligne de journal ni un mot à
l'utilisateur. Sur le terrain, ça se raconte « il me redemande tout à chaque
lancement ».

## Call sites inventoriés

`ini_flush()` (main.c) est le seul point d'écriture ; `ini_save_file()`
(ini_win32.c) est la seule primitive disque. État AVANT :

| Site | Appel | Retour |
| --- | --- | --- |
| `settings_save()` | `ini_flush` | **ignoré** (fonction `void`) |
| `wm_actions` — ajout dossier média | `ini_flush` | **ignoré** |
| `wm_actions` — retrait dossier média | `ini_flush` | **ignoré** |
| `session_load()` | `ini_flush` | traité (`vs_log`) |
| `settings_apply()` | `ini_flush` | traité (message du panneau) |

`settings_save()` étant `void`, ses 4 appelants perdaient l'information à leur
tour : connexion (`do_connect`), bascule « retenir le mot de passe », `WM_CLOSE`,
sortie de `wWinMain`. La bascule « retenir le mot de passe » était le pire cas :
elle affichait « Mot de passe oublié. » alors que le chiffré était toujours sur
le disque.

Note : le retry `MoveFileExW` du commit d12450d est dans `auto.c` (mise à jour
automatique), pas dans le chemin ini — rien à reprendre de ce côté.

## Traitement retenu

1. **`ini_save_file()` journalise la cause technique** (ini_win32.c) : code
   Win32 de `CreateFileW` (3 = chemin introuvable, 5 = accès refusé, 32 =
   fichier verrouillé, 112 = disque plein) et cas de l'écriture incomplète, qui
   laisse un fichier tronqué par le `CREATE_ALWAYS`. Le chemin vide
   (`%APPDATA%` introuvable) est signalé au lieu d'un `return 0` muet. La couche
   plateforme est la seule à connaître le code d'erreur ; elle ne décide rien
   d'autre.
2. **`ini_flush_notify(app, quoi, toast)`** (main.c) remplace les appels nus :
   journal à **chaque** échec, toast **une seule fois par session**
   (`App.ini_write_toasted`) — `ini_flush` part à chaque geste, un disque plein
   les ferait tous échouer et l'utilisateur verrait le même toast en boucle.
   Niveau 1 (avertissement), non bloquant, aucune boîte de dialogue, aucun
   retry ajouté.
3. `toast = 0` là où il ne serait pas lu ou ferait doublon : panneau Réglages
   (message déjà affiché sous le bouton), `WM_CLOSE` et sortie de `wWinMain`
   (la fenêtre disparaît). Le journal, lui, est écrit dans tous les cas.
4. `settings_save()` renvoie `b32` et prend le drapeau `toast`. La bascule
   « retenir le mot de passe » remplace son toast de succès par un message
   d'échec explicite plutôt que de mentir.

Pattern conforme à l'existant : `ui_toast(..., 1, now_ms())` est déjà ce que fait
`session_load()` pour l'ini saturé, et `vlc_win32.c` journalise déjà ses échecs
avec le détail technique.

## Portable / macOS

`core/src/ini_core.c` ne touche **pas** au disque (analyse, édition et
sérialisation en mémoire uniquement — ADR-010) : aucun retour d'écriture à
propager côté portable. Le client macOS ne lit ni n'écrit d'ini : il persiste
dans `UserDefaults` + trousseau (`ui/macos/Sources/VibeSync/UI/Preferences.swift`,
`Net/Keychain.swift`). **Aucun fichier de `core/` n'a été modifié**, donc pas de
revalidation macOS nécessaire.

## Test ajouté

`ui/win32/src/test_win32.c`, section `ini` (+6 vérifications) : trois écritures
qui doivent échouer — répertoire inexistant, chemin vide, répertoire donné en
guise de fichier (le plus proche d'un « accès refusé » qu'on puisse provoquer
sans droits) — puis la preuve qu'aucun fichier n'a été créé au passage et que
l'écriture suivante repart normalement (aucun état collant, aucun crash).

## Validation

VM Win11 ARM64 par SSH, clone jetable `C:\Users\OPMVPC\work-ini` (supprimé
depuis), fichiers modifiés poussés par scp :

- `ui\win32\build.bat test` : **1535 vérifications, 0 échec**, 14/14 vecteurs.
  Référence sur le même clone sans le patch : 1529 — soit exactement les 6
  vérifications ajoutées (le total varie de quelques unités d'un run à l'autre,
  les boucles de stress réseau comptent leurs itérations).
- `ui\win32\build.bat` (release) : compile `main.c` sous `-Werror`, 265 216
  octets — le mode `test` ne compile pas `main.c`, cette cible est donc
  indispensable pour couvrir le patch.
- Smoke d'exécution réelle : `vibesync.exe --capture` avec
  `APPDATA=C:\Users\OPMVPC\pas-de-profil` (inexistant). Journal obtenu :
  `ini: ouverture en écriture refusée (erreur Win32 3) — "…\vibesync.ini"` puis
  `ini: jeton de session non enregistré — vibesync.ini n'a pas pu être écrit
  (disque plein, droits d'accès ou fichier verrouillé ?)`. Code de sortie 0,
  captures produites : l'échec est visible et ne bloque rien.
- Les mêmes lignes apparaissent dans `%APPDATA%\vibesync.log` pendant la suite de
  tests (codes 3 et 5), ce qui vérifie le trajet complet jusqu'au journal.

ASan non rejoué : documenté impossible sur Windows ARM64 (émulation, pas de
runtime aarch64).

## Points de review

- **Écriture non atomique** (hors périmètre, à arbitrer) : `ini_save_file` fait
  `CREATE_ALWAYS` puis `WriteFile`. Une panne en cours d'écriture laisse un
  `vibesync.ini` tronqué — désormais journalisé, mais pas évité. Le remède
  classique est temporaire + `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`, exactement
  ce que fait déjà `auto.c` pour l'exe. Diff plus lourd, à traiter comme un
  ticket à part.
- Le toast une-fois-par-session ne se réarme jamais : si l'utilisateur libère de
  la place, le succès suivant ne le lui dit pas (il n'y a rien à dire) mais un
  second échec plus tard ne re-toastera pas non plus. Choix assumé contre le
  spam ; réarmer sur succès serait une ligne si on le préfère.
- Le toast est posé même quand aucune fenêtre n'existe encore (`session_load`
  s'exécute avant `ShowWindow`) : sans conséquence, l'état est simplement
  affiché au premier rendu, comme le toast « ini saturé » déjà présent.
