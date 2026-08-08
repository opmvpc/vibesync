# VS-042 — icône générique en Régisseur (Stage Manager) — 2026-08-09

Constat terrain (Thibault, macOS 26 Tahoe) : l'icône de `VibeSync.app` est correcte
dans le Dock, mais le Régisseur affiche le gabarit blanc générique. `killall Dock`
sans effet.

## Verdict

**Le Régisseur ne lit ni `CFBundleIconFile`/`.icns`, ni un `Assets.car` classique.**
Il lui faut la représentation *empilée* de macOS 26 — les couches Liquid Glass,
`IconImageStack` dans un `Assets.car` — produite à partir d'un paquet `.icon`
(format Icon Composer). Sans elle : icône générique.

C'est **corrigé** dans `scripts/build-macos.sh`, et vérifié à l'œil dans le vrai
Régisseur (captures ci-dessous).

Un **effet de bord empoisonne la vérification** : le cache d'icônes du système
(`/Library/Caches/com.apple.iconservices.store`, appartenant à root) mémorise le
résultat **par identifiant de bundle**. Sur un Mac qui a déjà lancé une version
cassée sous `org.vibesync.client`, l'icône reste générique même après le correctif.
Voir [Le cache qui ment](#le-cache-qui-ment).

## Comment le Régisseur va chercher l'icône

Reconstitué sur la machine, pas deviné :

| Observation | Commande / preuve |
|---|---|
| Le Régisseur, c'est `WindowManager` | `/System/Library/CoreServices/WindowManager.app` |
| Il ne passe pas par `NSWorkspace` mais par **IconServices** | `otool -L` → `IconServices` + `IconFoundation` (frameworks privés) ; `strings` → `IconServicesAppIconProvider`, `processSerialNumbersToAppIcons` |
| Il identifie l'app **par processus**, via LaunchServices | `nm -u` → `_LSCopyApplicationInformation`, `_LSASNCreateWithPid` |
| Les vignettes du bandeau sont de vraies fenêtres | `CGWindowListCopyWindowInfo` → fenêtres 64×64 nommées `App Icon Window`, propriétaire `WindowManager` |

Ces fenêtres 64×64 sont la clé de la méthode de mesure : `screencapture -l<id>` les
capture **une par une**, ce qui donne une lecture exacte de ce que le Régisseur
affiche, sans toucher à un seul réglage système.

## Reproduction et A/B

Le Régisseur était **déjà activé** sur le Mac (`defaults read
com.apple.WindowManager GloballyEnabled` → `1`, lecture seule) : la reproduction
visuelle directe était donc possible sans rien modifier.

Protocole, pour chaque variante : bundle recopié sous un **identifiant neuf** (sinon
le cache décide à notre place, cf. plus bas), lancé par
`open -n --env HOME=<répertoire vierge>` — le `HOME` vierge évite l'invite trousseau
au démarrage (VS-025), qui bloque l'app avant qu'elle n'ouvre sa fenêtre, donc avant
qu'elle n'entre dans le Régisseur. Puis on repère la nouvelle `App Icon Window` et on
la capture.

| Variante | `Assets.car` | Régisseur |
|---|---|---|
| `.icns` + `CFBundleIconFile` seuls (**état d'avant**) | aucun | **générique** |
| + `CFBundleIconName` + `Assets.car` depuis un `AppIcon.appiconset` | 10 « Icon Image » | **générique** |
| + `CFBundleIconName` + `Assets.car` depuis un paquet `.icon` | « Icon Image » **+ 3 `IconImageStack`** | **notre icône** |

La deuxième ligne est le résultat contre-intuitif : la piste « il manque
`CFBundleIconName` » est **juste mais insuffisante**. Déclarer la clé et livrer un
catalogue d'assets classique ne change rien — ce catalogue ne contient que des
`Icon Image`. Ce que le système réclame, c'est la pile.

Comparaison qui a mis sur la voie : `assetutil --info` sur `Calculator.app` →
`IconImageStack` ×3 ; sur notre premier `Assets.car` → zéro.

Deux fausses pistes écartées en chemin, chacune par une mesure :

- **« C'est le SDK »** — notre binaire est lié au SDK 26.5, Discord (icône correcte
  en Régisseur, `.icns` seul) au 15.5. Testé en réécrivant le `LC_BUILD_VERSION` avec
  `vtool -set-build-version macos 13.0 15.0 -replace` : **toujours générique**. Le
  SDK n'est pas la porte.
- **« C'est une copie concurrente du même identifiant »** — deux copies sous un même
  identifiant neuf, l'une sans pile, l'autre avec ; on lance celle **avec** :
  **icône correcte**. Ce n'est pas la copie voisine qui gagne.

Et un contrôle négatif qui a recadré tout le diagnostic : `Calculator.app` recopiée,
ré-identifiée et resignée ad hoc affiche elle aussi le gabarit générique — preuve que
le symptôme ne vient pas de *notre* dessin.

## Le correctif

`scripts/build-macos.sh` fabrique désormais, à la volée, un paquet `.icon` puis le
compile avec `actool` :

```
AppIcon.icon/
  icon.json          ← écrit à la main par le script
  Assets/icon.png    ← le 1024 extrait du .icns committé (iconutil -c iconset)
```

```json
{
  "fill" : { "automatic-gradient" : "extended-srgb:0.10,0.09,0.13,1.00" },
  "groups" : [
    { "layers" : [ { "image-name" : "icon.png", "name" : "icon" } ] }
  ],
  "supported-platforms" : { "squares" : [ "macOS" ] }
}
```

```sh
actool --compile <out> --app-icon AppIcon --platform macosx \
       --minimum-deployment-target 13.0 \
       --output-partial-info-plist <plist> AppIcon.icon
```

Points de méthode :

- **Aucune dépendance nouvelle, aucun binaire de plus au dépôt.** Le `.icon` est un
  simple dossier ; l'app **Icon Composer n'est pas nécessaire** — le format se laisse
  écrire à la main, et `actool` valide. `actool` est l'outil Apple du catalogue
  d'assets. Le dessin garde **une seule source de vérité** : le `.icns` déjà committé
  (donc `assets/icon.svg` → `tools/genicon`, cf.
  `docs/research/2026-08-08-icone-macos.md`).
- Une seule couche, opaque et pleine cadre : notre dessin porte déjà son fond et son
  squircle, donc `fill` n'est jamais visible (mais la clé est attendue).
- `--minimum-deployment-target 13.0` et `26.0` produisent le **même** `Assets.car`
  (mêmes 3 `IconImageStack`, même taille) : on garde 13.0, cohérent avec
  `LSMinimumSystemVersion`.
- `CFBundleIconFile` + `VibeSync.icns` restent inchangés (Dock, Finder, macOS
  antérieurs). `CFBundleIconName` n'est écrite **que si** le catalogue a bien été
  produit *et* contient une pile — une clé pointant sur un asset absent serait pire
  que pas de clé.
- **`actool` vient avec Xcode, pas avec les seuls Command Line Tools** (le prérequis
  documenté du script). S'il manque, le script **avertit et continue** : bundle en
  mode historique, Dock correct, Régisseur générique. Pas d'échec de build.
- Copie du `Assets.car` **avant** le `codesign`, comme le `.icns` (sinon la ressource
  n'est pas scellée et le bundle est invalide).

## Le cache qui ment

C'est ce qui explique le « `killall Dock` sans effet » de Thibault, et c'est le
piège qui a failli faire conclure que le correctif ne marchait pas.

Le même bundle, au bit près :

| Identifiant | Régisseur |
|---|---|
| `org.vibesync.client` (déjà utilisé par les versions cassées) | **générique** |
| un identifiant neuf | **notre icône** |

Le résultat ne dépend donc pas du contenu du bundle mais de son **identifiant** :
une entrée négative persiste dans le cache d'icônes du système. Ce qui **ne suffit
pas** à la purger, mesuré un par un :

- `killall Dock`
- `killall iconservicesagent`
- vider `$(getconf DARWIN_USER_CACHE_DIR)/com.apple.iconservices{,agent}`
- `lsregister -f` sur le bundle corrigé
- **monter la version** (`CFBundleVersion` 0.2.4 → 0.2.5)
- supprimer les copies concurrentes du même identifiant

Reste `/Library/Caches/com.apple.iconservices.store`, qui appartient à root — non
purgé ici, pour ne pas toucher au système de Thibault avec `sudo`. C'est la commande
à lui donner :

```sh
sudo rm -rf /Library/Caches/com.apple.iconservices.store
sudo killall iconservicesd iconservicesagent Dock
```

**Sur une machine qui n'a jamais lancé VibeSync, la question ne se pose pas** :
l'icône sera correcte du premier coup. Le cache empoisonné ne concerne que les Mac de
développement et celui de Thibault. C'est cohérent avec les remontées publiques sur
Tahoe, où le cache du Régisseur est décrit comme non invalidable côté développeur.

## Validations

| Contrôle | Résultat |
|---|---|
| `sh -n scripts/build-macos.sh` | OK |
| `bash scripts/build-macos.sh` | OK, bundle assemblé |
| `plutil -lint` de l'`Info.plist` | OK |
| `CFBundleIconFile` / `CFBundleIconName` | `VibeSync.icns` / `AppIcon` |
| `Assets.car` présent et contenant la pile | `assetutil` → **3 `IconImageStack`** |
| `codesign --verify --deep --strict` | `valid on disk`, `satisfies its Designated Requirement` |
| `codesign -dv` | `Signature=adhoc` (inchangé) |
| Icône **et** catalogue scellés | `Resources/Assets.car` et `Resources/VibeSync.icns` dans `CodeResources` |
| Budget bundle (10 Mo) | **3,2 Mo** (1,5 Mo avant ; le `Assets.car` pèse 1,78 Mo) |
| `swift test` | **61/61**, 0 échec |
| **Régisseur, vraie session, capture de l'`App Icon Window`** | **notre icône** (identifiant neuf) ; **générique** sans le correctif — contrôle négatif rouge |
| Go | absent de ce Mac ; aucun fichier Go touché par ce ticket |

À noter : `actool` n'est **pas reproductible au bit près** (deux compilations du même
`.icon` donnent des `Assets.car` de sommes différentes). Sans conséquence
fonctionnelle, mais le zip de release ne sera pas identique d'un build à l'autre.

## Réserves

- Le rendu Tahoe applique son masque et son ombre à notre dessin : dans le bandeau,
  notre squircle apparaît **dans** le squircle système. C'est lisible et conforme aux
  autres apps, mais c'est le corollaire de la réserve cosmétique déjà notée dans
  `docs/research/2026-08-08-icone-macos.md` (notre squircle occupe 93,75 % du cadre
  là où Apple met le sien à ~80,5 %). Une passe sur `assets/icon.svg` + `genicon`
  donnerait un rendu plus juste — hors périmètre.
- Le `.icon` n'a **qu'une couche**. Le format en accepte plusieurs (parallaxe,
  spéculaire, variantes clair/sombre/teinté). On ne s'en sert pas : notre dessin est
  plat. Piste d'amélioration, pas un manque.
- La CI (`macos-latest`) a Xcode complet, donc `actool` : le chemin nominal est
  couvert. Un Mac avec les seuls Command Line Tools produira un bundle sans
  `Assets.car` — avec un avertissement explicite.
