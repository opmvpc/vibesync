package server

import (
	"errors"
	"fmt"
	"log/slog"
	"math"
	"strings"
	"sync"
	"time"

	"github.com/thibsix/vibesync/internal/protocol"
)

const (
	// lateThresholdSec : retard (en secondes) au-delà duquel un membre est
	// considéré comme décroché de la position de référence.
	lateThresholdSec = 4.0
	// lateSustain : durée pendant laquelle le retard doit persister avant la
	// pause automatique (le buffering, lui, déclenche immédiatement).
	lateSustain = 2 * time.Second
	// durationMismatchSec : écart de durée entre fichiers au-delà duquel on
	// avertit la salle (sans bloquer).
	durationMismatchSec = 2.0
	// rttWindow : nombre de RTT conservés pour la moyenne glissante.
	rttWindow = 5
	// usersThrottle : fréquence maximale des broadcasts `users` déclenchés par
	// les reports (qui arrivent chaque seconde et par membre).
	usersThrottle = time.Second

	maxNameLen     = 32
	maxRoomLen     = 64
	maxChatLen     = 500
	maxPositionSec = 1e7 // ~115 jours, garde-fou contre les valeurs absurdes

	// setByServer identifie les changements d'état décidés par le serveur.
	setByServer = "server"
)

// errNameTaken est renvoyé par Room.join quand le pseudo est déjà pris.
var errNameTaken = errors.New("server: pseudo déjà utilisé dans la salle")

// sink est la sortie d'un membre : une implémentation doit être non bloquante
// et sûre en concurrence (le WebSocket sérialise via une goroutine d'écriture).
type sink interface {
	send(msgType string, data any)
}

// member est l'état serveur d'un utilisateur connecté à une salle.
type member struct {
	id          string
	name        string
	ready       bool
	file        *protocol.FileInfo
	positionSec float64
	buffering   bool
	latencyMs   int64
	rtts        []int64
	// lateSince : instant depuis lequel le membre est en retard (zéro sinon).
	lateSince time.Time
	out       sink
}

func (m *member) snapshot() protocol.User {
	u := protocol.User{
		ID:          m.id,
		Name:        m.name,
		Ready:       m.ready,
		PositionSec: m.positionSec,
		LatencyMs:   m.latencyMs,
	}
	if m.file != nil {
		f := *m.file
		u.File = &f
	}
	return u
}

// Room est une salle : état autoritatif + membres. Tous les accès passent par
// mu, ce qui sérialise complètement le traitement des messages d'une salle.
type Room struct {
	name  string
	clock Clock
	log   *slog.Logger

	mu        sync.Mutex
	state     protocol.RoomState
	members   []*member
	nextID    int
	started   bool // un play a déjà été accepté → le ready-gate est levé
	lastUsers time.Time
}

func newRoom(name string, clock Clock, log *slog.Logger) *Room {
	now := clock.Now()
	return &Room{
		name:  name,
		clock: clock,
		log:   log,
		state: protocol.RoomState{
			Paused:      true,
			PositionSec: 0,
			Rate:        1,
			RefServerMs: msOf(now),
		},
	}
}

// Name renvoie le nom de la salle.
func (r *Room) Name() string { return r.name }

// State renvoie une copie de l'état autoritatif (utile aux tests).
func (r *Room) State() protocol.RoomState {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.state
}

// size renvoie le nombre de membres de la salle.
func (r *Room) size() int {
	r.mu.Lock()
	defer r.mu.Unlock()
	return len(r.members)
}

// Users renvoie la liste des membres (utile aux tests).
func (r *Room) Users() []protocol.User {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.usersLocked()
}

// --- Cycle de vie ---

// join ajoute un membre et lui envoie son welcome. Renvoie errNameTaken si le
// pseudo est déjà utilisé dans cette salle (comparaison insensible à la casse).
func (r *Room) join(name string, latencyMs int64, out sink) (*member, error) {
	r.mu.Lock()
	defer r.mu.Unlock()

	for _, m := range r.members {
		if strings.EqualFold(m.name, name) {
			return nil, errNameTaken
		}
	}

	r.nextID++
	m := &member{
		id:        fmt.Sprintf("u%d", r.nextID),
		name:      name,
		latencyMs: latencyMs,
		out:       out,
	}
	r.members = append(r.members, m)

	now := r.clock.Now()
	m.out.send(protocol.TypeWelcome, protocol.Welcome{
		SelfID: m.id,
		Room:   r.name,
		State:  r.state,
		Users:  r.usersLocked(),
	})
	for _, other := range r.members {
		if other != m {
			other.out.send(protocol.TypeToast, protocol.Toast{
				Level: protocol.LevelInfo,
				Text:  fmt.Sprintf("%s a rejoint la salle", m.name),
			})
		}
	}
	r.broadcastUsersLocked(now)
	r.log.Info("membre rejoint", "room", r.name, "user", m.id, "name", m.name)
	return m, nil
}

