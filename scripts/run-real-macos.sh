#!/bin/bash
# run-real-macos.sh — séance réelle à deux clients sur UNE machine macOS.
#
# Équivalent mac de scripts/run-real-sandbox.ps1 : là où Windows jette le tout
# dans une sandbox, on isole ici deux instances de VibeSync.app sur la même
# machine, chacune avec son HOME, sa suite de préférences et son vrai VLC. Le
# but est le même — vérifier de bout en bout ce qu'aucun test unitaire ne peut
# prouver : que deux VLC réels restent synchronisés.
#
#   ./scripts/run-real-macos.sh [fichier-video] [url-serveur]
#
#   fichier-video : facultatif. Sans lui, un WAV silencieux de 10 minutes est
#                   généré (aucune dépendance : perl est fourni par macOS).
#   url-serveur   : facultatif, défaut ws://127.0.0.1:8080/ws — dans ce cas un
#                   serveur doit DÉJÀ tourner (go run ./cmd/vibesync-server).
#
# Variables :
#   VIBESYNC_PASSWORD  mot de passe du serveur (jamais en argument : argv est
#                      lisible par tous les processus de la machine)
#   VIBESYNC_ROOM      salle à utiliser (défaut : vibesync-test-$RANDOM)
#   VIBESYNC_KEEP=1    ne pas effacer le dossier de travail à la fin
#
# Les deux instances tournent en « mode auto » (ui/macos/Sources/VibeSync/UI/
# AutoPilot.swift) : elles se connectent seules, publient leur état dans un
# fichier JSON une-ligne et exécutent les commandes qu'on ajoute à leur fichier
# de commandes. C'est ce qui rend ce script possible sans CLI dans l'app.
#
# Sortie : un PASS/FAIL par point de contrôle, code retour non nul si l'un
# échoue, journaux dans le dossier de travail affiché en fin de course.

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
APP="$REPO_ROOT/ui/macos/build/VibeSync.app"
BIN="$APP/Contents/MacOS/VibeSync"

MEDIA_ARG="${1:-}"
URL="${2:-ws://127.0.0.1:8080/ws}"
PASSWORD="${VIBESYNC_PASSWORD:-}"
ROOM="${VIBESYNC_ROOM:-vibesync-test-$RANDOM}"

# Bornes du scénario (secondes). Le total tient sous ~90 s.
CONNECT_TIMEOUT=25
FILE_TIMEOUT=30
STEP_TIMEOUT=20
HOLD_SEC=5
SEEK_TARGET=120
# DRIFT_MAX : écart toléré entre les deux lecteurs à la fin de la séance.
# Aligné sur la ZONE MORTE du moteur (1,5 s, docs/protocol.md §Correction) :
# depuis VS-038 rien ne rapproche deux lecteurs tant qu'ils sont dedans, exiger
# mieux serait exiger du moteur ce qu'il ne cherche plus à faire.
DRIFT_MAX=1.5
MEDIA_SECONDS=600

FAILURES=0
PASSED=0

# --- affichage -------------------------------------------------------------

