package vlc

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestLocateEnvPrioritaire(t *testing.T) {
	dir := t.TempDir()
	bin := filepath.Join(dir, "vlc.exe")
	if err := os.WriteFile(bin, []byte("x"), 0o600); err != nil {
		t.Fatal(err)
	}
	env := func(k string) string {
		if k == EnvBinary {
			return bin
		}
		return ""
	}
	got, err := locate("windows", env, func(string) (string, error) { return "", errors.New("nope") }, isRegularFile)
	if err != nil {
		t.Fatalf("locate: %v", err)
	}
	if got != bin {
		t.Fatalf("chemin = %q, attendu %q", got, bin)
	}
}

func TestLocateEnvInvalide(t *testing.T) {
	env := func(k string) string {
		if k == EnvBinary {
			return filepath.Join(t.TempDir(), "absent.exe")
		}
		return ""
	}
	_, err := locate("windows", env, func(string) (string, error) { return "C:/vlc.exe", nil }, isRegularFile)
	if !errors.Is(err, ErrNotFound) {
		t.Fatalf("erreur = %v, attendu ErrNotFound", err)
	}
}

func TestLocateCheminsStandards(t *testing.T) {
	cases := []struct {
		goos string
		want string
	}{
		{"windows", `C:\Program Files\VideoLAN\VLC\vlc.exe`},
		{"darwin", "/Applications/VLC.app/Contents/MacOS/VLC"},
	}
	for _, tc := range cases {
		exists := func(p string) bool { return p == tc.want }
		got, err := locate(tc.goos, func(string) string { return "" }, func(string) (string, error) { return "", errors.New("nope") }, exists)
		if err != nil {
			t.Fatalf("%s: locate: %v", tc.goos, err)
		}
		if got != tc.want {
			t.Fatalf("%s: chemin = %q, attendu %q", tc.goos, got, tc.want)
		}
	}
}

func TestLocateFallbackPath(t *testing.T) {
	got, err := locate("linux", func(string) string { return "" },
		func(name string) (string, error) {
			if name != "vlc" {
				t.Fatalf("nom cherché dans le PATH = %q", name)
			}
			return "/usr/bin/vlc-from-path", nil
		},
		func(string) bool { return false })
	if err != nil {
		t.Fatalf("locate: %v", err)
	}
	if got != "/usr/bin/vlc-from-path" {
		t.Fatalf("chemin = %q", got)
	}
}

func TestLocateIntrouvable(t *testing.T) {
	_, err := locate("windows", func(string) string { return "" },
		func(string) (string, error) { return "", errors.New("nope") },
		func(string) bool { return false })
	if !errors.Is(err, ErrNotFound) {
		t.Fatalf("erreur = %v, attendu ErrNotFound", err)
	}
	if !strings.Contains(err.Error(), EnvBinary) {
		t.Fatalf("le message devrait mentionner %s: %v", EnvBinary, err)
	}
}

func TestCandidatsUtilisentLesVariablesEnv(t *testing.T) {
	env := func(k string) string {
		if k == "LOCALAPPDATA" {
			return `D:\App`
		}
		return ""
	}
	list := candidates("windows", env)
	var found bool
	for _, c := range list {
		if c == filepath.Join(`D:\App`, "Programs", "VideoLAN", "VLC", "vlc.exe") {
			found = true
		}
		if c == "" {
			t.Fatal("candidat vide non filtré à la construction")
		}
	}
	if !found {
		t.Fatalf("LOCALAPPDATA non exploité: %v", list)
	}
}
