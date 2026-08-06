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
PACKAGE_DIR="$REPO_ROOT/ui/macos"
BUILD_DIR="$PACKAGE_DIR/build"
APP="$BUILD_DIR/VibeSync.app"

ARCH_ARGS=""
if [ "${1:-}" = "--arm64" ]; then
    ARCH_ARGS="--arch arm64"
fi

if ! command -v swift >/dev/null 2>&1; then
    echo "swift introuvable : installez les Xcode Command Line Tools" >&2
    echo "  xcode-select --install" >&2
    exit 2
fi

echo "== compilation (release)"
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

cat > "$APP/Contents/Info.plist" <<'PLIST'
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
    <key>CFBundleIdentifier</key>
    <string>org.vibesync.client</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>vibesync</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0</string>
    <key>CFBundleVersion</key>
    <string>1</string>
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
