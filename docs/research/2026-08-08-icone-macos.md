# Icône de VibeSync.app (macOS) — 2026-08-08

`VibeSync.app` n'avait aucune icône : il n'y a jamais eu de `.icns` (rien à voir avec
la signature ad hoc). Le client Windows, lui, embarque `assets/vibesync.ico` généré
par `tools/genicon`. On comble le trou côté mac, avec le même asset source
`assets/icon.svg` et les mêmes contraintes ADR-008 : zéro dépendance, zéro
installation, outils système uniquement.

> **Mise à jour du même jour — la réserve « WebKit » est levée.** `tools/genicon` a
> reçu un flag `-iconset` et produit désormais les 10 PNG lui-même ; le `.icns`
> committé est celui-là. Le rendu WebKit décrit ci-dessous n'a plus qu'une valeur
> historique (il reste en annexe comme trace de la première passe). Voir
> [Regénération](#regénération-la-procédure-actuelle) pour la procédure en vigueur.

## Méthode de rendu retenue (première passe, historique)

**Swift + WebKit** (`WKWebView.takeSnapshot`), le SVG inliné dans une page à fond
transparent. Les deux autres pistes ont été écartées après mesure :

| Piste | Verdict |
|---|---|
| `tools/genicon` (Go, rastériseur SDF maison) | **Impossible sur ce Mac** : aucune toolchain Go (`go` absent, pas de mise/asdf/brew). C'est pourtant l'outil canonique du projet — repris depuis via la VM Windows, voir plus bas. |
| `qlmanage -t -s 1024` | **Infidèle** : QuickLook applique sa décoration de vignette — ombre portée + inset. Le squircle n'occupe que ~80 % du cadre et déborde d'une ombre grise. Inutilisable. |
| **Swift + WebKit** | **Retenu** : plein cadre, alpha préservé, géométrie exacte. |

Deux vérifications avant de valider cette piste :

1. **Géométrie** — la formule de `tools/genicon/main.go` (`seamGap`, `seamSlide`,
   dilatation de `triRad`) recalculée à la main redonne exactement les sommets
   écrits dans le SVG : `52.17,44.77 142.17,96.27 52.17,147.77` et
   `203.83,211.23 113.83,159.73 203.83,108.23`. Les deux sources de vérité sont
   bien synchronisées ; rendre le SVG équivaut à lancer `genicon`.
2. **Pixels** — comparaison du rendu WebKit 256×256 contre
   `assets/png/icon-256.png` (sortie `genicon`, après prémultiplication alpha) :
   écart médian **0**, p99 **7**, et les 421 pixels (0,64 %) d'écart > 16 sont
   **tous** sur une frontière de forme — zéro écart en aplat ou dans un dégradé.
   C'est de l'antialiasing de bord, pas une différence de dessin.

Piège rencontré : `WKSnapshotConfiguration.snapshotWidth` est en **points**, et
l'écran est Retina → la capture sort à 2× la valeur demandée. Les fichiers sont donc
nommés d'après `cgImage.width` réel, et on demande `taille/2` points.
Sans `webView.setValue(false, forKey: "drawsBackground")` la capture sort sur blanc
opaque.

## Tailles générées

Les 10 représentations standard, chacune **rendue nativement** depuis le vecteur
(aucun rééchantillonnage `sips` : le SVG donne un tracé net à chaque taille) :

| Fichier iconset | Pixels | | Fichier iconset | Pixels |
|---|---|---|---|---|
| `icon_16x16.png` | 16 | | `icon_128x128@2x.png` | 256 |
| `icon_16x16@2x.png` | 32 | | `icon_256x256.png` | 256 |
| `icon_32x32.png` | 32 | | `icon_256x256@2x.png` | 512 |
| `icon_32x32@2x.png` | 64 | | `icon_512x512.png` | 512 |
| `icon_128x128.png` | 128 | | `icon_512x512@2x.png` | 1024 |

Puis `iconutil -c icns` → `ui/macos/Resources/VibeSync.icns`, **committé comme
asset**, exactement comme `assets/vibesync.ico` côté Windows.

## Poids

| | rendu WebKit | rendu `genicon` |
|---|---|---|
| `ui/macos/Resources/VibeSync.icns` | 950 259 o (0,91 Mio) | **356 922 o** (0,34 Mio) |
| `VibeSync.app` (bundle complet) | 2,0 Mio | **1,4 Mio** (binaire 1 142 608 o + icône) |
| Budget CI (ADR-007/008) | 10 Mio → 20 % | 10 Mio → **14 % consommé** |

La cause était identifiée dès la première passe : **WebKit trame ses dégradés**
(bruit ±1 LSB), ce qui plombe la compression PNG. Mesure faite alors avec un `.icns`
d'essai ne contenant qu'une entrée 256 : 22 455 o depuis le PNG `genicon` (non tramé)
contre 49 500 o depuis le PNG WebKit — **×2,2**, d'où une prévision « de l'ordre de
430 Ko » pour un `.icns` `genicon`. Le passage effectif à `genicon` donne **356 922 o**,
soit **−62,4 %** — mieux que prévu (le tramage coûte proportionnellement plus cher aux
grandes tailles, qui dominent le fichier).

À noter : `iconutil` **ré-encode systématiquement** les PNG qu'on lui donne (via
ImageIO). Un ré-encodage maison sans perte (filtre adaptatif par ligne + zlib 9)
divisait pourtant les PNG par deux (1024 : 514 887 → 272 517 o) — peine perdue,
`iconutil` jette ces octets et réécrit les siens. Optimiser en amont ne sert à rien ;
ce qui compte est que les **pixels** entrants soient propres, pas leur encodage.

