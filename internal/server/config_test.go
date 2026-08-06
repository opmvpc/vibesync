package server

import (
	"crypto/sha256"
	"log/slog"
	"testing"
	"time"
)

func TestConfigFromEnv(t *testing.T) {
	t.Setenv("VIBESYNC_ADDR", "")
	t.Setenv("VIBESYNC_PASSWORD", "")
	t.Setenv("VIBESYNC_MAX_CLIENTS", "")
	t.Setenv("VIBESYNC_MAX_ROOMS", "")
	t.Setenv("VIBESYNC_MAX_ROOM_SIZE", "")
	t.Setenv("VIBESYNC_ROOM_LINGER", "")
	t.Setenv("VIBESYNC_DOWNLOAD_URL", "")
	got := ConfigFromEnv()
	if got.DownloadURL != defaultDownloadURL {
		t.Fatalf("lien de téléchargement par défaut attendu, obtenu %q", got.DownloadURL)
	}
	if got.Addr != ":8080" || got.Password != "" {
		t.Fatalf("config par défaut inattendue: %+v", got)
	}
	if got.MaxClients != defaultMaxClients || got.MaxRooms != defaultMaxRooms || got.MaxRoomSize != defaultMaxRoomSize {
		t.Fatalf("plafonds par défaut inattendus: %+v", got)
	}
	if got.RoomLinger != defaultRoomLinger {
		t.Fatalf("fenêtre de reprise par défaut attendue %s, obtenue %s", defaultRoomLinger, got.RoomLinger)
	}

	t.Setenv("VIBESYNC_ADDR", " 127.0.0.1:9999 ")
	t.Setenv("VIBESYNC_PASSWORD", "s3cret")
	t.Setenv("VIBESYNC_MAX_CLIENTS", "10")
	t.Setenv("VIBESYNC_MAX_ROOMS", " 3 ")
	t.Setenv("VIBESYNC_MAX_ROOM_SIZE", "4")
	t.Setenv("VIBESYNC_ROOM_LINGER", " 45m ")
	t.Setenv("VIBESYNC_DOWNLOAD_URL", " https://exemple.test/dl ")
	got = ConfigFromEnv()
	if got.DownloadURL != "https://exemple.test/dl" {
		t.Fatalf("lien de téléchargement inattendu: %q", got.DownloadURL)
	}
	if got.Addr != "127.0.0.1:9999" || got.Password != "s3cret" {
		t.Fatalf("config inattendue: %+v", got)
	}
	if got.MaxClients != 10 || got.MaxRooms != 3 || got.MaxRoomSize != 4 {
		t.Fatalf("plafonds inattendus: %+v", got)
	}
	if got.RoomLinger != 45*time.Minute {
		t.Fatalf("fenêtre de reprise attendue 45m, obtenue %s", got.RoomLinger)
	}

	// Valeurs illisibles ou ≤ 0 → défauts.
	t.Setenv("VIBESYNC_MAX_CLIENTS", "beaucoup")
	t.Setenv("VIBESYNC_MAX_ROOMS", "0")
	t.Setenv("VIBESYNC_MAX_ROOM_SIZE", "-5")
	for _, invalide := range []string{"longtemps", "0", "-5m", "30"} {
		t.Setenv("VIBESYNC_ROOM_LINGER", invalide)
		got = ConfigFromEnv()
		if got.MaxClients != defaultMaxClients || got.MaxRooms != defaultMaxRooms || got.MaxRoomSize != defaultMaxRoomSize {
			t.Fatalf("valeurs invalides : défauts attendus, obtenu %+v", got)
		}
		if got.RoomLinger != defaultRoomLinger {
			t.Fatalf("VIBESYNC_ROOM_LINGER=%q : défaut attendu, obtenu %s", invalide, got.RoomLinger)
		}
	}
}

func TestLogLevelFromEnv(t *testing.T) {
	cases := map[string]slog.Level{
		"":               slog.LevelInfo,
		"info":           slog.LevelInfo,
		"DEBUG":          slog.LevelDebug,
		" warn ":         slog.LevelWarn,
		"warning":        slog.LevelWarn,
		"error":          slog.LevelError,
		"n'importe quoi": slog.LevelInfo,
	}
	for value, want := range cases {
		t.Setenv("VIBESYNC_LOG", value)
		if got := LogLevelFromEnv(); got != want {
			t.Fatalf("VIBESYNC_LOG=%q : niveau attendu %v, obtenu %v", value, want, got)
		}
	}
}

func TestNewAppliqueLesDefauts(t *testing.T) {
	s := New(Config{})
	if s.Addr() != ":8080" {
		t.Fatalf("adresse par défaut attendue :8080, obtenue %q", s.Addr())
	}
	if s.clock == nil || s.log == nil || s.hub == nil {
		t.Fatal("serveur incomplètement initialisé")
	}
	if s.cfg.MaxClients != defaultMaxClients || s.hub.maxRooms != defaultMaxRooms ||
		s.hub.maxRoomSize != defaultMaxRoomSize {
		t.Fatalf("plafonds par défaut non appliqués: %+v", s.cfg)
	}
	if s.hasPassword {
		t.Fatal("aucun mot de passe attendu")
	}
	if s.hub.linger != defaultRoomLinger {
		t.Fatalf("fenêtre de reprise par défaut attendue %s, obtenue %s", defaultRoomLinger, s.hub.linger)
	}
	if s.Version() != defaultVersion || s.hub.info.downloadURL != defaultDownloadURL {
		t.Fatalf("version/lien par défaut attendus, obtenus %q / %q",
			s.Version(), s.hub.info.downloadURL)
	}
	// Valeur négative : destruction immédiate, pas de retour au défaut.
	if h := New(Config{RoomLinger: RoomLingerDisabled}).hub; h.linger > 0 {
		t.Fatalf("RoomLingerDisabled doit désactiver la conservation, obtenu %s", h.linger)
	}
}

func TestNewHacheLeMotDePasse(t *testing.T) {
	s := New(Config{Password: "s3cret"})
	if !s.hasPassword {
		t.Fatal("mot de passe attendu actif")
	}
	want := sha256.Sum256([]byte("s3cret"))
	if s.passwordHash != want {
		t.Fatal("le condensat SHA-256 du mot de passe doit être précalculé")
	}
	// La taille fixe du condensat ([32]byte) garantit par construction que la
	// longueur du secret ne fuite pas — rien à tester dynamiquement.
}