say()  { printf '%s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }
pass() { PASSED=$((PASSED + 1)); printf '  PASS  %s\n' "$*"; }
fail() { FAILURES=$((FAILURES + 1)); printf '  FAIL  %s\n' "$*"; }
die()  { printf 'erreur : %s\n' "$*" >&2; exit 2; }

# --- lecture de l'état publié par les instances ----------------------------

# jget <fichier-état> <clé> — les valeurs lues ici sont toutes des nombres, des
# booléens ou des identifiants sans virgule : une extraction sed suffit, et on
# évite d'imposer jq.
jget() {
    [ -f "$1" ] || return 0
    sed -n "s/.*\"$2\":\([^,}]*\).*/\1/p" "$1" 2>/dev/null | tr -d '"'
}

# num <valeur> — 0 si vide, pour que awk ait toujours un nombre.
num() { [ -n "${1:-}" ] && printf '%s' "$1" || printf '0'; }

# lt / ge : comparaisons flottantes (le shell ne sait faire que des entiers).
lt() { awk -v a="$(num "$1")" -v b="$(num "$2")" 'BEGIN{exit !(a<b)}'; }
ge() { awk -v a="$(num "$1")" -v b="$(num "$2")" 'BEGIN{exit !(a>=b)}'; }
# absdiff <a> <b>
absdiff() { awk -v a="$(num "$1")" -v b="$(num "$2")" 'BEGIN{d=a-b; if(d<0)d=-d; printf "%.3f", d}'; }

pos1() { jget "$ST1" positionSec; }
pos2() { jget "$ST2" positionSec; }
gap()  { absdiff "$(pos1)" "$(pos2)"; }

# snapshot — une ligne lisible par pair, comme le logPositions du harnais Go.
snapshot() {
    for i in 1 2; do
        eval "f=\$ST$i"
        printf '  client%s : phase=%-9s vlc=%-7s pos=%8s salle=%8s drift=%7s paused=%-5s ready=%-5s users=%s buf=%s\n' \
            "$i" "$(jget "$f" phase)" "$(jget "$f" vlcState)" "$(jget "$f" positionSec)" \
            "$(jget "$f" roomPositionSec)" "$(jget "$f" driftSec)" "$(jget "$f" paused)" \
            "$(jget "$f" ready)" "$(jget "$f" users)" "$(jget "$f" buffering)"
    done
}

# wait_for <secondes> <description> <condition shell> — attend que la condition
# devienne vraie. Rend 1 au dépassement du délai (et le laisse dire à l'appelant).
wait_for() {
    local timeout=$1 desc=$2 cond=$3
    local end=$(( $(date +%s) + timeout ))
    while [ "$(date +%s)" -lt "$end" ]; do
        if eval "$cond"; then
            return 0
        fi
        sleep 0.5
    done
    say "  (délai dépassé : $desc)"
    return 1
}

# holds <secondes> <condition> — la condition doit rester vraie tout du long.
holds() {
    local secs=$1 cond=$2
    local end=$(( $(date +%s) + secs ))
    while [ "$(date +%s)" -lt "$end" ]; do
        eval "$cond" || return 1
        sleep 0.5
    done
    return 0
}

# --- média ------------------------------------------------------------------

# make_silent_wav <chemin> <secondes> — WAV PCM 8 bits, 8 kHz, mono. Même
# recette que writeSilentWAV du harnais Go : VLC l'ouvre, en donne la durée, et
# il ne coûte rien à produire (4,8 Mo pour 10 minutes).
make_silent_wav() {
    perl -e '
        my ($path, $secs) = @ARGV;
        my $rate = 8000;
        my $data = $rate * $secs;
        open(my $fh, ">", $path) or die "ouverture de $path : $!";
        binmode($fh);
        print $fh "RIFF" . pack("V", 36 + $data) . "WAVE";
        print $fh "fmt " . pack("V", 16) . pack("v", 1) . pack("v", 1)
                . pack("V", $rate) . pack("V", $rate) . pack("v", 1) . pack("v", 8);
        print $fh "data" . pack("V", $data);
        my $chunk = chr(128) x $rate;   # silence en PCM 8 bits non signé
        print $fh $chunk for (1 .. $secs);
        close($fh);
    ' "$1" "$2"
}

# --- préparation ------------------------------------------------------------

step "vérifications"
[ -d /Applications/VLC.app ] || die "VLC introuvable dans /Applications — le test réel exige le vrai lecteur"
command -v perl >/dev/null 2>&1 || die "perl introuvable (fourni par macOS)"
say "  VLC     : /Applications/VLC.app"

if [ ! -x "$BIN" ]; then
    say "  bundle  : absent, construction…"
    "$SCRIPT_DIR/build-macos.sh" >/dev/null || die "scripts/build-macos.sh a échoué"
fi
[ -x "$BIN" ] || die "binaire introuvable : $BIN"
say "  client  : $BIN"
say "  serveur : $URL"
say "  salle   : $ROOM"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/vibesync-real.XXXXXX") || die "dossier de travail impossible"
ST1="$WORK/c1.json"; ST2="$WORK/c2.json"
CMD1="$WORK/c1.cmds"; CMD2="$WORK/c2.cmds"
LOG1="$WORK/c1.log"; LOG2="$WORK/c2.log"
: > "$CMD1"; : > "$CMD2"
say "  travail : $WORK"

step "média"
if [ -n "$MEDIA_ARG" ]; then
    [ -f "$MEDIA_ARG" ] || die "fichier introuvable : $MEDIA_ARG"
    MEDIA1=$(cd "$(dirname "$MEDIA_ARG")" && pwd)/$(basename "$MEDIA_ARG")
    MEDIA2="$MEDIA1"
    say "  fichier fourni : $MEDIA1 (les deux clients ouvrent la même copie)"
else
    # Chacun sa copie, comme dans la vraie vie — mais le MÊME nom de base et la
    # même durée, sinon le serveur signalerait une divergence de fichier.
    mkdir -p "$WORK/media1" "$WORK/media2"
    MEDIA1="$WORK/media1/vibesync-test.wav"
    MEDIA2="$WORK/media2/vibesync-test.wav"
    make_silent_wav "$MEDIA1" "$MEDIA_SECONDS" || die "génération du média impossible"
    cp "$MEDIA1" "$MEDIA2" || die "copie du média impossible"
    say "  WAV silencieux généré ($MEDIA_SECONDS s, une copie par client)"
fi

PID1=""; PID2=""

# cleanup — départ volontaire par la commande `quit` (c'est LUI qui envoie la
# close 1000 et arrête VLC), puis on ne tue que ce qui a survécu.
cleanup() {
    [ -n "$PID1" ] && kill -0 "$PID1" 2>/dev/null && echo quit >> "$CMD1"
    [ -n "$PID2" ] && kill -0 "$PID2" 2>/dev/null && echo quit >> "$CMD2"
    local end=$(( $(date +%s) + 8 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        kill -0 "$PID1" 2>/dev/null || kill -0 "$PID2" 2>/dev/null || break
        sleep 0.5
    done
    [ -n "$PID1" ] && kill -9 "$PID1" 2>/dev/null
    [ -n "$PID2" ] && kill -9 "$PID2" 2>/dev/null
    pkill -f "$WORK" 2>/dev/null
    return 0
}
trap 'cleanup; exit 130' INT TERM

# launch <n°> <pseudo> <média> — une instance isolée.
#
# Deux points qui ont coûté cher à trouver :
#
#   * on passe par `open -n --env` (macOS 13+) et NON par un lancement direct du
#     binaire. Une app lancée directement depuis un shell hérite du contexte de
#     ce shell ; sous un shell restreint (agent, CI, session sans Aqua) le
#     réseau d'URLSession reste muet — la connexion ne part jamais et aucune
#     erreur ne remonte. LaunchServices, lui, démarre l'app dans la session
#     graphique de l'utilisateur, comme un double-clic.
#   * `open` ne rend pas le pid : c'est l'app qui l'écrit dans son état.
#
# Isolation : suite de préférences distincte — indispensable, sans quoi les deux
# instances présentent le MÊME jeton de session au serveur (VS-028) — et HOME
# distinct pour que chaque VLC ait sa configuration.
launch() {
    local idx=$1 name=$2 media=$3
    local home="$WORK/home$idx"
    mkdir -p "$home"
    eval "local status=\$ST$idx cmds=\$CMD$idx log=\$LOG$idx"
    rm -f "$status"
    open -n -g -a "$APP" --stdout "$log" --stderr "$log" \
        --env "HOME=$home" \
        --env "VIBESYNC_SUITE=org.vibesync.test.$$.$idx" \
        --env "VIBESYNC_AUTO_URL=$URL" \
        --env "VIBESYNC_AUTO_NAME=$name" \
        --env "VIBESYNC_AUTO_ROOM=$ROOM" \
        --env "VIBESYNC_AUTO_PASSWORD=$PASSWORD" \
        --env "VIBESYNC_AUTO_FILE=$media" \
        --env "VIBESYNC_AUTO_STATUS=$status" \
        --env "VIBESYNC_AUTO_CMDS=$cmds" \
        --env "VIBESYNC_AUTO_SCENARIO=client$idx" \
        || die "open a refusé de lancer $APP"
    # L'app publie son pid dès son premier état.
    local end=$(( $(date +%s) + 20 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        local pid
        pid=$(jget "$status" pid)
        if [ -n "$pid" ]; then
            eval "PID$idx=$pid"
            return 0
        fi
        sleep 0.5
    done
    die "l'instance $idx n'a pas publié son état ($status) — voir $log"
}

step "lancement des deux clients"
launch 1 "alice" "$MEDIA1"
launch 2 "bob" "$MEDIA2"
say "  pid client1=$PID1  client2=$PID2"

# --- points de contrôle -----------------------------------------------------

step "(a) les deux clients sont connectés"
# Un refus du serveur (mauvais mot de passe, pseudo pris) est définitif : le
# client repasse en `idle` et n'insiste pas. Inutile d'attendre le délai plein.
if wait_for "$CONNECT_TIMEOUT" "connexion des deux clients" \
    '[ "$(jget "$ST1" connected)" = true ] && [ "$(jget "$ST2" connected)" = true ] \
     || { [ "$(jget "$ST1" phase)" = idle ] && [ -n "$(jget "$ST1" error)" ]; }' \
   && [ "$(jget "$ST1" connected)" = true ] && [ "$(jget "$ST2" connected)" = true ]; then
    pass "connectés à $URL (salle $ROOM)"
else
    fail "connexion impossible : $(jget "$ST1" connection) — $(jget "$ST1" error)$(jget "$ST1" lastError)"
    say "  (mot de passe : VIBESYNC_PASSWORD ; journaux : $LOG1 / $LOG2)"
    snapshot
    cleanup
    say ""
    say "journaux : $WORK"
    exit 1
fi
wait_for "$STEP_TIMEOUT" "les deux membres se voient" \
    '[ "$(jget "$ST1" users)" = 2 ] && [ "$(jget "$ST2" users)" = 2 ]' \
    && pass "les deux membres se voient dans la salle" \
    || fail "les membres ne se voient pas ($(jget "$ST1" users) / $(jget "$ST2" users))"

step "(b) les deux VLC sont attachés et le fichier déclaré"
if wait_for "$FILE_TIMEOUT" "fichier déclaré des deux côtés" \
    '[ "$(jget "$ST1" fileDeclared)" = true ] && [ "$(jget "$ST2" fileDeclared)" = true ]'; then
    pass "VLC lancé et fichier déclaré des deux côtés"
else
    fail "fichier non déclaré — VLC n'a pas démarré ?"
fi
snapshot

step "(c) lecture lancée par le client 1"
echo ready >> "$CMD1"
echo ready >> "$CMD2"
wait_for "$STEP_TIMEOUT" "les deux se déclarent prêts" \
    '[ "$(jget "$ST1" ready)" = true ] && [ "$(jget "$ST2" ready)" = true ]' \
    && say "  ready-gate levé" || say "  (ready non confirmé, on tente quand même)"
echo play >> "$CMD1"
if wait_for "$STEP_TIMEOUT" "les deux VLC jouent" \
    '[ "$(jget "$ST1" vlcState)" = playing ] && [ "$(jget "$ST2" vlcState)" = playing ]'; then
    if wait_for "$STEP_TIMEOUT" "la position avance des deux côtés" \
        'ge "$(pos1)" 1 && ge "$(pos2)" 1'; then
        pass "play depuis le client 1 : la position avance chez les deux"
    else
        fail "la position n'avance pas (pos1=$(pos1) pos2=$(pos2))"
    fi
else
    fail "les deux VLC ne jouent pas (vlc1=$(jget "$ST1" vlcState) vlc2=$(jget "$ST2" vlcState))"
fi
snapshot

say "  maintien de la synchronisation pendant ${HOLD_SEC}s…"
if holds "$HOLD_SEC" '[ "$(jget "$ST1" vlcState)" = playing ] && lt "$(gap)" 2'; then
    pass "synchronisation stable en lecture (écart $(gap) s)"
else
    fail "synchronisation instable en lecture (écart $(gap) s)"
fi

step "(d) pause demandée par le client 2"
echo pause >> "$CMD2"
if wait_for "$STEP_TIMEOUT" "les deux VLC en pause" \
    '[ "$(jget "$ST1" vlcState)" = paused ] && [ "$(jget "$ST2" vlcState)" = paused ]'; then
    if holds 2 '[ "$(jget "$ST1" vlcState)" = paused ] && [ "$(jget "$ST2" vlcState)" = paused ]'; then
        pass "pause depuis le client 2 : les deux sont en pause et y restent"
    else
        fail "la pause n'a pas tenu"
    fi
else
    fail "pause non propagée (vlc1=$(jget "$ST1" vlcState) vlc2=$(jget "$ST2" vlcState))"
fi
snapshot

step "(e) seek demandé par le client 1"
echo "seek $SEEK_TARGET" >> "$CMD1"
if wait_for "$STEP_TIMEOUT" "les deux VLC près de la cible" \
    'lt "$(absdiff "$(pos1)" '"$SEEK_TARGET"')" 3 && lt "$(absdiff "$(pos2)" '"$SEEK_TARGET"')" 3'; then
    pass "seek à ${SEEK_TARGET}s : les deux positions y sont (écart $(gap) s)"
else
    fail "seek non propagé (pos1=$(pos1) pos2=$(pos2), cible $SEEK_TARGET)"
fi
snapshot

step "(f) drift final"
echo play >> "$CMD2"
wait_for "$STEP_TIMEOUT" "reprise de la lecture" \
    '[ "$(jget "$ST1" vlcState)" = playing ] && [ "$(jget "$ST2" vlcState)" = playing ]' \
    && say "  la lecture est repartie (relancée par le client 2)"
wait_for "$STEP_TIMEOUT" "convergence sous $DRIFT_MAX s" 'lt "$(gap)" '"$DRIFT_MAX" >/dev/null
FINAL_GAP=$(gap)
D1=$(jget "$ST1" driftSec); D2=$(jget "$ST2" driftSec)
if lt "$FINAL_GAP" "$DRIFT_MAX"; then
    pass "écart entre les deux VLC = ${FINAL_GAP}s (< ${DRIFT_MAX}s) — drift salle ${D1}/${D2}"
else
    fail "écart final ${FINAL_GAP}s ≥ ${DRIFT_MAX}s — drift salle ${D1}/${D2}"
fi
snapshot

step "(g) vitesse constante 1x (VS-038)"
# Le critère de confort du ticket : sur une séance normale, le moteur ne doit
# envoyer AUCUNE commande `rate` à VLC — la vitesse ne corrige plus la dérive.
R1=$(jget "$ST1" rateCmds); R2=$(jget "$ST2" rateCmds)
if [ "$(num "$R1")" = "0" ] && [ "$(num "$R2")" = "0" ]; then
    pass "aucune commande rate envoyée à VLC (client1=$(num "$R1") client2=$(num "$R2"))"
else
    fail "commandes rate envoyées à VLC : client1=$(num "$R1") client2=$(num "$R2") — la vitesse ne doit plus corriger la dérive"
fi
say "  seeks de recalage : client1=$(num "$(jget "$ST1" seekCmds)") client2=$(num "$(jget "$ST2" seekCmds)")"

step "(h) fermeture propre"
echo quit >> "$CMD1"
echo quit >> "$CMD2"
if wait_for 15 "arrêt des deux clients" \
    '! kill -0 "$PID1" 2>/dev/null && ! kill -0 "$PID2" 2>/dev/null'; then
    pass "les deux clients se sont arrêtés d'eux-mêmes (close 1000 envoyée)"
else
    fail "un client ne s'est pas arrêté sur la commande quit"
fi
sleep 1
# Les VLC lancés par le test ouvrent NOS fichiers : aucun ne doit survivre.
ORPHANS=$(pgrep -f "VLC.app/Contents/MacOS/VLC.*$WORK" 2>/dev/null | wc -l | tr -d ' ')
if [ "$ORPHANS" = "0" ]; then
    pass "aucun VLC orphelin"
else
    fail "$ORPHANS VLC orphelin(s) — tuez-les à la main"
    pkill -f "VLC.app/Contents/MacOS/VLC.*$WORK" 2>/dev/null
fi

# --- bilan ------------------------------------------------------------------

cleanup
step "bilan"
say "  $PASSED point(s) OK, $FAILURES échec(s)"
say "  journaux et états : $WORK"
if [ "${VIBESYNC_KEEP:-0}" != "1" ] && [ "$FAILURES" = "0" ]; then
    say "  (VIBESYNC_KEEP=1 pour conserver le dossier)"
fi
if [ "$FAILURES" != "0" ]; then
    say ""
    say "--- fin du journal client1 ---"; tail -n 15 "$LOG1" 2>/dev/null
    say "--- fin du journal client2 ---"; tail -n 15 "$LOG2" 2>/dev/null
    exit 1
fi
say ""
say "RÉSULTAT : PASS"
exit 0
