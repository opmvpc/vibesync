#!/bin/sh
# test-core-macos.sh — compile et exécute la suite C PORTABLE sur macOS.
#
# ADR-010 : core/src (C portable) + core/posix (primitives du système) forment
# la couche commune aux deux clients natifs. Sous Windows c'est `build.bat test`
# qui la couvre ; ici on compile la moitié portable de la suite (core/tests)
# avec le même jeu d'avertissements que build.bat, sous AddressSanitizer et
# UndefinedBehaviorSanitizer — que le build Windows n'active pas.
#
#   ./scripts/test-core-macos.sh          # asan + ubsan, -O1 -g
#   ./scripts/test-core-macos.sh --fast   # -O2 sans sanitizer (contrôle release)
#
# Sortie non nulle si une vérification échoue.

set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
CORE="$REPO_ROOT/core"
# Sortie dans .build (déjà ignorée par git, et hors de tout chemin de
# cible SwiftPM : rien à exclure dans Package.swift).
OUT="$REPO_ROOT/.build/core-tests"
VECTORS="$REPO_ROOT/test/vectors"

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"
OPT="-O1 -g"
BIN="$OUT/test_core_asan"
if [ "${1:-}" = "--fast" ]; then
    SAN=""
    OPT="-O2"
    BIN="$OUT/test_core"
fi

if ! command -v clang >/dev/null 2>&1; then
    echo "clang introuvable : installez les Xcode Command Line Tools" >&2
    exit 2
fi

STD="-std=c11 -ffp-contract=off"
WARN="-Wall -Wextra -Werror -Wshadow -Wvla -Wstrict-prototypes -Wmissing-prototypes"

mkdir -p "$OUT"
echo "== compilation ($OPT ${SAN:-sans sanitizer})"
# shellcheck disable=SC2086
clang $STD $WARN $OPT $SAN \
    -I "$CORE/include" \
    -o "$BIN" \
    "$CORE"/src/*.c \
    "$CORE"/posix/*.c \
    "$CORE"/tests/test_core.c \
    "$CORE"/tests/main_posix.c

echo "== exécution"
# halt_on_error : un dépassement d'arène ou un UB doivent ARRÊTER la suite, pas
# la teinter. core/tests/ubsan.supp est volontairement vide de règles depuis la
# clôture de VS-035 : toute remontée fait échouer le script.
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS="suppressions=$CORE/tests/ubsan.supp:print_stacktrace=1:halt_on_error=1" \
    "$BIN" "$VECTORS"
