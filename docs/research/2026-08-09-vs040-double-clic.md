# VS-040 — double-clic sur le fichier d'un participant

2026-08-09. Sur la base 335cceb (VS-043). Aucun commit fait par l'agent.

## Ce qu'il fallait faire, et ce qu'il ne fallait pas faire

Le ticket demande un DÉCLENCHEUR, pas un mécanisme : la recherche d'un nom de
fichier dans les dossiers médias existe depuis VS-026 (`core/src/media_core.c`,
bornée en profondeur et en entrées, hors thread d'interface, homonymes tranchés
par la taille), et le bandeau « X regarde … » l'appelle déjà. Il s'agissait donc
d'ajouter un second chemin d'entrée vers le MÊME appel, sans toucher au moteur
de synchronisation ni aux vecteurs.

Constat en ouvrant le code : **le client Windows avait déjà le double-clic**
(livré avec VS-026, `act_open_user_file` / `act_open_user_index`), le client
macOS ne l'avait pas du tout. Le travail était donc asymétrique :

- macOS : câbler le déclencheur ;
- Windows : compléter les cas limites, dont un manquait.

## La règle, une seule fois par client

Le point de conception : « la ligne de ce participant mène-t-elle quelque part
quand on la double-clique ? » est une question à trois clauses (ni nous, fichier
déclaré, différent du nôtre). Elle était sur le point d'exister en trois
exemplaires par client — l'affordance de survol, l'action, et le bandeau qui
sélectionne le fichier à proposer. Elle est désormais une fonction :

- macOS : `AppModel.participantFileToOpen(user:selfId:myFile:) -> String?`
  (retourne le nom à chercher, ou nil). `refreshWatchBanner()` l'appelle aussi
  et n'ajoute plus que son propre refus (`dismissedWatchFile`).
- Windows : `b32 ui_user_openable(const UiUser *u)` dans ui.c, déclarée dans
  ui.h. Appelée par `draw_users` (surbrillance), par `handle_actions`
  (act_open_user_file) et par `refresh_watch_banner`.

Les deux implémentations comparent les noms **insensiblement à la casse**, comme
la recherche elle-même et comme le bandeau le faisait déjà.

## macOS — ce qui est câblé

`ui/macos/Sources/VibeSync/UI/AppModel.swift`

- `openWatchedFile()` ne contient plus de logique : son corps est extrait en
  `private func searchAndOpenMedia(named:)`, au bit près (garde
  `mediaSearching`, garde « aucun dossier configuré », jeton
  `mediaSearchGen`, trace des homonymes sur stderr, bandeau « introuvable »).
  Les deux déclencheurs partagent ce corps — c'est littéralement le même chemin,
  comme demandé.
- `participantFileToOpen` (statique, pure) porte les trois cas limites.
- `canOpenFile(of:)` pour l'affordance, `openParticipantFile(_:)` pour l'action.
- `myFileName` : notre fichier lu dans le MOTEUR (`engine.haveFile/fileName`),
  pas dans le miroir de la vue — c'est la leçon de VS-039, et `refreshWatchBanner`
  l'appliquait déjà.

`ui/macos/Sources/VibeSync/UI/RoomView.swift`

- La ligne devient `ParticipantRow`, une vue à part : la surbrillance au survol
  demande un `@State` par ligne, impossible dans un `HStack` inline. Même
  affordance que Windows (fond teinté + nom de fichier en couleur d'accent au
  survol, uniquement si la ligne est actionnable), plus une infobulle `.help`
  qui dit ce que le double-clic va faire.
- `.onTapGesture(count: 2)` sur toute la ligne (`.contentShape(Rectangle())`),
  pas seulement sur le texte du nom de fichier : parité avec Windows, où la
  zone cliquable est la ligne entière.

## Windows — ce qui est câblé

Le déclencheur existait ; il lui manquait le troisième cas limite et une
affordance honnête.

- `ui.h` : nouveau champ `UiUser.same_file` (« ce participant regarde déjà notre
  fichier ») et déclaration de `ui_user_openable`.
- `main.c` : `refresh_user_files(App *)` calcule `same_file` pour toutes les
  lignes en comparant au fichier du moteur via `vs_dir_ops()->name_eq_ci` —
  le même comparateur que la recherche. Appelée depuis `refresh_view` (donc à
  chaque pas : ouvrir NOTRE fichier éteint immédiatement les lignes devenues
  identiques, sans attendre un `users` du serveur) ET en tête de
  `refresh_watch_banner` (qui, lui, tourne juste après `fill_users`, avant le
  prochain `refresh_view`).
