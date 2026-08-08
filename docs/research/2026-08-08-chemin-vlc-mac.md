# Chemin de VLC réglable sur le client macOS (parité Windows)

*2026-08-08 — sous-agent. Rapport, pas de bureaucratie : STATUS/journal/tickets
restent à la main de l'orchestrateur.*

## Le manque

Le client Windows a, dans ses Réglages, un champ « Chemin de VLC (vide =
détection automatique) » avec bouton « Détecter » et une ligne d'état
(`ui/win32/src/ui.c` ~1895, réglage `vlc` du vibesync.ini appliqué par
`apply_vlc_path` dans `main.c`). Le client macOS n'avait **rien** : VLC hors de
`/Applications` (ou `~/Applications`, `/opt/homebrew/bin`, `/usr/local/bin`,
`$PATH`) n'était atteignable que par la variable d'environnement
`VIBESYNC_VLC`, c'est-à-dire en lançant l'app depuis un Terminal. Pour un
« guide amis », ce n'est pas une solution.

## Ce qui a été câblé

### Réglage persistant

`Preferences.keyVLC = "vibesync.vlc"` + `vlcPath(_:)` / `setVLCPath(_:_:)`
(`ui/macos/Sources/VibeSync/UI/Preferences.swift`). Chemin rogné ; la chaîne
vide est une valeur légitime (« rends-moi la détection automatique ») et non un
effacement de clé, pour que la relecture rende exactement ce que le champ
affiche.

### Résolution (VLCLauncher.swift)

Ordre, identique au client Windows :

1. **réglage** non vide et menant à un exécutable ;
2. **`$VIBESYNC_VLC`** ;
3. emplacements standards, puis `$PATH`.

Le réglage passe donc **devant l'environnement**. Windows obtient le même ordre
autrement (le réglage écrase la variable au démarrage, un réglage vide
restituant la valeur héritée — cf. le bug corrigé dans VS-029) ; sur macOS rien
n'est écrit dans l'environnement du processus : `AppModel` passe le réglage à
`VLCLauncher.launch(filePath:setting:)`, qui reste sans état.

Fonctions nouvelles, toutes pures (prédicat `exists` injecté, environnement en
paramètre — donc testables sans disque) :

| Fonction | Rôle |
|---|---|
| `normalize(_:)` | rogne, développe `~`, retire les `/` finaux |
| `resolveBundle(_:)` | `…/VLC.app` → `…/VLC.app/Contents/MacOS/VLC` ; tout le reste inchangé |
| `settingBinary(_:exists:)` | binaire du réglage seul, `nil` si vide ou invalide |
| `locate(env:exists:)` | détection automatique (env + emplacements + PATH) — ignore le réglage |
| `binary(setting:env:exists:)` | l'ordre ci-dessus, `nil` si aucun VLC |
| `pathStatus(setting:env:exists:)` | ce qu'affiche la ligne d'état |

Points de conception :

- **Un réglage invalide n'est jamais fatal** : il est ignoré, la détection
  reprend la main, et c'est l'interface qui le dit en rouge. Le lancement d'un
  ami qui a bricolé son champ ne casse pas.
- **`$VIBESYNC_VLC` qui pointe dans le vide reste bloquant** : comportement
  d'origine de `locate()`, conservé — une consigne explicite ne doit pas être
  contournée en douce.
- **Bit d'exécution** : `isExecutableFile` vérifie désormais fichier régulier
  **ET** exécutable (avant : seulement « existe et n'est pas un dossier »). Un
  fichier non exécutable échouait plus loin dans `Process.run()` avec un message
  bien plus obscur.
- **Bundle renommé** (« VLC 3.app ») : le chemin nominal `Contents/MacOS/VLC 3`
  n'existe pas ; rattrapage par `Bundle(path:)?.executableURL` (le seul accès
  disque non injecté, et seulement en secours).

### Interface (SettingsView.swift + AppModel.swift)

Carte « VLC » au-dessus des dossiers médias : libellé « Chemin de VLC (vide =
détection automatique) », `TextField` (placeholder « Détection automatique »),
bouton **Parcourir…**, puis la ligne d'état. Recalcul **à la frappe**
(`onChange`), **au retour du panel**, et **à l'ouverture du panneau**
(`onAppear` — VLC a pu être installé depuis le lancement).

`VLCPathStatus` (enum + `severity`) porte les quatre cas, la vue ne fait que
peindre :

| Cas | Texte | Couleur |
|---|---|---|
| réglage vide, détection OK | « VLC détecté : `<binaire>` » | secondaire |
| réglage vide, rien trouvé | « Aucun VLC détecté sur cette machine… » | orange |
| réglage valide | « VLC trouvé à ce chemin : `<binaire>` » | vert |
| réglage invalide | « VLC introuvable à ce chemin — corrigez-le, ou videz le champ… » | rouge |

