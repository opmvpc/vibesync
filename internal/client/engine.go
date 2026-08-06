// Package client est le moteur de synchronisation : il pilote VLC
// (internal/vlc), parle au serveur (internal/protocol) et expose un état
// complet à l'UI locale (internal/webui).
//
// Règles de sync : docs/protocol.md §Comportements client.
package client

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"log/slog"
	"math"
	"os"
	"path/filepath"
	"sort"
	"sync"
	"time"

	"github.com/thibsix/vibesync/internal/protocol"
	"github.com/thibsix/vibesync/internal/vlc"
)

// Clock est l'horloge du moteur (injectable pour les tests).
type Clock interface {
	Now() time.Time
}

type systemClock struct{}

func (systemClock) Now() time.Time { return time.Now() }

// Launcher ouvre un fichier dans un lecteur et renvoie son contrôleur.
type Launcher func(ctx context.Context, path string) (vlc.Controller, error)

// Config paramètre le moteur ; tous les champs ont un défaut raisonnable.
type Config struct {
	Clock          Clock
	Dialer         Dialer
	Launcher       Launcher
	Locator        func() (string, error)
	Logger         *slog.Logger
	PollInterval   time.Duration
	InitialBackoff time.Duration
	MaxBackoff     time.Duration
	// KeepVLCOpen : ne pas fermer VLC quand le client s'arrête.
	KeepVLCOpen bool
	// Version est la version applicative de ce client (fichier VERSION du repo,
	// injectée au build). Défaut "dev" : illisible en semver, donc jamais de
	// proposition de mise à jour — le client Go est un harnais, pas un livrable.
	Version string
}

func (c *Config) applyDefaults() {
	if c.Clock == nil {
		c.Clock = systemClock{}
	}
	if c.Dialer == nil {
		c.Dialer = WSDialer{}
	}
	if c.Locator == nil {
		c.Locator = vlc.Locate
	}
	if c.Launcher == nil {
		keep := c.KeepVLCOpen
		c.Launcher = func(ctx context.Context, path string) (vlc.Controller, error) {
			return vlc.Launch(ctx, vlc.LaunchOptions{FilePath: path, KeepAlive: keep})
		}
	}
	if c.Logger == nil {
		c.Logger = slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelWarn}))
	}
	if c.PollInterval <= 0 {
		c.PollInterval = PollInterval
	}
	if c.InitialBackoff <= 0 {
		c.InitialBackoff = time.Second
	}
	if c.MaxBackoff <= 0 {
		c.MaxBackoff = 10 * time.Second
	}
	if c.Version == "" {
		c.Version = DevVersion
	}
}

// expectation est ce que le moteur croit que VLC est en train de faire ;
// tout écart non expliqué = action de l'utilisateur.
type expectation struct {
	valid  bool
	paused bool
	pos    float64
	at     time.Time
	rate   float64
}

func (e expectation) predict(now time.Time) float64 {
	if e.paused {
		return e.pos
	}
	rate := e.rate
	if rate <= 0 {
		rate = 1
	}
	d := now.Sub(e.at).Seconds()
	if d < 0 {
		d = 0
	}
	return e.pos + d*rate
}

// action est une commande à envoyer à VLC, décidée sous verrou puis exécutée
// hors verrou.
type action struct {
	kind string // "pause" | "resume" | "seek" | "rate"
	val  float64
}

