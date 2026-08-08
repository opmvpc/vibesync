#!/bin/sh
# build-macos.sh — construit le client macOS et assemble VibeSync.app.
#
# Prérequis : Xcode Command Line Tools (swift). Aucune dépendance externe.
# Idempotent : relancer le script écrase proprement le bundle précédent.
#
#   ./scripts/build-macos.sh            # build release + bundle + signature ad hoc
#   ./scripts/build-macos.sh --arm64    # force arm64 (utile sur un Mac Intel)
#
# Sortie : ui/macos/build/VibeSync.app

set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
# Depuis VS-031 (phase 2 d'ADR-010), la racine du paquet SwiftPM est celle du
# dépôt : c'est la seule qui contienne à la fois core/ (couche C commune) et
# ui/macos/. Le bundle assemblé, lui, reste rangé sous ui/macos/build.
PACKAGE_DIR="$REPO_ROOT"
BUILD_DIR="$REPO_ROOT/ui/macos/build"
APP="$BUILD_DIR/VibeSync.app"

ARCH_ARGS=""
if [ "${1:-}" = "--arm64" ]; then
    ARCH_ARGS="--arch arm64"
fi

# Version applicative (VS-023) : source unique = fichier VERSION à la racine,
# injecté dans l'Info.plist du bundle (l'équivalent mac des ldflags Go et du -D
# du client C). Absente, le client reste « dev » : illisible en semver, donc
# jamais de bannière de mise à jour.
VERSION=dev
if [ -f "$REPO_ROOT/VERSION" ]; then
    VERSION=$(tr -d ' \t\r\n' < "$REPO_ROOT/VERSION")
    [ -n "$VERSION" ] || VERSION=dev
fi

if ! command -v swift >/dev/null 2>&1; then
    echo "swift introuvable : installez les Xcode Command Line Tools" >&2
    echo "  xcode-select --install" >&2
    exit 2
fi

echo "== compilation (release, version $VERSION)"
# shellcheck disable=SC2086
swift build -c release --package-path "$PACKAGE_DIR" $ARCH_ARGS

# shellcheck disable=SC2086
BIN_DIR=$(swift build -c release --package-path "$PACKAGE_DIR" $ARCH_ARGS --show-bin-path)
BINARY="$BIN_DIR/VibeSync"
if [ ! -f "$BINARY" ]; then
    echo "binaire introuvable : $BINARY" >&2
    exit 1
fi

echo "== assemblage du bundle"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
mkdir -p "$APP/Contents/Resources"
cp "$BINARY" "$APP/Contents/MacOS/VibeSync"
chmod +x "$APP/Contents/MacOS/VibeSync"

# Icône du bundle : asset committé (comme assets/vibesync.ico côté Windows),
# rastérisé depuis assets/icon.svg. Doit être copiée AVANT le codesign, sinon la
# signature ne couvre pas la ressource et le bundle devient invalide.
ICNS="$REPO_ROOT/ui/macos/Resources/VibeSync.icns"
if [ -f "$ICNS" ]; then
    cp "$ICNS" "$APP/Contents/Resources/VibeSync.icns"
else
    echo "icône absente : $ICNS (bundle sans icône)" >&2
fi