`NSOpenPanel` : `canChooseFiles`, `canChooseDirectories = false`,
`treatsFilePackagesAsDirectories = false` (sans quoi on entre dans `VLC.app` au
lieu de le sélectionner), dossier initial `/Applications`,
`allowedContentTypes = [.application, .unixExecutable, .executable]`. Ce que
l'utilisateur choisit est **stocké tel quel** (`VLC.app` reste `VLC.app` dans le
champ, lisible et corrigeable) ; la traversée vers le binaire se fait au
lancement.

Enfin, `VLCError.notFound` disait « renseignez VIBESYNC_VLC » — il renvoie
maintenant vers les Réglages.

## Le harnais n'est pas cassé (point 4, vérifié)

Raisonnement demandé : `run-real-macos.sh` isole chaque instance par
`VIBESYNC_SUITE`, leur réglage est donc vide, et `$VIBESYNC_VLC` continue de
commander malgré l'ordre « réglage > env ». **Vérifié, avec deux surprises
levées au passage :**

1. **Une `UserDefaults` ouverte sur une suite ne voit pas le domaine applicatif
   normal.** Vérifié par sonde inter-processus (écriture dans un processus,
   lecture dans un autre) : clé présente seulement dans le domaine app →
   `UserDefaults(suiteName:)` rend `nil` ; clé présente dans les deux → la suite
   gagne. Un `vibesync.vlc` réglé dans le profil de l'utilisateur ne peut donc
   pas fuiter dans une instance du harnais.
2. **`open --env` ne remplace pas l'environnement, il l'ajoute** : une variable
   exportée dans le shell appelant EST héritée par l'app lancée par
   LaunchServices (vérifié avec un faux bundle qui recopie son environnement).
   `$VIBESYNC_VLC` posée avant l'appel du script atteint donc bien les deux
   instances — ce qui rend le point 4 réel et pas seulement théorique.

Conclusion : réglage vide côté harnais + env héritée ⇒ priorité inchangée pour
lui. Prouvé par une séance réelle passée **avec** `VIBESYNC_VLC=/Applications/VLC.app`
(voir validation) : cette valeur emprunte le chemin env **et** la nouvelle
traversée `.app` → binaire ; si l'une des deux était cassée, `locate()` rendrait
`nil` et aucun VLC ne démarrerait.

À noter pour plus tard : le script lui-même ne pose pas `VIBESYNC_VLC` (il exige
`/Applications/VLC.app` dès ses vérifications). S'il fallait un jour piloter un
VLC hors emplacement standard, ajouter `--env VIBESYNC_VLC=…` dans `launch()`
serait plus explicite que de compter sur l'héritage du shell.

## Validation

| Épreuve | Résultat |
|---|---|
| `swift build` | vert |
| `swift test` | **55/55** (45 avant + 10 nouveaux, `VLCPathTests.swift`) |
| `bash scripts/build-macos.sh` | bundle 1,5 Mo, signature ad hoc OK |
| `run-real-macos.sh` contre la prod | **PASS 10/10** |
| idem avec `VIBESYNC_VLC=/Applications/VLC.app` | **PASS 10/10** |

Les 10 tests nouveaux couvrent : résolution de bundle (pure), normalisation
(`~`, `/` finaux, blancs), réglage seul, priorité réglage > env > détection,
repli d'un réglage invalide, env pointant dans le vide, repli sur `$PATH`, les
quatre états d'affichage et leurs sévérités, l'aller-retour de préférence, et le
bit d'exécution sur vrai disque (dossier temporaire).

## Points de review

1. **`isExecutableFile` durci** — le bit d'exécution est maintenant exigé, y
   compris dans `locate()`. C'est le seul changement de comportement sur un
   chemin déjà existant. Jugé sûr (un VLC non exécutable ne se lance pas de
   toute façon), mais c'est le point à contester en priorité.
2. **Écriture d'un `UserDefaults` par frappe** (`onChange` → `setVLCPath`).
   Windows n'écrit son ini qu'au flush. Coût réel : négligeable (cfprefsd
   agrège), mais c'est un choix, pas une fatalité.
3. **`allowedContentTypes`** : vérifié à la compilation, pas à l'œil. Un binaire
   nu sans extension est censé être vu comme `public.unix-executable` (bit
   d'exécution) ; si un `/opt/homebrew/bin/vlc` s'avérait grisé dans le panel,
   il faudrait élargir (ou retirer le filtre). Le champ texte reste la porte de
   secours dans tous les cas.
4. **Aucune capture d'écran du panneau** : la séance réelle prouve le
   lancement, pas le rendu. Un coup d'œil humain sur Réglages → VLC (longueur du
   chemin affiché, retour à la ligne de la ligne d'état) reste à faire.
5. **Ticket** : rien n'a été créé dans `docs/tickets/` ; le prochain numéro libre
   est VS-038 si l'orchestrateur veut en ouvrir un.
6. **Docs mises à jour au passage** : `docs/guide-amis-macos.md` (la section
   « VLC introuvable » envoyait les amis vers un Terminal) et
   `docs/build-macos.md` (priorité réglage/env, plus le nombre de tests qui
   annonçait encore 34).
