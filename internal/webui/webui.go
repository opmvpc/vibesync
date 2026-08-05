// Package webui sert la GUI locale (mode debug / implémentation de référence)
// sur 127.0.0.1 et expose le canal WebSocket /ui : c'est le contrat que les
// UIs natives réimplémenteront (cf. docs/research/2026-08-05-ui-protocol-draft.md).
package webui

import (
	"context"
	"crypto/rand"
	"crypto/subtle"
	"embed"
	"encoding/hex"
	"fmt"
	"io/fs"
	"net"
	"net/http"
	"os/exec"
	"runtime"
	"strings"
	"time"

	"github.com/thibsix/vibesync/internal/client"
)

//go:embed web
var assets embed.FS

// Engine est ce que l'UI attend du moteur de synchronisation.
type Engine interface {
	Connect(req client.ConnectRequest)
	Disconnect()
	OpenFile(ctx context.Context, path string) error
	SetReady(ready bool)
	Play()
	Pause()
	Seek(positionSec float64)
	Chat(text string)
	Snapshot() client.Snapshot
	Subscribe() (<-chan client.Event, func())
}

// Options paramètre le serveur local.
type Options struct {
	// Addr d'écoute (défaut "127.0.0.1:0").
	Addr string
	// Token exigé en query param sur /ui ; vide = généré aléatoirement.
	Token string
}

// Server est le serveur HTTP local (UI + canal /ui).
type Server struct {
	eng   Engine
	token string
	mux   *http.ServeMux
	srv   *http.Server
	ln    net.Listener
	index []byte
}

// New construit le serveur local.
func New(eng Engine, opts Options) (*Server, error) {
	token := opts.Token
	if token == "" {
		buf := make([]byte, 16)
		if _, err := rand.Read(buf); err != nil {
			return nil, fmt.Errorf("webui: génération du token: %w", err)
		}
		token = hex.EncodeToString(buf)
	}
	index, err := assets.ReadFile("web/index.html")
	if err != nil {
		return nil, fmt.Errorf("webui: index embarqué introuvable: %w", err)
	}
	s := &Server{
		eng:   eng,
		token: token,
		mux:   http.NewServeMux(),
		index: []byte(strings.ReplaceAll(string(index), "__UI_TOKEN__", token)),
	}
	sub, err := fs.Sub(assets, "web")
	if err != nil {
		return nil, fmt.Errorf("webui: assets: %w", err)
	}
	s.mux.HandleFunc("/", s.handleIndex)
	s.mux.Handle("/static/", http.StripPrefix("/static/", http.FileServer(http.FS(sub))))
	s.mux.HandleFunc("/api/fs", s.handleFS)
	s.mux.HandleFunc("/ui", s.handleUI)
	s.srv = &http.Server{Handler: s.mux, ReadHeaderTimeout: 10 * time.Second}
	return s, nil
}

// Handler expose le routeur (tests).
func (s *Server) Handler() http.Handler { return s.mux }

// Token est le jeton attendu sur /ui.
func (s *Server) Token() string { return s.token }

// Start écoute et sert en arrière-plan.
func (s *Server) Start(opts Options) error {
	addr := opts.Addr
	if addr == "" {
		addr = "127.0.0.1:0"
	}
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return fmt.Errorf("webui: écoute sur %s: %w", addr, err)
	}
	s.ln = ln
	go func() { _ = s.srv.Serve(ln) }()
	return nil
}

// Port est le port d'écoute effectif.
func (s *Server) Port() int {
	if s.ln == nil {
		return 0
	}
	if a, ok := s.ln.Addr().(*net.TCPAddr); ok {
		return a.Port
	}
	return 0
}

// URL est l'adresse de l'UI (token inclus).
func (s *Server) URL() string {
	return fmt.Sprintf("http://127.0.0.1:%d/?token=%s", s.Port(), s.token)
}

// Shutdown arrête le serveur local.
func (s *Server) Shutdown(ctx context.Context) error { return s.srv.Shutdown(ctx) }

func (s *Server) handleIndex(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	_, _ = w.Write(s.index)
}

// handleFS expose l'explorateur de fichiers du backend (le navigateur ne donne
// pas les chemins complets). Doublon pratique de la commande WS `browse`.
func (s *Server) handleFS(w http.ResponseWriter, r *http.Request) {
	if !s.authorized(r) {
		http.Error(w, "token invalide", http.StatusUnauthorized)
		return
	}
	listing, err := Browse(r.URL.Query().Get("path"))
	if err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, http.StatusOK, listing)
}

func (s *Server) authorized(r *http.Request) bool {
	got := r.URL.Query().Get("token")
	if got == "" {
		got = strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")
	}
	return subtle.ConstantTimeCompare([]byte(got), []byte(s.token)) == 1
}

func checkLocalOrigin(r *http.Request) bool {
	origin := r.Header.Get("Origin")
	if origin == "" {
		return true // client natif, pas un navigateur
	}
	return strings.HasPrefix(origin, "http://127.0.0.1:") || strings.HasPrefix(origin, "http://localhost:")
}

func goos() string { return runtime.GOOS }

// OpenBrowser ouvre l'URL dans le navigateur par défaut.
func OpenBrowser(url string) error {
	var cmd *exec.Cmd
	switch runtime.GOOS {
	case "windows":
		cmd = exec.Command("rundll32", "url.dll,FileProtocolHandler", url)
	case "darwin":
		cmd = exec.Command("open", url)
	default:
		cmd = exec.Command("xdg-open", url)
	}
	if err := cmd.Start(); err != nil {
		return fmt.Errorf("webui: ouverture du navigateur: %w", err)
	}
	go func() { _ = cmd.Wait() }()
	return nil
}