// leave retire un membre ; renvoie true si la salle est désormais vide.
// Si la salle était en lecture, la déconnexion déclenche une pause auto.
func (r *Room) leave(m *member) bool {
	r.mu.Lock()
	defer r.mu.Unlock()

	idx := -1
	for i, other := range r.members {
		if other == m {
			idx = i
			break
		}
	}
	if idx < 0 {
		return len(r.members) == 0
	}
	r.members = append(r.members[:idx:idx], r.members[idx+1:]...)

	now := r.clock.Now()
	r.broadcastLocked(protocol.TypeToast, protocol.Toast{
		Level: protocol.LevelInfo,
		Text:  fmt.Sprintf("%s a quitté la salle", m.name),
	})
	if !r.state.Paused && len(r.members) > 0 {
		r.autoPauseLocked(now, fmt.Sprintf("Pause auto : %s s'est déconnecté", m.name))
	}
	r.broadcastUsersLocked(now)
	r.log.Info("membre parti", "room", r.name, "user", m.id, "restants", len(r.members))
	return len(r.members) == 0
}

// --- Handlers de messages ---

// handlePing répond immédiatement au ping applicatif (offset d'horloge client).
func (r *Room) handlePing(m *member, p protocol.Ping) {
	m.out.send(protocol.TypePong, protocol.Pong{T: p.T, ServerMs: msOf(r.clock.Now())})
}

// observeRTT enregistre un RTT mesuré sur la connexion et met à jour la latence
// estimée du membre (moyenne glissante des rttWindow dernières mesures).
func (r *Room) observeRTT(m *member, rtt time.Duration) {
	r.mu.Lock()
	defer r.mu.Unlock()

	ms := rtt.Milliseconds()
	if ms < 0 {
		ms = 0
	}
	m.rtts = append(m.rtts, ms)
	if len(m.rtts) > rttWindow {
		m.rtts = m.rtts[len(m.rtts)-rttWindow:]
	}
	var sum int64
	for _, v := range m.rtts {
		sum += v
	}
	m.latencyMs = sum / int64(len(m.rtts))
	r.broadcastUsersThrottledLocked(r.clock.Now())
}

func (r *Room) handleSetReady(m *member, msg protocol.SetReady) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if m.ready == msg.Ready {
		return
	}
	m.ready = msg.Ready
	r.broadcastUsersLocked(r.clock.Now())
}

func (r *Room) handleSetFile(m *member, msg protocol.SetFile) {
	r.mu.Lock()
	defer r.mu.Unlock()

	name := strings.TrimSpace(msg.Name)
	if name == "" {
		return
	}
	m.file = &protocol.FileInfo{
		Name:        truncate(name, 200),
		DurationSec: sanitizeFloat(msg.DurationSec),
		SizeBytes:   msg.SizeBytes,
	}
	now := r.clock.Now()
	r.broadcastUsersLocked(now)

	// Avertissement (non bloquant) si un autre membre a une durée trop
	// différente. On n'avertit qu'au moment où un fichier est (re)déclaré.
	for _, other := range r.members {
		if other == m || other.file == nil || m.file.DurationSec <= 0 || other.file.DurationSec <= 0 {
			continue
		}
		if math.Abs(other.file.DurationSec-m.file.DurationSec) > durationMismatchSec {
			r.broadcastLocked(protocol.TypeToast, protocol.Toast{
				Level: protocol.LevelWarn,
				Text: fmt.Sprintf("Fichiers de durées différentes : %s (%.0f s) vs %s (%.0f s)",
					m.name, m.file.DurationSec, other.name, other.file.DurationSec),
			})
			break
		}
	}
}

// handleControl applique une action volontaire de l'utilisateur à l'état
// autoritatif, avec compensation de la latence estimée de l'émetteur.
func (r *Room) handleControl(m *member, msg protocol.Control) {
	r.mu.Lock()
	defer r.mu.Unlock()

	now := r.clock.Now()
	nowMs := msOf(now)
	pos := sanitizeFloat(msg.PositionSec)
	// La commande a mis ~RTT/2 à nous parvenir : on recale la référence dans le
	// passé pour que la position courante calculée intègre ce trajet.
	compensationMs := m.latencyMs / 2

	switch msg.Action {
	case protocol.ActionPlay:
		if !r.started {
			if missing := r.notReadyLocked(); len(missing) > 0 {
				m.out.send(protocol.TypeToast, protocol.Toast{
					Level: protocol.LevelWarn,
					Text:  fmt.Sprintf("Lecture bloquée : en attente de %s", strings.Join(missing, ", ")),
				})
				// On renvoie l'état courant à l'émetteur seul pour qu'il
				// annule sa lecture locale.
				m.out.send(protocol.TypeRoomState, r.state)
				r.log.Debug("play refusé (ready-gate)", "room", r.name, "user", m.id)
				return
			}
			r.started = true
		}
		r.state.Paused = false
		r.state.PositionSec = pos
		r.state.Rate = 1
		r.state.RefServerMs = nowMs - compensationMs
		r.state.SetBy = m.id
	case protocol.ActionPause:
		r.state.Paused = true
		r.state.PositionSec = pos
		r.state.Rate = 1
		r.state.RefServerMs = nowMs
		r.state.SetBy = m.id
	case protocol.ActionSeek:
		r.state.PositionSec = pos
		r.state.Rate = 1
		if r.state.Paused {
			r.state.RefServerMs = nowMs
		} else {
			r.state.RefServerMs = nowMs - compensationMs
		}
		r.state.SetBy = m.id
	default:
		r.log.Debug("action de control inconnue", "room", r.name, "user", m.id, "action", msg.Action)
		return
	}

	r.resetLatenessLocked()
	r.broadcastLocked(protocol.TypeRoomState, r.state)
	r.log.Debug("control appliqué", "room", r.name, "user", m.id, "action", msg.Action,
		"paused", r.state.Paused, "positionSec", r.state.PositionSec)
}