# VS-042 — deuxième chemin d'icône, le moderne. `CFBundleIconFile` + .icns suffit
# au Dock et au Finder, mais pas au Régisseur (Stage Manager) : celui-ci est rendu
# par WindowManager, qui passe par IconServices et réclame la représentation
# *empilée* de macOS 26 (Tahoe) — les couches Liquid Glass, `IconImageStack` dans
# un `Assets.car`. Faute de quoi il affiche le gabarit blanc générique, exactement
# ce que Thibault voit. Mesuré ici, et c'est le point contre-intuitif : déclarer
# `CFBundleIconName` avec un `Assets.car` fabriqué depuis un `AppIcon.appiconset`
# classique NE SUFFIT PAS (ce catalogue-là ne contient que des « Icon Image »,
# aucune pile) — il faut passer par un paquet `.icon`, le format d'Icon Composer.
#
# `.icon` est un simple dossier : `icon.json` + `Assets/`. On l'écrit à la main
# (aucun besoin de l'app Icon Composer), avec pour unique couche le PNG 1024 tiré
# du .icns déjà committé — une seule source de vérité pour le dessin, zéro binaire
# supplémentaire au dépôt. `actool` (outil Apple, livré avec Xcode) le compile en
# `Assets.car` contenant les 3 `IconImageStack` que le système attend.
#
# `actool` vient avec Xcode, pas avec les seuls Command Line Tools : s'il manque,
# on le dit et on livre le bundle en mode historique (Dock correct, Régisseur
# générique) plutôt que d'échouer.
ICON_NAME=""
if [ -f "$ICNS" ] && command -v actool >/dev/null 2>&1; then
    CARTMP=$(mktemp -d "${TMPDIR:-/tmp}/vibesync-appicon.XXXXXX")
    DOTICON="$CARTMP/AppIcon.icon"
    mkdir -p "$DOTICON/Assets" "$CARTMP/out"
    # `icon_512x512@2x.png` est le 1024 du .icns (cf. docs/research/2026-08-08-icone-macos.md).
    iconutil -c iconset "$ICNS" -o "$CARTMP/VibeSync.iconset"
    cp "$CARTMP/VibeSync.iconset/icon_512x512@2x.png" "$DOTICON/Assets/icon.png"
    # Une seule couche, opaque et pleine cadre : notre dessin porte déjà son fond
    # et son squircle. `fill` n'est donc jamais visible, mais la clé est attendue.
    cat > "$DOTICON/icon.json" <<'ICONJSON'
{
  "fill" : { "automatic-gradient" : "extended-srgb:0.10,0.09,0.13,1.00" },
  "groups" : [
    { "layers" : [ { "image-name" : "icon.png", "name" : "icon" } ] }
  ],
  "supported-platforms" : { "squares" : [ "macOS" ] }
}
ICONJSON

    if actool --compile "$CARTMP/out" --app-icon AppIcon --platform macosx \
              --minimum-deployment-target 13.0 \
              --output-partial-info-plist "$CARTMP/partial.plist" \
              --errors --warnings "$DOTICON" >/dev/null 2>&1 \
       && [ -f "$CARTMP/out/Assets.car" ] \
       && assetutil --info "$CARTMP/out/Assets.car" 2>/dev/null | grep -q IconImageStack; then
        cp "$CARTMP/out/Assets.car" "$APP/Contents/Resources/Assets.car"
        ICON_NAME=AppIcon
    else
        echo "actool n'a pas produit d'IconImageStack : bundle sans Assets.car" >&2
        echo "  (Dock correct, icône générique en Régisseur)" >&2
    fi
    rm -rf "$CARTMP"
else
    echo "actool introuvable (Xcode complet requis) : bundle sans Assets.car," >&2
    echo "  l'icône restera générique en Régisseur / Mission Control" >&2
fi

# La clé n'est écrite que si le catalogue a bien été produit : `CFBundleIconName`
# pointant sur un asset absent est pire que pas de clé du tout.
ICON_NAME_KEY=""
if [ -n "$ICON_NAME" ]; then
    ICON_NAME_KEY="    <key>CFBundleIconName</key>
    <string>$ICON_NAME</string>"
fi

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>fr</string>
    <key>CFBundleDisplayName</key>
    <string>vibesync</string>
    <key>CFBundleExecutable</key>
    <string>VibeSync</string>
    <key>CFBundleIconFile</key>
    <string>VibeSync.icns</string>
$ICON_NAME_KEY
    <key>CFBundleIdentifier</key>
    <string>org.vibesync.client</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>vibesync</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>$VERSION</string>
    <key>CFBundleVersion</key>
    <string>$VERSION</string>
    <key>VibeSyncVersion</key>
    <string>$VERSION</string>
    <key>LSMinimumSystemVersion</key>
    <string>13.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSPrincipalClass</key>
    <string>NSApplication</string>
</dict>
</plist>
PLIST

echo "== signature ad hoc"
codesign --force --sign - --timestamp=none "$APP" >/dev/null 2>&1 || {
    echo "codesign a échoué (le bundle reste utilisable en local)" >&2
}

echo "== résultat"
ls -l "$APP/Contents/MacOS/VibeSync" | awk '{print "  binaire : " $5 " octets"}'
du -sh "$APP" | awk '{print "  bundle  : " $1}'
echo "  chemin  : $APP"
echo
echo "Lancer : open \"$APP\""
