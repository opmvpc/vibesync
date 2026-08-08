# Bouton « Parcourir… » pour le chemin de VLC (client Windows)

*2026-08-08 — sous-agent. Rapport, pas de bureaucratie : STATUS/journal/tickets
restent à la main de l'orchestrateur.*

## Le manque

Le panneau Réglages a un champ « Chemin de VLC (vide = détection automatique) »
et un bouton « Détecter ». Mais « Détecter » se **grise** quand la détection
automatique n'a rien trouvé — c'est-à-dire exactement dans le cas où
l'utilisateur voit « Aucun VLC détecté sur cette machine : indiquez le chemin de
vlc.exe. » et a besoin d'aide. Il ne lui restait qu'à taper le chemin à la main.

## Ce qui a été câblé

### Le choix du dossier d'ouverture, en fonction pure

`vlc_browse_initial_dir` (`core/src/vlc_core.c`, déclarée dans
`core/include/vlc.h`) décide où ouvrir le sélecteur :

1. le chemin déjà saisi s'il désigne un répertoire existant ;
2. sinon le répertoire qui le contient s'il existe (cas normal :
   `…\VLC\vlc.exe` → `…\VLC`) ;
3. sinon `<ProgramFiles>\VideoLAN\VLC` ;
4. sinon `<ProgramFiles>` ;
5. sinon chaîne vide — à Windows de décider.

Le test d'existence est **injecté** (`VlcDirExistsFn`, un `b32 (*)(void *, Str8)`) :
la fonction ne touche ni au disque ni à Win32, donc elle se teste. Elle vit dans
la moitié portable du core (aucun `windows.h`, aucun `wchar_t`) et accepte les
deux séparateurs, comme Windows.

Deux pièges traités dans `path_parent` : `C:\vlc.exe` rend `C:\` et non `C:`
(qui désigne le *répertoire courant du lecteur*, pas sa racine), et `\foo` rend
`\`. Un `program_files` déjà terminé par un séparateur ne se voit pas doubler le
sien.

### La boîte de dialogue

`browse_vlc_path` (`ui/win32/src/main.c`) appelle `GetOpenFileNameW` :

- filtre `vlc.exe\0vlc.exe\0Exécutables (*.exe)\0*.exe\0\0` (premier filtre
  actif : c'est le seul fichier que ce champ accepte ; le second sert au VLC
  portable renommé) ;
- `lpstrInitialDir` = le résultat de `vlc_browse_initial_dir`, et `lpstrFile`
  pré-rempli avec le chemin courant pour que la boîte s'ouvre *sur* le fichier
  déjà configuré ;
- `OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR`.
  `OFN_NOCHANGEDIR` n'est pas cosmétique : sans lui la boîte laisse le
  répertoire **courant du processus** là où l'utilisateur a navigué, et tout
  chemin relatif manipulé ensuite part ailleurs ;
- tampon de **32768 unités UTF-16** (64 Ko sur l'arène de travail, libérés à la
  sortie) : `MAX_PATH` ferait échouer la boîte sur un fichier pourtant valide.

Appelée depuis `handle_actions`, donc depuis le thread UI — c'est déjà l'endroit
d'où `pick_folder` et `open_file_dialog` ouvrent leurs boîtes modales. La boîte
pompe sa propre boucle de messages ; le `WM_PAINT` en cours se rejoue une fois
en imbriqué et se termine normalement, comportement inchangé par rapport à
« Ajouter un dossier… ».

Au retour : UTF-16 → UTF-8 (`utf16_to_utf8`), `ui_text_set(&ui->f_set_vlc, …)`,
et `settings_validate()` — déjà appelée en fin de `handle_actions` — rafraîchit
le voyant exactement comme après une frappe. « Aucun VLC détecté… » devient donc
« vlc.exe trouvé à ce chemin. » sans code supplémentaire.

### Le bouton

`ui/win32/src/ui.c`, `screen_settings` : `ID_SET_BROWSE`, `BTN_GHOST`, **toujours
actif**, entre le champ et « Détecter ». Le champ passe de `w - 96 - 8` à
`w - 92 - 100 - 16` (304 dp : la fenêtre a une largeur minimale de 900 dp et la
carte est figée à 560 dp, la largeur du champ est donc constante et jamais
négative). Nouveau drapeau `act_settings_browse` dans `ui.h`.

### Chemin trop long

Le champ (`UiText`) est plafonné à `UI_TEXT_CAP` = 320 octets. Un chemin choisi
qui n'y tient pas est **refusé** avec « Ce chemin est trop long pour le champ
(319 octets au plus). » plutôt qu'inséré tronqué : un chemin tronqué ne désigne
rien et serait rejeté à l'enregistrement avec un message incompréhensible.

### `build.bat`

`-lcomdlg32` ajouté à `LIBS`. API système livrée avec Windows, même famille que
`shell32`/`ole32` déjà liées — pas une dépendance tierce (ADR-008).

## Un piège trouvé au passage : build.bat, LF et non-ASCII

Le commentaire ajouté au-dessus de `LIBS` contenait `«`, `»` et `…`. Résultat
dans la VM : `'lg32' n'est pas reconnu`, `'SION' n'est pas reconnu`, `'-I' n'est
pas reconnu` — cmd.exe **se repère par offset d'octet** dans un fichier de
commandes, et le fichier est en LF pur : chaque caractère non ASCII lui fait
perdre la ligne suivante. Le fichier était d'ailleurs déjà écrit sans accents,
mais gardait un tiret cadratin ligne 2 qui produisait un `'m' n'est pas reconnu`
bénin à chaque `:release`.