// handleReport enregistre l'état observé d'un client et déclenche si besoin la
// pause automatique (buffering immédiat, retard soutenu > lateSustain).
func (r *Room) handleReport(m *member, msg protocol.Report) {
	r.mu.Lock()
	defer r.mu.Unlock()

	now := r.clock.Now()
	m.positionSec = sanitizeFloat(msg.PositionSec)
	m.buffering = msg.Buffering

	if r.state.Paused {
		m.lateSince = time.Time{}
		r.broadcastUsersThrottledLocked(now)
		return
	}

	if msg.Buffering {
		m.lateSince = time.Time{}
		r.autoPauseLocked(now, fmt.Sprintf("Pause auto : %s bufferise", m.name))
		r.broadcastUsersLocked(now)
		return
	}

	lateness := r.positionAtLocked(msOf(now)) - m.positionSec
	switch {
	case lateness <= lateThresholdSec:
		m.lateSince = time.Time{}
	case m.lateSince.IsZero():
		m.lateSince = now
	case now.Sub(m.lateSince) > lateSustain:
		r.autoPauseLocked(now, fmt.Sprintf("Pause auto : %s a %.1f s de retard", m.name, lateness))
		r.broadcastUsersLocked(now)
		return
	}
	r.broadcastUsersThrottledLocked(now)
}

func (r *Room) handleChat(m *member, msg protocol.Chat) {
	text := strings.TrimSpace(msg.Text)
	if text == "" {
		return
	}
	text = truncate(text, maxChatLen)

	r.mu.Lock()
	defer r.mu.Unlock()
	r.broadcastLocked(protocol.TypeChatEvent, protocol.ChatEvent{
		From:     m.name,
		Text:     text,
		ServerMs: msOf(r.clock.Now()),
	})
}

// --- Helpers (mu détenu) ---

// positionAtLocked calcule la position de référence de la salle à nowMs.
func (r *Room) positionAtLocked(nowMs int64) float64 {
	if r.state.Paused {
		return r.state.PositionSec
	}
	return r.state.PositionSec + float64(nowMs-r.state.RefServerMs)/1000*r.state.Rate
}

// autoPauseLocked fige la salle à sa position courante et prévient tout le monde.
func (r *Room) autoPauseLocked(now time.Time, reason string) {
	nowMs := msOf(now)
	r.state.PositionSec = r.positionAtLocked(nowMs)
	r.state.Paused = true
	r.state.Rate = 1
	r.state.RefServerMs = nowMs
	r.state.SetBy = setByServer
	r.resetLatenessLocked()
	r.broadcastLocked(protocol.TypeRoomState, r.state)
	r.broadcastLocked(protocol.TypeToast, protocol.Toast{Level: protocol.LevelWarn, Text: reason})
	r.log.Info("pause automatique", "room", r.name, "raison", reason)
}

func (r *Room) resetLatenessLocked() {
	for _, m := range r.members {
		m.lateSince = time.Time{}
	}
}

func (r *Room) notReadyLocked() []string {
	var out []string
	for _, m := range r.members {
		if !m.ready {
			out = append(out, m.name)
		}
	}
	return out
}

func (r *Room) usersLocked() []protocol.User {
	out := make([]protocol.User, 0, len(r.members))
	for _, m := range r.members {
		out = append(out, m.snapshot())
	}
	return out
}

func (r *Room) broadcastLocked(msgType string, data any) {
	for _, m := range r.members {
		m.out.send(msgType, data)
	}
}

func (r *Room) broadcastUsersLocked(now time.Time) {
	r.lastUsers = now
	r.broadcastLocked(protocol.TypeUsers, protocol.UsersMsg{Users: r.usersLocked()})
}

func (r *Room) broadcastUsersThrottledLocked(now time.Time) {
	if !r.lastUsers.IsZero() && now.Sub(r.lastUsers) < usersThrottle {
		return
	}
	r.broadcastUsersLocked(now)
}

// sanitizeFloat neutralise NaN/Inf et borne les positions à des valeurs sensées.
func sanitizeFloat(v float64) float64 {
	if math.IsNaN(v) || math.IsInf(v, 0) {
		return 0
	}
	if v < 0 {
		return 0
	}
	if v > maxPositionSec {
		return maxPositionSec
	}
	return v
}

func truncate(s string, max int) string {
	runes := []rune(s)
	if len(runes) <= max {
		return s
	}
	return string(runes[:max])
}
