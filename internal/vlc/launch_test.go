package vlc

import (
	"strings"
	"testing"
)

// Ligne de lancement (VS-029) : chaque drapeau neutralise un réglage que le
// vlcrc de l'utilisateur pourrait imposer. Les perdre, c'est reproduire le
// retour terrain « VLC s'ouvre et joue, l'app dit aucun fichier ouvert » —
// d'où ce gel explicite, miroir de celui du client C (core/tests/test_core.c,
// bloc vlc_build_command).
func TestLaunchArgsGeleWindows(t *testing.T) {
	want := []string{
		"--extraintf=http",
		"--lua-intf=http",
		"--http-host=127.0.0.1",
		"--http-port=41234",
		"--http-password=deadbeef",
		"--no-one-instance",
		"--no-one-instance-when-started-from-file",
		"--no-playlist-enqueue",
		"--playlist-autostart",
		"--start-paused",
		"--no-random",
		"--no-loop",
		"--no-repeat",
		"--no-play-and-exit",
		"--no-video-title-show",
	}
	got := launchArgs("windows", 41234, "deadbeef")
	assertArgs(t, got, want)
}

// macOS : mêmes drapeaux MOINS la famille « instance unique ». VLC ne les
// déclare que sur Windows (et Linux+D-Bus) ; le VLC macOS refuse de démarrer
// sur chacun des trois — « unknown option or missing mandatory argument »,
// vérifié un par un sur VLC 3.0.23. Le client Swift exclut les mêmes.
func TestLaunchArgsGeleDarwin(t *testing.T) {
	want := []string{
		"--extraintf=http",
		"--lua-intf=http",
		"--http-host=127.0.0.1",
		"--http-port=41234",
		"--http-password=deadbeef",
		"--playlist-autostart",
		"--start-paused",
		"--no-random",
		"--no-loop",
		"--no-repeat",
		"--no-play-and-exit",
		"--no-video-title-show",
	}
	assertArgs(t, launchArgs("darwin", 41234, "deadbeef"), want)
	// Un OS inconnu (Linux du harnais) est traité comme macOS : on ne parie
	// pas sur un VLC compilé avec D-Bus.
	assertArgs(t, launchArgs("linux", 41234, "deadbeef"), want)
}

// Les formes positives sont des pièges : `--one-instance` seul est réactivé
// par VLC quand le média vient d'un fichier, et `--playlist-enqueue` enfile le
// média au lieu de l'ouvrir. Aucune ne doit jamais apparaître.
func TestLaunchArgsPasDeFormePositive(t *testing.T) {
	interdits := []string{"--one-instance", "--one-instance-when-started-from-file", "--playlist-enqueue"}
	for _, goos := range []string{"windows", "darwin", "linux"} {
		for _, arg := range launchArgs(goos, 41234, "deadbeef") {
			for _, mauvais := range interdits {
				if arg == mauvais {
					t.Fatalf("%s: drapeau interdit présent : %s", goos, arg)
				}
			}
		}
	}
}

// L'interface HTTP n'écoute que la loopback et le mot de passe est bien celui
// qu'on a tiré : le port et le mot de passe sont les seules parties variables.
func TestLaunchArgsPortEtMotDePasse(t *testing.T) {
	got := launchArgs("windows", 5000, "s3cr3t")
	if !hasArg(got, "--http-port=5000") || !hasArg(got, "--http-password=s3cr3t") {
		t.Fatalf("port ou mot de passe absents : %v", got)
	}
	for _, arg := range got {
		if strings.HasPrefix(arg, "--http-host=") && arg != "--http-host=127.0.0.1" {
			t.Fatalf("interface HTTP exposée hors loopback : %s", arg)
		}
	}
}

func assertArgs(t *testing.T, got, want []string) {
	t.Helper()
	if len(got) != len(want) {
		t.Fatalf("%d drapeaux, attendu %d\n  obtenu : %v\n  attendu: %v", len(got), len(want), got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("drapeau %d = %q, attendu %q", i, got[i], want[i])
		}
	}
}

func hasArg(args []string, want string) bool {
	for _, a := range args {
		if a == want {
			return true
		}
	}
	return false
}
