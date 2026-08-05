// Package vlctest fournit un faux VLC : un serveur httptest qui imite
// l'interface HTTP de VLC (/requests/status.json + commandes), avec une
// position qui avance selon une horloge injectable. Réutilisable par les tests
// du moteur de sync.
package vlctest

import (
	"encoding/json"
	"math"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Fake est un VLC simulé. Toutes les méthodes sont sûres en concurrence.
type Fake struct {
	mu       sync.Mutex
	now      func() time.Time
	state    string
	pos      float64
	lastAt   time.Time
	length   float64
	rate     float64
	file     string
	password string
	stalled  bool
	seeks    int
	requests int
	fail     int

	srv *httptest.Server
}

// New crée un faux VLC arrêté (state=stopped) avec l'horloge fournie
// (nil = time.Now) et démarre le serveur httptest.
func New(now func() time.Time) *Fake {
	if now == nil {
		now = time.Now
	}
	f := &Fake{
		now:      now,
		state:    "stopped",
		rate:     1,
		password: "faux-mdp",
		length:   0,
	}
	f.lastAt = now()
	mux := http.NewServeMux()
	mux.HandleFunc("/requests/status.json", f.handleStatus)
	f.srv = httptest.NewServer(mux)
	return f
}

// URL est la base HTTP du faux VLC.
func (f *Fake) URL() string { return f.srv.URL }

// Password est le mot de passe attendu en basic auth.
func (f *Fake) Password() string { return f.password }

// Close arrête le serveur httptest.
func (f *Fake) Close() { f.srv.Close() }

// LoadFile charge un média : état pause, position 0.
func (f *Fake) LoadFile(name string, lengthSec float64) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.advanceLocked()
	f.file = name
	f.length = lengthSec
	f.pos = 0
	f.state = "paused"
	f.rate = 1
}

// Play démarre la lecture (comme si l'utilisateur appuyait sur play).
func (f *Fake) Play() {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.advanceLocked()
	if f.state != "stopped" {
		f.state = "playing"
	}
}

// Pause met en pause (comme si l'utilisateur appuyait sur pause).
func (f *Fake) Pause() {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.advanceLocked()
	if f.state != "stopped" {
		f.state = "paused"
	}
}

// SeekTo déplace la position (action utilisateur).
func (f *Fake) SeekTo(sec float64) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.advanceLocked()
	f.pos = clamp(sec, 0, f.length)
}

// SetStalled simule un buffering : la position n'avance plus.
func (f *Fake) SetStalled(v bool) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.advanceLocked()
	f.stalled = v
}

// Position renvoie la position courante (secondes).
func (f *Fake) Position() float64 {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.advanceLocked()
	return f.pos
}

// State renvoie "playing", "paused" ou "stopped".
func (f *Fake) State() string {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.state
}

// Rate renvoie la vitesse de lecture courante.
func (f *Fake) Rate() float64 {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.rate
}

// Seeks compte les seeks reçus par l'interface HTTP.
func (f *Fake) Seeks() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.seeks
}

// Requests compte les requêtes servies.
func (f *Fake) Requests() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.requests
}

// FailNext fait échouer les n prochaines requêtes (HTTP 500).
func (f *Fake) FailNext(n int) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.fail = n
}

// Tick fait avancer l'état interne jusqu'à l'instant courant de l'horloge.
// Utile quand le test avance l'horloge sans interroger le faux VLC.
func (f *Fake) Tick() {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.advanceLocked()
}

func (f *Fake) advanceLocked() {
	now := f.now()
	elapsed := now.Sub(f.lastAt).Seconds()
	f.lastAt = now
	if elapsed <= 0 || f.state != "playing" || f.stalled {
		return
	}
	f.pos = clamp(f.pos+elapsed*f.rate, 0, f.length)
}

func (f *Fake) handleStatus(w http.ResponseWriter, r *http.Request) {
	user, pass, ok := r.BasicAuth()
	if !ok || user != "" || pass != f.password {
		w.Header().Set("WWW-Authenticate", `Basic realm="VLC"`)
		w.WriteHeader(http.StatusUnauthorized)
		return
	}
	f.mu.Lock()
	if f.fail > 0 {
		f.fail--
		f.mu.Unlock()
		w.WriteHeader(http.StatusInternalServerError)
		return
	}
	f.requests++
	f.advanceLocked()
	q := r.URL.Query()
	switch q.Get("command") {
	case "":
	case "pl_forcepause", "pl_pause":
		if f.state == "playing" {
			f.state = "paused"
		}
	case "pl_forceresume", "pl_play":
		if f.state == "paused" {
			f.state = "playing"
		}
	case "seek":
		if v, err := parseSeek(q.Get("val")); err == nil {
			f.pos = clamp(v, 0, f.length)
			f.seeks++
		}
	case "rate":
		if v, err := strconv.ParseFloat(q.Get("val"), 64); err == nil && v > 0 {
			f.rate = v
		}
	}
	// VLC expose `length` en secondes entières : la fraction `position` est
	// calculée avec cette même durée, sinon position × length dérive.
	length := math.Round(f.length)
	payload := map[string]any{
		"state":    f.state,
		"length":   length,
		"time":     math.Floor(f.pos),
		"rate":     f.rate,
		"position": positionRatio(f.pos, length),
		"volume":   256,
		"information": map[string]any{
			"category": map[string]any{
				"meta": map[string]any{"filename": f.file},
			},
		},
	}
	f.mu.Unlock()
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(payload)
}

func parseSeek(val string) (float64, error) {
	val = strings.TrimSpace(val)
	return strconv.ParseFloat(val, 64)
}

func positionRatio(pos, length float64) float64 {
	if length <= 0 {
		return 0
	}
	r := pos / length
	if r < 0 {
		return 0
	}
	if r > 1 {
		return 1
	}
	return r
}

func clamp(v, lo, hi float64) float64 {
	if hi > 0 && v > hi {
		return hi
	}
	if v < lo {
		return lo
	}
	return v
}
