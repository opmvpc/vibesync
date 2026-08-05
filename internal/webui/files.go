package webui

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
)

// Entry est un élément listé par l'explorateur de fichiers.
type Entry struct {
	Name      string `json:"name"`
	Path      string `json:"path"`
	IsDir     bool   `json:"isDir"`
	SizeBytes int64  `json:"sizeBytes"`
}

// Listing est le contenu d'un dossier, tel que renvoyé à l'UI.
type Listing struct {
	Path    string  `json:"path"`
	Parent  string  `json:"parent"`
	Roots   []Entry `json:"roots"`
	Entries []Entry `json:"entries"`
}

// VideoExtensions sont les extensions proposées par l'explorateur.
var VideoExtensions = map[string]bool{
	".mkv": true, ".mp4": true, ".avi": true, ".mov": true, ".m4v": true,
	".webm": true, ".wmv": true, ".flv": true, ".mpg": true, ".mpeg": true,
	".ts": true, ".m2ts": true, ".mts": true, ".ogv": true, ".ogm": true,
	".3gp": true, ".vob": true, ".divx": true, ".rmvb": true, ".asf": true,
	".m2v": true, ".mpv": true, ".iso": true,
}

// IsVideo indique si un nom de fichier a une extension vidéo courante.
func IsVideo(name string) bool {
	return VideoExtensions[strings.ToLower(filepath.Ext(name))]
}

// Browse liste un dossier : sous-dossiers d'abord, puis fichiers vidéo.
// Un chemin vide part du dossier personnel de l'utilisateur.
func Browse(path string) (Listing, error) {
	return browse(path, runtime.GOOS, os.UserHomeDir)
}

func browse(path, goos string, userHome func() (string, error)) (Listing, error) {
	roots := rootsFor(goos, userHome)
	if strings.TrimSpace(path) == "" {
		if home, err := userHome(); err == nil {
			path = home
		} else if len(roots) > 0 {
			path = roots[0].Path
		} else {
			return Listing{}, fmt.Errorf("aucun point de départ pour l'explorateur")
		}
	}
	abs, err := filepath.Abs(path)
	if err != nil {
		return Listing{}, fmt.Errorf("chemin invalide: %w", err)
	}
	items, err := os.ReadDir(abs)
	if err != nil {
		return Listing{}, fmt.Errorf("dossier illisible: %w", err)
	}
	listing := Listing{Path: abs, Parent: parentOf(abs), Roots: roots, Entries: []Entry{}}
	for _, it := range items {
		name := it.Name()
		if strings.HasPrefix(name, ".") {
			continue // fichiers cachés
		}
		full := filepath.Join(abs, name)
		if it.IsDir() {
			listing.Entries = append(listing.Entries, Entry{Name: name, Path: full, IsDir: true})
			continue
		}
		if !IsVideo(name) {
			continue
		}
		var size int64
		if fi, err := it.Info(); err == nil {
			size = fi.Size()
		}
		listing.Entries = append(listing.Entries, Entry{Name: name, Path: full, SizeBytes: size})
	}
	sort.SliceStable(listing.Entries, func(i, j int) bool {
		a, b := listing.Entries[i], listing.Entries[j]
		if a.IsDir != b.IsDir {
			return a.IsDir // dossiers d'abord
		}
		return strings.ToLower(a.Name) < strings.ToLower(b.Name)
	})
	return listing, nil
}

// parentOf renvoie le dossier parent, ou "" si on est déjà à la racine.
func parentOf(path string) string {
	parent := filepath.Dir(path)
	if parent == path {
		return ""
	}
	return parent
}

// Roots liste les points d'entrée (lecteurs Windows, volumes + home macOS).
func Roots() []Entry { return rootsFor(runtime.GOOS, os.UserHomeDir) }

func rootsFor(goos string, userHome func() (string, error)) []Entry {
	var roots []Entry
	if home, err := userHome(); err == nil && home != "" {
		roots = append(roots, Entry{Name: "Dossier personnel", Path: home, IsDir: true})
	}
	switch goos {
	case "windows":
		for c := 'A'; c <= 'Z'; c++ {
			drive := string(c) + `:\`
			if fi, err := os.Stat(drive); err == nil && fi.IsDir() {
				roots = append(roots, Entry{Name: string(c) + ":", Path: drive, IsDir: true})
			}
		}
	case "darwin":
		roots = append(roots, Entry{Name: "Macintosh HD", Path: "/", IsDir: true})
		if vols, err := os.ReadDir("/Volumes"); err == nil {
			for _, v := range vols {
				if !v.IsDir() || strings.HasPrefix(v.Name(), ".") {
					continue
				}
				roots = append(roots, Entry{Name: v.Name(), Path: filepath.Join("/Volumes", v.Name()), IsDir: true})
			}
		}
	default:
		roots = append(roots, Entry{Name: "/", Path: "/", IsDir: true})
	}
	return roots
}
