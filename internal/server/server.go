// Package server implémente le serveur vibesync : salles, état autoritatif et
// transport WebSocket décrits dans docs/protocol.md.
package server

import (
	"crypto/sha256"
	"log/slog"
	"net/http"
	"os"
	"strconv"
	"strings"
	"sync/atomic"
	"time"

	"github.com/thibsix/vibesync/internal/ws"
)

// Plafonds anti-abus par défaut (§Comportements serveur, point 6).
const (
	defaultMaxClients  = 200
	defaultMaxRooms    = 50
	defaultMaxRoomSize = 20

	// Budget de messages par connexion : ~20 msg/s, rafale de 40.
	msgRatePerSec = 20
	msgRateBurst  = 40
	// Budget spécifique au chat : ~5 msg/s, rafale de 10.
	chatRatePerSec = 5
	chatRateBurst  = 10
)

// Config regroupe la configuration du serveur (alimentée par l'environnement).
type Config struct {
	// Addr est l'adresse d'écoute HTTP (défaut ":8080").
	Addr string
	// Password est le mot de passe global optionnel exigé dans le hello.
	Password string
	// MaxClients est le nombre maximal de connexions simultanées (défaut 200).
	MaxClients int
	// MaxRooms est le nombre maximal de salles vivantes (défaut 50).
	MaxRooms int
	// MaxRoomSize est le nombre maximal de membres par salle (défaut 20).
	MaxRoomSize int
}

// Server expose les routes HTTP et détient le hub de salles.
type Server struct {
	cfg   Config
	hub   *Hub
	clock Clock
	log   *slog.Logger

	// passwordHash : condensat SHA-256 du mot de passe attendu ; on compare des
	// condensats de taille fixe pour ne pas fuiter la longueur du secret.
	passwordHash [sha256.Size]byte
	hasPassword  bool

	clients atomic.Int64
}

// Option personnalise le serveur (horloge injectable, logger).
type Option func(*Server)

// WithClock injecte une horloge (tests déterministes).
func WithClock(c Clock) Option {
	return func(s *Server) {
		if c != nil {
			s.clock = c
		}
	}
}

// WithLogger injecte un logger.
func WithLogger(l *slog.Logger) Option {
	return func(s *Server) {
		if l != nil {
			s.log = l
		}
	}
}

// New construit un serveur prêt à servir.
func New(cfg Config, opts ...Option) *Server {
	if cfg.Addr == "" {
		cfg.Addr = ":8080"
	}
	if cfg.MaxClients <= 0 {
		cfg.MaxClients = defaultMaxClients
	}
	if cfg.MaxRooms <= 0 {
		cfg.MaxRooms = defaultMaxRooms
	}
	if cfg.MaxRoomSize <= 0 {
		cfg.MaxRoomSize = defaultMaxRoomSize
	}
	s := &Server{
		cfg:   cfg,
		clock: systemClock{},
		log:   slog.Default(),
	}
	if cfg.Password != "" {
		s.passwordHash = sha256.Sum256([]byte(cfg.Password))
		s.hasPassword = true
	}
	for _, opt := range opts {
		opt(s)
	}
	s.hub = newHub(s.clock, s.log, cfg.MaxRooms, cfg.MaxRoomSize)
	return s
}

// Addr renvoie l'adresse d'écoute configurée.
func (s *Server) Addr() string { return s.cfg.Addr }

// Handler renvoie le routeur HTTP (`/ws` et `/healthz`).
func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", s.handleHealthz)
	mux.HandleFunc("/ws", s.handleWS)
	return mux
}

func (s *Server) handleHealthz(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		w.Header().Set("Allow", "GET, HEAD")
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	if r.Method == http.MethodGet {
		_, _ = w.Write([]byte("ok"))
	}
}

func (s *Server) handleWS(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		w.Header().Set("Allow", "GET")
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	// Plafond de connexions simultanées : refus avant l'upgrade (le client
	// applique son backoff de reconnexion).
	if n := s.clients.Add(1); n > int64(s.cfg.MaxClients) {
		s.clients.Add(-1)
		s.log.Warn("plafond de connexions atteint", "max", s.cfg.MaxClients, "remote", r.RemoteAddr)
		w.Header().Set("Retry-After", "5")
		http.Error(w, "server full", http.StatusServiceUnavailable)
		return
	}
	defer s.clients.Add(-1)

	// Les clients sont des applications desktop, pas des navigateurs porteurs
	// de cookies : aucun contrôle d'origine à assurer ici.
	conn, err := ws.Upgrade(w, r)
	if err != nil {
		// Upgrade a déjà répondu au client.
		s.log.Debug("upgrade websocket échoué", "err", err, "remote", r.RemoteAddr)
		return
	}
	c := newWSClient(s, conn, r.RemoteAddr)
	go c.writePump()
	c.readPump()
}

// ConfigFromEnv lit VIBESYNC_ADDR, VIBESYNC_PASSWORD et les plafonds anti-abus
// VIBESYNC_MAX_CLIENTS, VIBESYNC_MAX_ROOMS, VIBESYNC_MAX_ROOM_SIZE.
func ConfigFromEnv() Config {
	cfg := Config{
		Addr:        strings.TrimSpace(os.Getenv("VIBESYNC_ADDR")),
		Password:    os.Getenv("VIBESYNC_PASSWORD"),
		MaxClients:  envInt("VIBESYNC_MAX_CLIENTS", defaultMaxClients),
		MaxRooms:    envInt("VIBESYNC_MAX_ROOMS", defaultMaxRooms),
		MaxRoomSize: envInt("VIBESYNC_MAX_ROOM_SIZE", defaultMaxRoomSize),
	}
	if cfg.Addr == "" {
		cfg.Addr = ":8080"
	}
	return cfg
}

// envInt lit un entier positif ; toute valeur absente, illisible ou ≤ 0 rend la
// valeur par défaut.
func envInt(key string, def int) int {
	raw := strings.TrimSpace(os.Getenv(key))
	if raw == "" {
		return def
	}
	n, err := strconv.Atoi(raw)
	if err != nil || n <= 0 {
		slog.Default().Warn("valeur d'environnement ignorée", "cle", key, "valeur", raw, "defaut", def)
		return def
	}
	return n
}

// LogLevelFromEnv traduit VIBESYNC_LOG (debug|info|warn|error) ; info par défaut.
func LogLevelFromEnv() slog.Level {
	switch strings.ToLower(strings.TrimSpace(os.Getenv("VIBESYNC_LOG"))) {
	case "debug":
		return slog.LevelDebug
	case "warn", "warning":
		return slog.LevelWarn
	case "error":
		return slog.LevelError
	default:
		return slog.LevelInfo
	}
}

// ShutdownGrace est le délai laissé aux connexions lors d'un arrêt propre.
const ShutdownGrace = 5 * time.Second
