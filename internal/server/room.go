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
	// autoPauseCooldown : intervalle minimal entre deux pauses automatiques
	// d'une même salle (§Comportements serveur 2, garde-fous). Une salle qui
	// bufferise vraiment se remet en pause d'elle-même, mais elle ne doit pas
	// noyer les commandes de l'utilisateur sous les pauses.
	autoPauseCooldown = 5 * time.Second
	// controlGrace : après son propre `control`, les reports d'un membre ne
	// peuvent plus déclencher de pause automatique pendant cette durée — son
	// seek fige mécaniquement sa position (VS-017, §Comportements serveur 2).
	controlGrace = 2 * time.Second
	// durationMismatchSec : écart de durée entre fichiers au-delà duquel on
	// avertit la salle (sans bloquer).
	durationMismatchSec = 2.0
	// rttWindow : nombre de RTT conservés pour la moyenne glissante.
	rttWindow = 5
	// usersThrottle : fréquence maximale des broadcasts `users` déclenchés par
	// les reports (qui arrivent chaque seconde et par membre).
	usersThrottle = time.Second

	maxNameLen = 32
	// maxSessionLen borne le jeton de reprise de session (16 octets hex = 32
	// caractères ; on laisse de la marge sans accepter n'importe quoi).
	maxSessionLen  = 128
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
	// evict ferme la connexion remplacée par une reprise de session
	// (§Comportements serveur, point 6). Appelée hors des verrous du hub et de
	// la salle.
	evict()
}