// Engine est le moteur de synchronisation.
type Engine struct {
	cfg Config
	log *slog.Logger
	// sessionToken est le jeton opaque de reprise de session (docs/protocol.md
	// §Messages client → serveur) : généré une fois, conservé pour toute la vie
	// du processus, envoyé dans chaque hello. Immuable après New.
	sessionToken string

	rootCtx  context.Context
	rootStop context.CancelFunc

	mu sync.Mutex
	// connexion
	phase      Phase
	retrying   bool
	req        ConnectRequest
	conn       Conn
	selfID     string
	users      []protocol.User
	lastError  string
	connCancel context.CancelFunc
	// connGen identifie la tentative de connexion courante : une boucle annulée
	// ne doit plus modifier l'état (phase, conn…) de la suivante.
	connGen uint64
	// horloge
	offsets []int64
	// haveOffset : aucune correction tant que le premier pong n'a pas fourni
	// de mesure d'offset (docs/protocol.md §Conditions de correction).
	haveOffset bool
	offsetMs   int64
	latency    int64
	// versions annoncées par le serveur dans le welcome (VS-023)
	serverVersion string
	downloadURL   string
	// état de salle
	roomState protocol.RoomState
	haveState bool
	// graceUntil suspend la détection d'action utilisateur (anti-boucle) ;
	// holdUntil suspend une nouvelle correction le temps que VLC applique la
	// précédente ; userHoldUntil est le hold de 2 s post-action utilisateur,
	// levé par l'écho du serveur (roomState setBy = soi) ou par expiration.
	graceUntil    time.Time
	holdUntil     time.Time
	userHoldUntil time.Time
	// pendingRS mémorise le dernier roomState d'autrui reçu pendant le hold ;
	// il ne s'applique qu'à l'expiration du hold, faute d'écho.
	pendingRS *protocol.RoomState
	// nudging porte l'hystérésis du rate-nudge (engage > 0,1 s, désengage < 0,03 s).
	nudging bool
	// lecteur
	playerGen  uint64
	player     vlc.Controller
	filePath   string
	fileSize   int64
	fileInfo   *protocol.FileInfo
	status     vlc.Status
	haveStatus bool
	vlcErr     string
	vlcBinary  string
	vlcOK      bool
	buffering  bool
	bufDetect  vlc.BufferingDetector
	expect     expectation
	appliedRat float64
	correcting string
	drift      float64
	ready      bool
	// tâches périodiques
	lastPing   time.Time
	lastReport time.Time
	// sorties
	outbox [][]byte

	subMu sync.Mutex
	subs  map[chan Event]struct{}

	writeMu sync.Mutex
}

// New construit un moteur prêt à l'emploi.
func New(cfg Config) *Engine {
	cfg.applyDefaults()
	ctx, stop := context.WithCancel(context.Background())
	e := &Engine{
		cfg:        cfg,
		log:        cfg.Logger,
		rootCtx:    ctx,
		rootStop:   stop,
		phase:      PhaseIdle,
		appliedRat: 1,
		subs:       map[chan Event]struct{}{},
	}
	e.roomState = protocol.RoomState{Paused: true, Rate: 1}
	if token, err := newSessionToken(); err == nil {
		e.sessionToken = token
	} else {
		// Sans jeton, on reste fonctionnel : la reprise de session après une
		// coupure silencieuse est simplement indisponible (name_taken possible).
		e.log.Error("jeton de session non généré, reprise de session indisponible", "err", err)
	}
	e.refreshVLCBinary()
	return e
}

// newSessionToken tire le jeton de reprise de session (16 octets aléatoires
// en hexadécimal, cf. docs/protocol.md).
func newSessionToken() (string, error) {
	buf := make([]byte, 16)
	if _, err := rand.Read(buf); err != nil {
		return "", fmt.Errorf("client: génération du jeton de session: %w", err)
	}
	return hex.EncodeToString(buf), nil
}

// Session est le jeton de reprise envoyé dans chaque hello (diagnostic, tests).
func (e *Engine) Session() string { return e.sessionToken }

// refreshVLCBinary met à jour la disponibilité de l'exécutable VLC.
func (e *Engine) refreshVLCBinary() {
	path, err := e.cfg.Locator()
	e.mu.Lock()
	defer e.mu.Unlock()
	if err != nil {
		e.vlcBinary, e.vlcOK = "", false
		e.vlcErr = err.Error()
		return
	}
	e.vlcBinary, e.vlcOK = path, true
	e.vlcErr = ""
}

// Run fait tourner la boucle de poll jusqu'à l'annulation du contexte.
func (e *Engine) Run(ctx context.Context) {
	ticker := time.NewTicker(e.cfg.PollInterval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-e.rootCtx.Done():
			return
		case <-ticker.C:
			e.tick(e.cfg.Clock.Now())
		}
	}
}

// Close arrête le moteur, la connexion et (sauf KeepVLCOpen) VLC.
func (e *Engine) Close() error {
	e.Disconnect()
	e.rootStop()
	e.mu.Lock()
	p := e.player
	e.player = nil
	e.mu.Unlock()
	var err error
	if p != nil {
		err = p.Close()
	}
	e.subMu.Lock()
	for ch := range e.subs {
		delete(e.subs, ch)
		close(ch)
	}
	e.subMu.Unlock()
	return err
}