## Regénération — la procédure actuelle

`tools/genicon` accepte `-iconset <dir>` : il rastérise nativement (SDF + supersampling
×4, comme pour le `.ico`) les tailles 16/32/64/128/256/512/1024 et écrit les 10 noms
attendus par `iconutil`. Les tailles du `.ico` Windows (`icoSizes`) sont inchangées ;
`go run ./tools/genicon` sans le flag se comporte exactement comme avant.

**Go n'est pas installé sur le Mac de Thibault** — tant que c'est le cas, l'étape Go
passe par la VM Windows 11 ARM64 (voir `scripts/provision-vm.ps1`), et seul
l'assemblage `iconutil` se fait sur le Mac :

```sh
# 1. rendu des 10 PNG dans la VM (Go absent du Mac)
ssh -i ~/.ssh/vibesync_vm_ed25519 OPMVPC@192.168.64.2 \
  'cd C:\Users\OPMVPC\vibesync && go run ./tools/genicon -iconset C:\Users\OPMVPC\VibeSync.iconset'

# 2. rapatriement sur le Mac
scp -i ~/.ssh/vibesync_vm_ed25519 -r \
  OPMVPC@192.168.64.2:C:/Users/OPMVPC/VibeSync.iconset/ .

# 3. assemblage (iconutil n'existe que sur macOS)
iconutil -c icns VibeSync.iconset -o ui/macos/Resources/VibeSync.icns
```

Le jour où le Mac a une toolchain Go, les étapes 1–2 se réduisent à
`go run ./tools/genicon -iconset VibeSync.iconset`.

Contrôle de non-régression gratuit : les PNG 16/32/64/128/256 du `.iconset` sont
**bit à bit identiques** à `assets/png/icon-{16,32,64,128,256}.png` déjà committés
(même rastériseur, même encodeur) — un `cmp` suffit à prouver que le dessin n'a pas
bougé. Et après `iconutil`, la représentation 256 extraite du `.icns` est
**pixel-identique** (0/65536) à `assets/png/icon-256.png`.

## Câblage dans le build

`scripts/build-macos.sh`, deux ajouts :