// member est l'état serveur d'un utilisateur connecté à une salle.
type member struct {
	id string
	// session est le jeton opaque du client (vide si le client ne le gère pas) ;
	// il autorise le remplacement d'une connexion zombie par le même client.
	session     string
	name        string
	ready       bool
	file        *protocol.FileInfo
	positionSec float64
	buffering   bool
	latencyMs   int64
	rtts        []int64
	// lateSince : instant depuis lequel le membre est en retard (zéro sinon).
	lateSince time.Time
	// lastControlAt : instant du dernier `control` accepté de ce membre ; ses
	// reports sont neutres vis-à-vis de la pause auto pendant controlGrace.
	lastControlAt time.Time
	out           sink
	// evicted marque un membre remplacé par une reprise de session : sa
	// connexion agonise peut-être encore, mais elle ne doit plus rien changer.
	evicted bool
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

// buildInfo est ce que le serveur dit de lui-même à chaque arrivant : sa
// version applicative et où télécharger un client à jour (VS-023). Immuable.
type buildInfo struct {
	version     string
	downloadURL string
}

// Room est une salle : état autoritatif + membres. Tous les accès passent par
// mu, ce qui sérialise complètement le traitement des messages d'une salle.
type Room struct {
	name  string
	clock Clock
	log   *slog.Logger
	info  buildInfo

	mu        sync.Mutex
	state     protocol.RoomState
	members   []*member
	nextID    int
	started   bool // un play a déjà été accepté → le ready-gate est levé
	lastUsers time.Time
	// lastAutoPause : dernière pause automatique, pour l'espacement minimal
	// (autoPauseCooldown).
	lastAutoPause time.Time
	// emptySince : instant où la salle s'est vidée (zéro tant qu'elle a des
	// membres). Une salle vide est conservée avec son état — figé en pause —
	// pendant la fenêtre de reprise du hub, puis détruite (§Modèle, VS-021).
	emptySince time.Time
}

func newRoom(name string, clock Clock, log *slog.Logger, info buildInfo) *Room {
	now := clock.Now()
	return &Room{
		name:  name,
		clock: clock,
		log:   log,
		info:  info,
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
//
// Reprise de session (§Comportements serveur, point 6) : si le pseudo est pris
// mais que le jeton `session` est le même que celui du détenteur, ce dernier est
// une connexion zombie (coupure silencieuse). Il est retiré sans toast de départ
// ni pause automatique, et son sink est renvoyé pour que l'appelant ferme la
// connexion **hors verrou**.
func (r *Room) join(name, session string, latencyMs int64, out sink) (*member, sink, error) {
	r.mu.Lock()
	defer r.mu.Unlock()

	var replaced sink
	for i, m := range r.members {
		if !strings.EqualFold(m.name, name) {
			continue
		}
		if session == "" || m.session != session {
			return nil, nil, errNameTaken
		}
		// Même client : on évince le zombie et on prend sa place.
		m.evicted = true
		replaced = m.out
		r.members = append(r.members[:i:i], r.members[i+1:]...)
		r.log.Info("reprise de session", "room", r.name, "remplace", m.id, "name", m.name)
		break
	}

	// Salle en attente de reprise (vide mais conservée) : cet arrivant reprend la
	// séance interrompue, son welcome porte l'état gelé (§Modèle, VS-021).
	resumed := len(r.members) == 0 && !r.emptySince.IsZero()
	r.emptySince = time.Time{}

	r.nextID++
	m := &member{
		id:        fmt.Sprintf("u%d", r.nextID),
		session:   session,
		name:      name,
		latencyMs: latencyMs,
		out:       out,
	}
	r.members = append(r.members, m)

	now := r.clock.Now()
	m.out.send(protocol.TypeWelcome, protocol.Welcome{
		SelfID:        m.id,
		Room:          r.name,
		State:         r.state,
		Users:         r.usersLocked(),
		ServerVersion: r.info.version,
		DownloadURL:   r.info.downloadURL,
	})
	if resumed {
		m.out.send(protocol.TypeToast, protocol.Toast{
			Level: protocol.LevelInfo,
			Text:  fmt.Sprintf("Séance reprise à %s", formatTimecode(r.state.PositionSec)),
		})
		r.log.Info("séance reprise", "room", r.name, "user", m.id,
			"positionSec", r.state.PositionSec)
	}
	text := fmt.Sprintf("%s a rejoint la salle", m.name)
	if replaced != nil {
		// Ni départ ni arrivée : le membre n'a jamais vraiment quitté la salle.
		text = fmt.Sprintf("%s a repris sa session", m.name)
	}
	for _, other := range r.members {
		if other != m {
			other.out.send(protocol.TypeToast, protocol.Toast{Level: protocol.LevelInfo, Text: text})
		}
	}
	r.broadcastUsersLocked(now)
	r.log.Info("membre rejoint", "room", r.name, "user", m.id, "name", m.name, "reprise", replaced != nil)
	return m, replaced, nil
}

// canResume dit si ce couple (pseudo, jeton) remplacerait un membre existant.
// Sert au hub à ne pas opposer le plafond de la salle à une simple reprise (la
// taille de la salle ne change pas). À appeler sous le verrou du hub, qui
// sérialise les arrivées.
func (r *Room) canResume(name, session string) bool {
	if session == "" {
		return false
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	for _, m := range r.members {
		if strings.EqualFold(m.name, name) && m.session == session {
			return true
		}
	}
	return false
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
	now := r.clock.Now()
	if idx < 0 {
		// Membre déjà retiré : reprise de session (le zombie ne provoque ni
		// toast de départ ni pause automatique) ou double leave.
		if len(r.members) == 0 {
			r.freezeLocked(now)
			return true
		}
		return false
	}
	r.members = append(r.members[:idx:idx], r.members[idx+1:]...)

	r.broadcastLocked(protocol.TypeToast, protocol.Toast{
		Level: protocol.LevelInfo,
		Text:  fmt.Sprintf("%s a quitté la salle", m.name),
	})
	if !r.state.Paused && len(r.members) > 0 {
		r.autoPauseLocked(now, fmt.Sprintf("Pause auto : %s s'est déconnecté", m.name))
	}
	r.broadcastUsersLocked(now)
	r.log.Info("membre parti", "room", r.name, "user", m.id, "restants", len(r.members))
	if len(r.members) > 0 {
		return false
	}
	// Dernier parti : la séance est gelée ici même, à l'instant du départ.
	r.freezeLocked(now)
	return true
}

// freezeLocked fait entrer en « linger » une salle qui vient de se vider : la
// séance est gelée à sa position courante, en pause, et l'instant du départ est
// noté pour le ramasse-miettes du hub (§Modèle, VS-021).
//
// Appelé depuis leave, sous le verrou de la salle et dans le même geste que le
// retrait du dernier membre : le gel doit dater de l'instant exact du départ,
// pas de celui où le hub s'en aperçoit.
func (r *Room) freezeLocked(now time.Time) {
	if !r.state.Paused {
		// Personne ne regarde plus : sans ce gel, la position de référence
		// continuerait de courir et la reprise retomberait n'importe où.
		r.state.PositionSec = r.positionAtLocked(msOf(now))
		r.state.Paused = true
		r.state.Rate = 1
		r.state.RefServerMs = msOf(now)
		r.state.SetBy = setByServer
	}
	if r.emptySince.IsZero() {
		// Idempotent : la fin d'agonie d'une connexion zombie ne doit pas
		// repousser la fin de la fenêtre de reprise.
		r.emptySince = now
	}
}

// markEmpty confirme, sous le verrou du hub, que la salle est toujours vide et
// bien en attente de reprise. Renvoie false si quelqu'un est arrivé entre-temps.
func (r *Room) markEmpty(now time.Time) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	if len(r.members) > 0 {
		return false
	}
	// Filet : une salle qui n'a jamais eu de membre n'est jamais passée par
	// leave et n'a donc pas d'instant de départ.
	if r.emptySince.IsZero() {
		r.emptySince = now
	}
	return true
}

// expired dit si la fenêtre de reprise de cette salle vide est écoulée.
func (r *Room) expired(now time.Time, linger time.Duration) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	return len(r.members) == 0 && !r.emptySince.IsZero() && now.Sub(r.emptySince) >= linger
}

// --- Handlers de messages ---

// handlePing répond immédiatement au ping applicatif (offset d'horloge client).
func (r *Room) handlePing(m *member, p protocol.Ping) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if m.evicted {
		return
	}
	m.out.send(protocol.TypePong, protocol.Pong{T: p.T, ServerMs: msOf(r.clock.Now())})
}

// observeRTT enregistre un RTT mesuré sur la connexion et met à jour la latence
// estimée du membre (moyenne glissante des rttWindow dernières mesures).
func (r *Room) observeRTT(m *member, rtt time.Duration) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if m.evicted {
		return
	}

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
	if m.evicted {
		return
	}
	if m.ready == msg.Ready {
		return
	}
	m.ready = msg.Ready
	r.broadcastUsersLocked(r.clock.Now())
}

