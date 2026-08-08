---
id: VS-042
titre: Icône absente dans Stage Manager (Régisseur) macOS
statut: fait
priorité: basse
dépend-de: []
créé: 2026-08-08
mis-à-jour: 2026-08-09
---

Constat Thibault (capture) : icône OK dans le Dock mais carré générique dans
Stage Manager. Pistes : cache LaunchServices (lsregister/killall Dock),
CFBundleIconName absent de l Info.plist (chemin moderne AppKit), ou
représentation manquante. Reproduire, diagnostiquer, corriger.

## Cause

Le Régisseur est rendu par `WindowManager`, qui passe par IconServices (frameworks
privés `IconServices`/`IconFoundation`) et réclame la représentation **empilée** de
macOS 26 : les couches Liquid Glass, `IconImageStack` dans un `Assets.car`. Notre
bundle n'avait que `CFBundleIconFile` + `.icns` → gabarit blanc générique.

La piste `CFBundleIconName` était **juste mais insuffisante** : mesuré, un
`Assets.car` compilé depuis un `AppIcon.appiconset` classique ne contient que des
`Icon Image`, et le Régisseur reste générique. Il faut un paquet `.icon` (format
Icon Composer).

## Correctif

`scripts/build-macos.sh` fabrique à la volée un `AppIcon.icon` (dossier `icon.json`
+ `Assets/icon.png`, le 1024 extrait du `.icns` committé) et le compile avec
`actool` → `Assets.car` avec 3 `IconImageStack`, plus `CFBundleIconName = AppIcon`.
Aucune dépendance nouvelle, aucun binaire de plus au dépôt, `.icns` et
`CFBundleIconFile` inchangés. `actool` venant avec Xcode et non avec les seuls
Command Line Tools, son absence avertit sans faire échouer le build.

Validé à l'œil dans le vrai Régisseur (capture de l'`App Icon Window` de
`WindowManager`), contrôle négatif rouge, `swift test` 61/61, `codesign --verify
--deep --strict` vert, `Assets.car` scellé, bundle 3,2 Mo / 10 Mo.

## Piège pour la vérification

Le cache d'icônes système (`/Library/Caches/com.apple.iconservices.store`, root)
mémorise le résultat **par identifiant de bundle** : sur un Mac ayant déjà lancé une
version cassée sous `org.vibesync.client`, l'icône reste générique malgré le
correctif. Ni `killall Dock`, ni `killall iconservicesagent`, ni `lsregister -f`, ni
une montée de version ne le purgent. Commande à passer une fois :

```sh
sudo rm -rf /Library/Caches/com.apple.iconservices.store
sudo killall iconservicesd iconservicesagent Dock
```

Sur une machine vierge, l'icône est correcte du premier coup.

Détail complet : `docs/research/2026-08-09-vs042-stage-manager.md`.