// --- Abonnement de l'UI ---

// Subscribe renvoie un canal d'événements et sa fonction d'annulation.
// Le premier événement est l'état complet courant.
func (e *Engine) Subscribe() (<-chan Event, func()) {
	ch := make(chan Event, 64)
	e.subMu.Lock()
	e.subs[ch] = struct{}{}
	e.subMu.Unlock()
	snap := e.Snapshot()
	ch <- Event{Kind: EventState, State: &snap}
	var once sync.Once
	return ch, func() {
		once.Do(func() {
			e.subMu.Lock()
			if _, ok := e.subs[ch]; ok {
				delete(e.subs, ch)
				close(ch)
			}
			e.subMu.Unlock()
		})
	}
}

func (e *Engine) publish(ev Event) {
	e.subMu.Lock()
	defer e.subMu.Unlock()
	for ch := range e.subs {
		select {
		case ch <- ev:
		default: // abonné trop lent : on saute l'événement
		}
	}
}

func (e *Engine) publishState() {
	snap := e.Snapshot()
	e.publish(Event{Kind: EventState, State: &snap})
}

// Snapshot construit l'état complet exposé à l'UI.
func (e *Engine) Snapshot() Snapshot {
	e.mu.Lock()
	defer e.mu.Unlock()
	return e.snapshotLocked()
}

func (e *Engine) snapshotLocked() Snapshot {
	users := make([]protocol.User, len(e.users))
	copy(users, e.users)
	s := Snapshot{
		Phase:         e.phase,
		Version:       e.cfg.Version,
		ServerVersion: e.serverVersion,
		DownloadURL:   e.downloadURL,
		ServerURL:     e.req.URL,
		Room:          e.req.Room,
		Name:          e.req.Name,
		SelfID:        e.selfID,
		Users:         users,
		Ready:         e.ready,
		Paused:        e.roomState.Paused,
		RoomPosition:  e.expectedPositionLocked(e.cfg.Clock.Now()),
		RoomRate:      e.roomRateLocked(),
		DriftSec:      e.drift,
		Correcting:    e.correcting,
		LatencyMs:     e.latency,
		ClockOffsetMs: e.offsetMs,
		Retrying:      e.retrying,
		LastError:     e.lastError,
		VLC: VLCSnapshot{
			Running:     e.player != nil,
			Available:   e.vlcOK,
			BinaryPath:  e.vlcBinary,
			State:       string(e.status.State),
			PositionSec: e.status.PositionSec,
			DurationSec: e.status.LengthSec,
			Rate:        e.status.Rate,
			FilePath:    e.filePath,
			Buffering:   e.buffering,
			Error:       e.vlcErr,
		},
	}
	if s.VLC.State == "" {
		s.VLC.State = string(vlc.StateStopped)
	}
	if e.fileInfo != nil {
		s.VLC.FileName = e.fileInfo.Name
		if s.VLC.DurationSec <= 0 {
			s.VLC.DurationSec = e.fileInfo.DurationSec
		}
	}
	return s
}

// --- Commandes venues de l'UI ---

// Connect (re)démarre la boucle de connexion vers le serveur.
func (e *Engine) Connect(req ConnectRequest) {
	e.Disconnect()
	ctx, cancel := context.WithCancel(e.rootCtx)
	e.mu.Lock()
	e.connGen++
	gen := e.connGen
	e.req = req
	e.connCancel = cancel
	e.phase = PhaseConnecting
	e.lastError = ""
	e.invalidateReferenceLocked()
	e.mu.Unlock()
	e.publishState()
	go e.connLoop(ctx, req, gen)
}

// Disconnect ferme la connexion serveur (VLC reste ouvert).
func (e *Engine) Disconnect() {
	e.mu.Lock()
	e.connGen++ // toute boucle en cours devient obsolète
	cancel := e.connCancel
	e.connCancel = nil
	conn := e.conn
	e.conn = nil
	e.phase = PhaseIdle
	e.retrying = false
	e.invalidateReferenceLocked()
	e.mu.Unlock()
	if cancel != nil {
		cancel()
	}
	if conn != nil {
		_ = conn.Close()
	}
	e.publishState()
}