- copie de `ui/macos/Resources/VibeSync.icns` vers `Contents/Resources/`, placée
  **avant** le `codesign` (sinon la ressource n'est pas scellée et le bundle est
  invalide) — le script tolère l'absence du fichier avec un avertissement ;
- clé `CFBundleIconFile` = `VibeSync.icns` dans le heredoc de l'`Info.plist`.

Aucun changement nécessaire dans `.github/workflows/ci.yml` : la CI appelle le script
puis vérifie signature et budget, tout passe.

## Validations

| Contrôle | Résultat |
|---|---|
| `bash scripts/build-macos.sh` | OK, bundle assemblé |
| `Contents/Resources/VibeSync.icns` présent | OK |
| `CFBundleIconFile` dans l'`Info.plist` du bundle | `VibeSync.icns`, `plutil -lint` OK |
| `codesign --verify --deep --strict` | `valid on disk`, `satisfies its Designated Requirement` |
| Icône scellée dans `_CodeSignature/CodeResources` | OK (`Resources/VibeSync.icns` présent dans `files2`) |
| `iconutil -c iconset` (relecture inverse) | 10 représentations, toutes aux dimensions attendues |
| 256 du `.icns` vs `assets/png/icon-256.png` | **0/65536 pixels d'écart** (rendu `genicon` identique par construction) |
| `NSWorkspace.icon(forFile:)` sur le bundle | renvoie **notre** icône (32 représentations, jusqu'à 2048²), pas l'icône générique |
| `swift test` | **45/45**, 0 échec |
| Go dans la VM : `build` / `test` / `vet` / `staticcheck` | tout vert (10 paquets testés) |

Le contrôle `NSWorkspace` est le seul qui prouve vraiment que *le système* voit
l'icône : LaunchServices résout le bundle et rend le squircle violet, pas le
document blanc générique.

Piège de lecture sur ce contrôle : sous **macOS 26 (Tahoe)**, `NSWorkspace` ne rend
pas nos pixels tels quels — il **remasque** l'icône d'application dans la superellipse
système et lui ajoute une ombre portée. Une comparaison pixel à pixel contre
`assets/png/icon-256.png` donne donc un gros écart (24 797/65536 pixels > 8) **sans que
rien ne soit cassé** ; l'écart contre une icône système quelconque est bien plus grand
encore (46 350). Le rendu dumpé en PNG lève tout doute : c'est notre double triangle
violet sur fond sombre, à la forme Tahoe. Ne pas transformer ce contrôle en assertion
d'égalité de pixels.

## Réserves / suites possibles

- ~~**Regénération** : le rastériseur utilisé est un one-shot ; le vrai point de chute
  serait `tools/genicon`, étendu avec une sortie `-iconset`.~~ **Fait** — voir
  [Regénération](#regénération-la-procédure-actuelle). Le `.icns` reste un binaire
  committé (comme `assets/vibesync.ico`), mais il est maintenant reproductible par
  commande, et il est passé de 950 259 à 356 922 o.
- **Reste ouvert** : le rendu Go doit passer par la VM Windows tant que le Mac n'a pas
  de toolchain Go — c'est le seul asset du projet dont la regénération est à cheval sur
  deux machines (Go dans la VM, `iconutil` sur le Mac). Un `go` sur le Mac ferait
  disparaître l'aller-retour.
- **Cosmétique, hors périmètre** : le squircle occupe 93,75 % du cadre (240/256) avec
  un rayon de 21,9 %, là où la grille Apple met le sien à ~80,5 % avec ~18 % de rayon.
  L'icône paraîtra donc un peu plus grosse et plus carrée que ses voisines dans le
  Dock. C'est le dessin déjà figé côté Windows — à ne changer que délibérément, dans
  le SVG *et* dans `genicon`.
- Le libellé de l'étape CI dit encore « 41 tests » alors qu'il y en a 45.

## Annexe — le rastériseur WebKit de la première passe (historique)

Conservé pour mémoire : ce n'est **plus** la procédure de regénération (voir
[Regénération](#regénération-la-procédure-actuelle)). Il reste utile comme moyen
indépendant de rendre `assets/icon.svg` sans toolchain Go — c'est d'ailleurs lui qui
a servi à vérifier que le SVG et `genicon` dessinent bien la même chose.

```swift
// rendersvg.swift — swift rendersvg.swift assets/icon.svg <outdir> 512 256 128 64 32 16 8
// (tailles en POINTS : l'écran Retina double, les fichiers sont nommés en pixels réels)
import Cocoa
import WebKit

let args = CommandLine.arguments
let svgPath = args[1], outDir = args[2]
let sizes = args[3...].compactMap { Int($0) }
let svg = try! String(contentsOfFile: svgPath, encoding: .utf8)

final class Renderer: NSObject, WKNavigationDelegate {
    let webView: WKWebView; var done = false; let size: Int
    init(size: Int, html: String) {
        self.size = size
        webView = WKWebView(frame: NSRect(x: 0, y: 0, width: size, height: size),
                            configuration: WKWebViewConfiguration())
        super.init()
        webView.setValue(false, forKey: "drawsBackground")   // sinon fond blanc opaque
        if #available(macOS 12.0, *) { webView.underPageBackgroundColor = .clear }
        webView.navigationDelegate = self
        webView.loadHTMLString(html, baseURL: nil)
    }
    func webView(_ wv: WKWebView, didFinish nav: WKNavigation!) {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) { self.snap() }
    }
    func snap() {
        let cfg = WKSnapshotConfiguration()
        cfg.rect = NSRect(x: 0, y: 0, width: size, height: size)
        cfg.snapshotWidth = NSNumber(value: size)
        if #available(macOS 13.0, *) { cfg.afterScreenUpdates = true }
        webView.takeSnapshot(with: cfg) { image, _ in
            var rect = NSRect(x: 0, y: 0, width: self.size, height: self.size)
            let cg = image!.cgImage(forProposedRect: &rect, context: nil, hints: nil)!
            let url = URL(fileURLWithPath: outDir)
                .appendingPathComponent("icon_\(cg.width).png")
            let d = CGImageDestinationCreateWithURL(url as CFURL, "public.png" as CFString, 1, nil)!
            CGImageDestinationAddImage(d, cg, nil); _ = CGImageDestinationFinalize(d)
            self.done = true
        }
    }
}

NSApplication.shared.setActivationPolicy(.accessory)
for size in sizes {
    let html = """
    <!doctype html><meta charset="utf-8">
    <style>html,body{margin:0;padding:0;background:transparent;overflow:hidden}
    svg{display:block;width:\(size)px;height:\(size)px}</style>
    \(svg)
    """
    let r = Renderer(size: size, html: html)
    let deadline = Date().addingTimeInterval(30)
    while !r.done && Date() < deadline {
        RunLoop.main.run(mode: .default, before: Date().addingTimeInterval(0.02))
    }
}
```

Puis l'assemblage — cette copie manuelle est précisément ce que `genicon -iconset`
fait maintenant tout seul, en écrivant directement les bons noms :

```sh
mkdir VibeSync.iconset
cp icon_16.png   VibeSync.iconset/icon_16x16.png
cp icon_32.png   VibeSync.iconset/icon_16x16@2x.png
cp icon_32.png   VibeSync.iconset/icon_32x32.png
cp icon_64.png   VibeSync.iconset/icon_32x32@2x.png
cp icon_128.png  VibeSync.iconset/icon_128x128.png
cp icon_256.png  VibeSync.iconset/icon_128x128@2x.png
cp icon_256.png  VibeSync.iconset/icon_256x256.png
cp icon_512.png  VibeSync.iconset/icon_256x256@2x.png
cp icon_512.png  VibeSync.iconset/icon_512x512.png
cp icon_1024.png VibeSync.iconset/icon_512x512@2x.png
iconutil -c icns VibeSync.iconset -o ui/macos/Resources/VibeSync.icns
```