`build.bat` est maintenant **ASCII pur** (tiret cadratin ligne 2 compris) et le
dit dans un commentaire. Le message parasite a disparu, ce qui confirme le
diagnostic.

## Tests

`core/tests/test_core.c`, section `vlc` : `browse_fs_has_dir` est un faux système
de fichiers (liste de répertoires terminée par NULL) passé comme prédicat, et une
table de 12 cas + 1 vérification de durée de vie couvre chemin saisi → dossier
parent, champ déjà répertoire, espaces autour du chemin collé, séparateurs `/`,
champ vide avec et sans VLC installé, dossier saisi inexistant, nom de fichier
sans dossier, racine du lecteur, `program_files` terminé par un séparateur,
aucun candidat, tout vide.

Le test vit dans la moitié **portable** de la suite : il tourne dans les deux
harnais (Windows et macOS). La boîte modale elle-même n'est pas testable en
headless.

## Validation

| Où | Quoi | Résultat |
|---|---|---|
| macOS (local) | `scripts/test-core-macos.sh` (asan+ubsan) | 972 vérifications, 0 échec (959 avant, +13) |
| VM Win11 ARM64 | `ui\win32\build.bat test` | 1571 vérifications, 0 échec |
| VM Win11 ARM64 | `ui\win32\build.bat` (release, `-Werror`) | 268 288 octets, budget < 500 Ko tenu |

Clone jetable `C:\Users\OPMVPC\work-browse` + archive supprimés après coup.

## Points de review

1. **Deux mécanismes de sélecteur cohabitent désormais.** `open_file_dialog` et
   `pick_folder` utilisent `IFileOpenDialog` (COM, déjà lié) ; `browse_vlc_path`
   utilise `GetOpenFileNameW` comme demandé. Les deux donnent la même boîte
   Vista sur Windows moderne. À unifier si la duplication gêne — dans un sens
   comme dans l'autre.
2. **`UI_TEXT_CAP` = 320 octets plafonne le champ**, pas le tampon du sélecteur.
   Le tampon de 32768 wchar évite l'échec côté Windows, mais un chemin de plus de
   319 octets UTF-8 reste refusé par le champ. Relever `UI_TEXT_CAP` (ou donner
   au chemin VLC un stockage à part) est une décision produit, pas faite ici.
3. **`ProgramFiles` seul** est consulté pour le repli, pas `ProgramFiles(x86)`
   ni `ProgramW6432` (contrairement à `vlc_locate`). Un VLC 32 bits sur système
   64 bits ouvrira donc la boîte sur `C:\Program Files` et non
   `C:\Program Files (x86)`. Facile à étendre : la fonction est pure et testée.
4. **Le bouton n'a pas d'accélérateur clavier** et n'entre pas dans un ordre de
   tabulation explicite (l'UI immediate-mode donne le focus au clic). Cohérent
   avec « Détecter » et « Ajouter un dossier… ».
5. **`build.bat` ligne 2 modifiée** (tiret cadratin → deux-points) hors du strict
   périmètre : c'était la cause d'un message d'erreur parasite à chaque build
   release, et le commentaire ajouté juste en dessous documente la contrainte.