// invalidateReferenceLocked oublie tout ce qui sert à corriger : hors état
// connecté, l'état de référence est invalidé jusqu'au welcome suivant
// (docs/protocol.md §Conditions de correction).
func (e *Engine) invalidateReferenceLocked() {
	e.selfID = ""
	e.users = nil
	e.haveState = false
	e.haveOffset = false
	e.offsets = nil
	e.pendingRS = nil
	e.userHoldUntil = time.Time{}
	e.holdUntil = time.Time{}
	e.nudging = false
	e.correcting = ""
	e.drift = 0
	e.outbox = nil
}

// OpenFile lance VLC sur le fichier demandé (ferme le lecteur précédent).
func (e *Engine) OpenFile(ctx context.Context, path string) error {
	if path == "" {
		return errors.New("aucun fichier sélectionné")
	}
	info, err := os.Stat(path)
	if err != nil {
		return fmt.Errorf("fichier illisible: %w", err)
	}
	if info.IsDir() {
		return fmt.Errorf("%q est un dossier", path)
	}
	e.mu.Lock()
	old := e.player
	e.player = nil
	e.playerGen++
	e.haveStatus = false
	e.mu.Unlock()
	if old != nil {
		_ = old.Close()
	}
	player, err := e.cfg.Launcher(ctx, path)
	if err != nil {
		e.mu.Lock()
		e.vlcErr = err.Error()
		e.vlcOK = false
		e.mu.Unlock()
		e.publishState()
		return err
	}
	e.mu.Lock()
	e.player = player
	e.playerGen++
	e.filePath = path
	e.fileSize = info.Size()
	e.fileInfo = &protocol.FileInfo{Name: filepath.Base(path), SizeBytes: info.Size()}
	e.status = vlc.Status{}
	e.haveStatus = false
	e.expect = expectation{}
	// Média neuf : le diagnostic du précédent ne vaut plus rien. Ouvrir un
	// fichier enchaîne pause + seek 0 + démarrage, autant de raisons mécaniques
	// de voir la position figée : on suspend aussi la détection.
	e.bufDetect.Reset()
	e.buffering = false
	e.suspendBufferingLocked(e.cfg.Clock.Now())
	e.vlcErr = ""
	e.vlcOK = true
	e.appliedRat = 1
	e.queueLocked(protocol.TypeSetFile, protocol.SetFile{
		Name:      e.fileInfo.Name,
		SizeBytes: info.Size(),
	})
	e.mu.Unlock()
	e.flush()
	e.publishState()
	return nil
}

// SetReady annonce (ou retire) l'état « prêt ».
func (e *Engine) SetReady(ready bool) {
	e.mu.Lock()
	e.ready = ready
	e.queueLocked(protocol.TypeSetReady, protocol.SetReady{Ready: ready})
	e.mu.Unlock()
	e.flush()
	e.publishState()
}

// Chat envoie un message de salle.
func (e *Engine) Chat(text string) {
	if text == "" {
		return
	}
	e.mu.Lock()
	e.queueLocked(protocol.TypeChat, protocol.Chat{Text: text})
	e.mu.Unlock()
	e.flush()
}

// Play demande la lecture (action volontaire de l'utilisateur via l'UI).
func (e *Engine) Play() { e.userControl(protocol.ActionPlay, 0, false) }

// Pause demande la pause.
func (e *Engine) Pause() { e.userControl(protocol.ActionPause, 0, false) }

// Seek demande un saut de position (borné à [0, durée] ; non fini = ignoré).
func (e *Engine) Seek(positionSec float64) {
	e.userControl(protocol.ActionSeek, positionSec, true)
}

// userControl envoie un control. usePos à false = position courante observée.
func (e *Engine) userControl(act string, pos float64, usePos bool) {
	now := e.cfg.Clock.Now()
	e.mu.Lock()
	if !usePos {
		pos = e.currentPositionLocked(now)
	}
	if !isFinite(pos) {
		e.mu.Unlock()
		e.log.Warn("position de control non finie, ignorée", "action", act)
		return
	}
	pos = clampPosition(pos, e.durationLocked())
	e.queueLocked(protocol.TypeControl, protocol.Control{Action: act, PositionSec: pos})
	// Hold post-action : on suspend les corrections jusqu'à l'écho du serveur
	// (roomState setBy = soi) ou l'expiration.
	e.userHoldUntil = now.Add(UserHold)
	e.pendingRS = nil
	// Même raison que pour une action faite dans VLC : le seek (ou la
	// transition) qui va suivre fige la position, ce n'est pas un buffering.
	e.suspendBufferingLocked(now)
	e.mu.Unlock()
	e.flush()
}

