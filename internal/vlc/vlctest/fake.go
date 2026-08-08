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
	rates    int
	requests int
	fail     int

	// autoplay reproduit le comportement réel de VLC : à l'ouverture d'un
	// fichier, la lecture démarre toute seule (docs/protocol.md §Chargement de
	// fichier). loadDelay simule le temps d'ouverture du média, pendant lequel
	// VLC se déclare encore `stopped`.
	autoplay      bool
	loadDelay     time.Duration
	pendingName   string
	pendingLength float64
	pendingAt     time.Time

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
		autoplay: true,
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

// LoadFile ouvre un média. Comme VLC, la lecture démarre automatiquement à la
// fin du chargement — c'est précisément la course que le client doit gérer.
// Voir SetAutoplay et SetLoadDelay pour s'en écarter.
func (f *Fake) LoadFile(name string, lengthSec float64) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.advanceLocked()
	if f.loadDelay <= 0 {
		f.activateLocked(name, lengthSec, f.now())
		return
	}
	// Ouverture en cours : VLC n'a encore aucun média à montrer.
	f.state = "stopped"
	f.file = ""
	f.length = 0
	f.pos = 0
	f.pendingName, f.pendingLength = name, lengthSec
	f.pendingAt = f.now().Add(f.loadDelay)
}

// SetAutoplay règle le démarrage automatique à l'ouverture (vrai par défaut,
// comme VLC).
func (f *Fake) SetAutoplay(v bool) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.autoplay = v
}

// SetLoadDelay règle le temps d'ouverture d'un média : pendant ce délai, le
// faux VLC se déclare `stopped`, sans durée ni fichier.
func (f *Fake) SetLoadDelay(d time.Duration) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.loadDelay = d
}

// activateLocked rend le média disponible à l'instant at.
func (f *Fake) activateLocked(name string, lengthSec float64, at time.Time) {
	f.file = name
	f.length = lengthSec
	f.pos = 0
	f.rate = 1
	f.lastAt = at
	f.pendingAt = time.Time{}
	f.pendingName, f.pendingLength = "", 0
	if f.autoplay {
		f.state = "playing"
	} else {
		f.state = "paused"
	}
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

// SetRate change la vitesse de lecture COMME L'UTILISATEUR le ferait dans
// l'interface de VLC (le compteur Rates, lui, ne compte que les commandes
// reçues par l'interface HTTP, donc celles du moteur).
func (f *Fake) SetRate(v float64) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.advanceLocked()
	if v > 0 {
		f.rate = v
	}
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

// Rates compte les commandes `rate` reçues par l'interface HTTP. Depuis
// VS-038, la vitesse ne sert plus jamais à corriger la dérive : ce compteur
// est ce qui permet de le PROUVER (une lecture stable doit en produire zéro).
func (f *Fake) Rates() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.rates
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
	// Fin du chargement : le média devient disponible (et démarre si autoplay).
	// On l'active à sa date théorique pour que la position reste juste même si
	// personne n'a interrogé le faux VLC entre-temps.
	if !f.pendingAt.IsZero() && !now.Before(f.pendingAt) {
		f.activateLocked(f.pendingName, f.pendingLength, f.pendingAt)
	}
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
		// VLC ignore un seek tant qu'aucun média n'est ouvert.
		if v, err := parseSeek(q.Get("val")); err == nil && f.state != "stopped" {
			f.pos = clamp(v, 0, f.length)
			f.seeks++
		}
	case "rate":
		if v, err := strconv.ParseFloat(q.Get("val"), 64); err == nil && v > 0 {
			f.rate = v
			f.rates++
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