func (r *Room) handleSetFile(m *member, msg protocol.SetFile) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if m.evicted {
		return
	}

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
	// Un membre remplacé par une reprise de session ne pilote plus la salle.
	if m.evicted {
		return
	}

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

	// L'auteur d'un control va mécaniquement voir sa position figée le temps que
	// VLC obéisse : ses reports ne déclenchent plus de pause auto pendant
	// controlGrace (§Comportements serveur, point 2).
	m.lastControlAt = now
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
	if m.evicted {
		return
	}

	now := r.clock.Now()
	m.positionSec = sanitizeFloat(msg.PositionSec)
	m.buffering = msg.Buffering

	if r.state.Paused || !r.autoPauseAllowedLocked(m, now) {
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
	if m.evicted {
		return
	}
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

// autoPauseAllowedLocked applique les garde-fous de la pause automatique
// déclenchée par un `report` (§Comportements serveur, point 2) :
//
//	a) jamais de pause auto dans une salle à un seul membre — personne à
//	   attendre, et c'est le cas qui écrasait les commandes de l'utilisateur
//	   testant seul (VS-017) ;
//	b) au plus une pause auto toutes les autoPauseCooldown par salle ;
//	c) les reports de l'auteur d'un `control` sont neutres pendant controlGrace
//	   après ce control (son propre seek le fait « bufferiser »).
//
// La pause automatique sur déconnexion (point 3) n'est pas concernée : elle
// répond à un départ, pas à un état rapporté.
func (r *Room) autoPauseAllowedLocked(m *member, now time.Time) bool {
	if len(r.members) < 2 {
		return false
	}
	if !r.lastAutoPause.IsZero() && now.Sub(r.lastAutoPause) < autoPauseCooldown {
		return false
	}
	return m.lastControlAt.IsZero() || now.Sub(m.lastControlAt) >= controlGrace
}

// autoPauseLocked fige la salle à sa position courante et prévient tout le monde.
func (r *Room) autoPauseLocked(now time.Time, reason string) {
	nowMs := msOf(now)
	r.lastAutoPause = now
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

// formatTimecode rend une position en HH:MM:SS (toast de reprise de séance).
func formatTimecode(sec float64) string {
	if math.IsNaN(sec) || sec < 0 {
		sec = 0
	}
	total := int64(math.Round(math.Min(sec, maxPositionSec)))
	return fmt.Sprintf("%02d:%02d:%02d", total/3600, total/60%60, total%60)
}

func truncate(s string, max int) string {
	runes := []rune(s)
	if len(runes) <= max {
		return s
	}
	return string(runes[:max])
}
