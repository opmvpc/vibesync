package webui

import (
	"os"
	"path/filepath"
	"runtime"
	"testing"
)

func tempTree(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	must := func(err error) {
		t.Helper()
		if err != nil {
			t.Fatal(err)
		}
	}
	must(os.Mkdir(filepath.Join(dir, "Zeta série"), 0o755))
	must(os.Mkdir(filepath.Join(dir, "alpha"), 0o755))
	must(os.Mkdir(filepath.Join(dir, ".cache"), 0o755))
	for _, name := range []string{"b.mkv", "A.mp4", "notes.txt", "cover.jpg", ".secret.mkv"} {
		must(os.WriteFile(filepath.Join(dir, name), []byte("0123456789"), 0o600))
	}
	return dir
}

func names(entries []Entry) []string {
	out := make([]string, len(entries))
	for i, e := range entries {
		out[i] = e.Name
	}
	return out
}

func TestBrowseFiltreEtTri(t *testing.T) {
	dir := tempTree(t)
	home := func() (string, error) { return dir, nil }
	listing, err := browse(dir, "linux", home)
	if err != nil {
		t.Fatalf("browse: %v", err)
	}
	got := names(listing.Entries)
	want := []string{"alpha", "Zeta série", "A.mp4", "b.mkv"}
	if len(got) != len(want) {
		t.Fatalf("entrées = %v, attendu %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("entrées = %v, attendu %v (dossiers d'abord, puis vidéos triées)", got, want)
		}
	}
	for _, e := range listing.Entries {
		if !e.IsDir && e.SizeBytes != 10 {
			t.Fatalf("taille de %q = %d", e.Name, e.SizeBytes)
		}
		if !filepath.IsAbs(e.Path) {
			t.Fatalf("chemin non absolu: %q", e.Path)
		}
	}
	if listing.Parent == "" {
		t.Fatal("parent manquant")
	}
	if len(listing.Roots) == 0 {
		t.Fatal("aucune racine proposée")
	}
}

func TestBrowseNavigation(t *testing.T) {
	dir := tempTree(t)
	home := func() (string, error) { return dir, nil }
	sub := filepath.Join(dir, "alpha")
	if err := os.WriteFile(filepath.Join(sub, "film.avi"), []byte("x"), 0o600); err != nil {
		t.Fatal(err)
	}
	listing, err := browse(sub, "linux", home)
	if err != nil {
		t.Fatalf("browse: %v", err)
	}
	if got := names(listing.Entries); len(got) != 1 || got[0] != "film.avi" {
		t.Fatalf("entrées = %v", got)
	}
	if listing.Parent != dir {
		t.Fatalf("parent = %q, attendu %q", listing.Parent, dir)
	}
	// Remonter par le parent redonne le dossier de départ.
	up, err := browse(listing.Parent, "linux", home)
	if err != nil {
		t.Fatalf("browse parent: %v", err)
	}
	if up.Path != dir {
		t.Fatalf("path = %q, attendu %q", up.Path, dir)
	}
}

func TestBrowseCheminVideUtiliseLeHome(t *testing.T) {
	dir := tempTree(t)
	listing, err := browse("", "linux", func() (string, error) { return dir, nil })
	if err != nil {
		t.Fatalf("browse: %v", err)
	}
	if listing.Path != dir {
		t.Fatalf("path = %q, attendu le home %q", listing.Path, dir)
	}
}

func TestBrowseDossierInexistant(t *testing.T) {
	dir := t.TempDir()
	_, err := browse(filepath.Join(dir, "absent"), "linux", func() (string, error) { return dir, nil })
	if err == nil {
		t.Fatal("un dossier inexistant devrait être une erreur")
	}
}

func TestRootsParOS(t *testing.T) {
	home := func() (string, error) { return "/home/thib", nil }
	if got := rootsFor("darwin", home); len(got) < 2 || got[0].Path != "/home/thib" {
		t.Fatalf("racines macOS inattendues: %+v", got)
	}
	if got := rootsFor("linux", home); len(got) < 2 {
		t.Fatalf("racines linux inattendues: %+v", got)
	}
	if runtime.GOOS == "windows" {
		win := rootsFor("windows", home)
		var hasDrive bool
		for _, r := range win {
			if len(r.Name) == 2 && r.Name[1] == ':' {
				hasDrive = true
			}
		}
		if !hasDrive {
			t.Fatalf("aucun lecteur Windows listé: %+v", win)
		}
	}
}

func TestIsVideo(t *testing.T) {
	for _, name := range []string{"a.mkv", "A.MP4", "b.avi", "c.m2ts"} {
		if !IsVideo(name) {
			t.Errorf("%q devrait être une vidéo", name)
		}
	}
	for _, name := range []string{"a.txt", "b.jpg", "c", "d.mkv.part"} {
		if IsVideo(name) {
			t.Errorf("%q ne devrait pas être une vidéo", name)
		}
	}
}