- `ui.c` : `row_hover` passe de `!is_self && has_file` à `ui_user_openable(u)`.
- `main.c` : la garde de `act_open_user_file` passe de `has_file` à
  `ui_user_openable`. Revérifier au traitement n'est pas redondant : la liste
  peut avoir changé entre le dessin de la frame et la consommation de l'action.
- `refresh_watch_banner` perd sa copie de la règle et gagne trois lignes de
  moins ; son comportement est inchangé (mêmes clauses, même ordre, même
  `return` sur refus).
- Fixture `--capture` : `users[1].same_file = 1` (camille a le même fichier que
  nous dans la scène) — la capture reste ainsi une image d'un état cohérent.

## Ce qui n'a PAS été touché

`core/` (moteur, media_core, protocole), `internal/` (client Go de référence),
le serveur, les vecteurs `test/vectors/*.json`. Le protocole ne bouge pas : les
noms de fichiers circulaient déjà dans `users`.

## Validations

macOS (ce Mac) :

- `swift build` + `swift test` : **61/61**, 0 échec (55 avant, +6 nouveaux)
- `./scripts/test-core-macos.sh` : **1048 vérifications**, 0 échec, vecteurs 15/15
- `./scripts/build-macos.sh` : bundle 1,5 Mo, binaire 1 233 760 o
- séance réelle 2 clients contre la prod
  (`VIBESYNC_PASSWORD=onlyvibes ./scripts/run-real-macos.sh "" wss://vibesync.choboai.com/ws`) :
  **RÉSULTAT : PASS — 18 points OK, 0 échec**, dont (h) changement de fichier en
  cours de salle (VS-039) toujours vert et 0 pause automatique parasite

Windows (VM Win11 ARM64 par SSH, clone jetable `work-vs040` cloné depuis
GitHub à 335cceb + patch des 4 fichiers, supprimé après) :

- `build.bat test` : **1652 vérifications**, 0 échec, vecteurs **15/15**
  (1647 avant, +5 nouveaux)
- `build.bat` (release) : **270 336 octets = 264 Ko**, 53 % du budget 500 Ko —
  identique à l'octet près à la version d'avant le ticket
- `build.bat capture` : PNG de la salle relue, liste des participants intacte

L'autopilot ne clique pas : le double-clic lui-même se valide par la
compilation, par les tests de la règle et par la revue. Ce qui est réellement
prouvé automatiquement, c'est que le chemin appelé (recherche + ouverture) est
le même que celui du bandeau, lequel EST exercé par la séance réelle au point
(h).

## Points de review

1. **Partage du corps de recherche (macOS).** `searchAndOpenMedia` remet
   `showWatchBanner = false` à la fin, succès comme échec. Hérité du bandeau, et
   volontairement conservé pour que le chemin reste identique. Conséquence : si
   le bandeau propose le fichier de A et qu'on double-clique la ligne de B, un
   échec de recherche sur B fait aussi disparaître le bandeau de A. C'est
   transitoire — `refreshWatchBanner` le relève au prochain `users`, puisque
   `dismissedWatchFile` n'est pas touché. Si on préfère l'éviter, il faut cesser
   de partager le corps, ce que le ticket demandait explicitement.
2. **`refreshWatchBanner` / `refresh_watch_banner` réécrits.** VS-039 vient de
   corriger ces deux fonctions ; elles sont retouchées ici pour lire la règle
   partagée. La transformation est censée être une identité (`for … where A && B
   && C` + `if same continue` → `for …` + `guard eligible else continue`) —
   à relire à la loupe, c'est le seul endroit où j'ai touché du code chaud.
   Preuve indirecte : la séance réelle (h), qui est exactement le scénario que
   ces fonctions couvrent, reste PASS.
3. **`refresh_user_files` appelée deux fois par tour** (dans `refresh_view` et
   en tête de `refresh_watch_banner`). C'est délibéré — les deux points de
   lecture n'ont pas le même moment — mais c'est un coût : N comparaisons de
   noms par pas, N ≤ 16. Négligeable, à confirmer si la salle grossit.
4. **Zone cliquable = la ligne entière**, alors que le ticket dit « double-cliquer
   le nom de fichier ». J'ai choisi la parité avec Windows (déjà la ligne) et la
   facilité de visée. À arbitrer avec Thibault.
5. **Pas de garde `media_searching` côté Windows.** macOS refuse un second
   déclenchement pendant une recherche en vol ; Windows, lui, écrase
   `pending_find` et relance `worker_find`. Ce n'était pas vrai non plus avant ce
   ticket (le bandeau a le même trou) et le double-clic ne l'aggrave pas — mais
   maintenant qu'il y a deux déclencheurs, l'écart entre les clients mérite un
   ticket à part.