// durationLocked est la durée du média courant, 0 si inconnue.
func (e *Engine) durationLocked() float64 {
	if e.haveStatus && e.status.LengthSec > 0 {
		return e.status.LengthSec
	}
	if e.fileInfo != nil && e.fileInfo.DurationSec > 0 {
		return e.fileInfo.DurationSec
	}
	return 0
}

func isFinite(v float64) bool { return !math.IsNaN(v) && !math.IsInf(v, 0) }

// clampPosition borne une position à [0, durée] (durée connue seulement).
func clampPosition(pos, duration float64) float64 {
	if pos < 0 {
		pos = 0
	}
	if duration > 0 && pos > duration {
		pos = duration
	}
	return pos
}

// currentPositionLocked est la meilleure estimation de « où on en est ».
func (e *Engine) currentPositionLocked(now time.Time) float64 {
	if e.haveStatus && e.status.Loaded() {
		return e.status.PositionSec
	}
	return e.expectedPositionLocked(now)
}

// --- Horloge serveur ---

func (e *Engine) nowServerMsLocked(now time.Time) int64 {
	return now.UnixMilli() + e.offsetMs
}

func (e *Engine) roomRateLocked() float64 {
	if e.roomState.Rate <= 0 {
		return 1
	}
	return e.roomState.Rate
}

// expectedPositionLocked applique la formule de docs/protocol.md :
// positionSec + (nowServer - refServerMs)/1000 × rate, sans le terme temporel
// si la salle est en pause.
func (e *Engine) expectedPositionLocked(now time.Time) float64 {
	if !e.haveState {
		return 0
	}
	if e.roomState.Paused {
		return math.Max(0, e.roomState.PositionSec)
	}
	elapsed := float64(e.nowServerMsLocked(now)-e.roomState.RefServerMs) / 1000
	pos := e.roomState.PositionSec + elapsed*e.roomRateLocked()
	if pos < 0 {
		pos = 0
	}
	return pos
}

// applyPong met à jour l'offset d'horloge (médiane des 5 dernières mesures).
func (e *Engine) applyPongLocked(p protocol.Pong, now time.Time) {
	nowMs := now.UnixMilli()
	rtt := nowMs - p.T
	if rtt < 0 {
		rtt = 0
	}
	offset := p.ServerMs + rtt/2 - nowMs
	e.offsets = append(e.offsets, offset)
	if len(e.offsets) > offsetSamples {
		e.offsets = e.offsets[len(e.offsets)-offsetSamples:]
	}
	e.offsetMs = median(e.offsets)
	e.latency = rtt / 2
	e.haveOffset = true
}

func median(v []int64) int64 {
	if len(v) == 0 {
		return 0
	}
	cp := make([]int64, len(v))
	copy(cp, v)
	sort.Slice(cp, func(i, j int) bool { return cp[i] < cp[j] })
	return cp[len(cp)/2]
}

// --- Envoi de messages ---

func (e *Engine) queueLocked(msgType string, data any) {
	raw, err := protocol.Encode(msgType, data)
	if err != nil {
		e.log.Error("encodage impossible", "type", msgType, "err", err)
		return
	}
	e.outbox = append(e.outbox, raw)
}

// flush envoie les messages en attente (hors verrou d'état). Une erreur
// d'écriture ferme la connexion : la boucle de reconnexion repart et le welcome
// re-déclare notre état — pas de perte silencieuse (docs/protocol.md
// §Assainissement).
func (e *Engine) flush() {
	e.mu.Lock()
	out := e.outbox
	e.outbox = nil
	conn := e.conn
	e.mu.Unlock()
	if conn == nil || len(out) == 0 {
		return
	}
	var failed error
	e.writeMu.Lock()
	for _, raw := range out {
		if err := conn.WriteMessage(raw); err != nil {
			failed = err
			break
		}
	}
	e.writeMu.Unlock()
	if failed != nil {
		e.log.Warn("écriture serveur impossible, fermeture et reconnexion", "err", failed)
		_ = conn.Close()
	}
}
