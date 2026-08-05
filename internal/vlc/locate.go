package vlc

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
)

// EnvBinary permet de forcer le chemin de l'exécutable VLC.
const EnvBinary = "VIBESYNC_VLC"

// Locate cherche l'exécutable VLC : la variable d'environnement VIBESYNC_VLC
// est prioritaire, puis les emplacements standards de l'OS, puis le PATH.
func Locate() (string, error) {
	return locate(runtime.GOOS, os.Getenv, exec.LookPath, isRegularFile)
}

func locate(goos string, getenv func(string) string, lookPath func(string) (string, error), exists func(string) bool) (string, error) {
	if forced := getenv(EnvBinary); forced != "" {
		if exists(forced) {
			return forced, nil
		}
		return "", fmt.Errorf("%w: %s=%q ne pointe sur aucun fichier", ErrNotFound, EnvBinary, forced)
	}
	for _, cand := range candidates(goos, getenv) {
		if cand != "" && exists(cand) {
			return cand, nil
		}
	}
	if p, err := lookPath(binaryName(goos)); err == nil {
		return p, nil
	}
	return "", fmt.Errorf("%w (installez VLC ou renseignez %s)", ErrNotFound, EnvBinary)
}

func binaryName(goos string) string {
	if goos == "windows" {
		return "vlc.exe"
	}
	return "vlc"
}

// candidates liste les emplacements standards de VLC pour un OS donné
// (les entrées vides, faute de variable d'environnement, sont filtrées).
func candidates(goos string, getenv func(string) string) []string {
	join := func(base string, parts ...string) string {
		if base == "" {
			return ""
		}
		return filepath.Join(append([]string{base}, parts...)...)
	}
	out := make([]string, 0, 8)
	for _, c := range rawCandidates(goos, getenv, join) {
		if c != "" {
			out = append(out, c)
		}
	}
	return out
}

func rawCandidates(goos string, getenv func(string) string, join func(string, ...string) string) []string {
	switch goos {
	case "windows":
		return []string{
			`C:\Program Files\VideoLAN\VLC\vlc.exe`,
			`C:\Program Files (x86)\VideoLAN\VLC\vlc.exe`,
			join(getenv("ProgramFiles"), "VideoLAN", "VLC", "vlc.exe"),
			join(getenv("ProgramFiles(x86)"), "VideoLAN", "VLC", "vlc.exe"),
			join(getenv("ProgramW6432"), "VideoLAN", "VLC", "vlc.exe"),
			join(getenv("LOCALAPPDATA"), "Programs", "VideoLAN", "VLC", "vlc.exe"),
			join(getenv("LOCALAPPDATA"), "Programs", "VLC", "vlc.exe"),
		}
	case "darwin":
		return []string{
			"/Applications/VLC.app/Contents/MacOS/VLC",
			join(getenv("HOME"), "Applications", "VLC.app", "Contents", "MacOS", "VLC"),
			"/Applications/VLC/VLC.app/Contents/MacOS/VLC",
			"/opt/homebrew/bin/vlc",
			"/usr/local/bin/vlc",
		}
	default:
		return []string{
			"/usr/bin/vlc",
			"/usr/local/bin/vlc",
			"/snap/bin/vlc",
			"/var/lib/flatpak/exports/bin/org.videolan.VLC",
		}
	}
}

func isRegularFile(path string) bool {
	fi, err := os.Stat(path)
	return err == nil && !fi.IsDir()
}
