package client

import (
	"context"
	"encoding/json"
	"math"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"github.com/thibsix/vibesync/internal/protocol"
	"github.com/thibsix/vibesync/internal/vlc"
	"github.com/thibsix/vibesync/internal/vlc/vlctest"
)

// --- Conn factice en mémoire ---

type fakeConn struct {
	mu     sync.Mutex
	sent   [][]byte
	in     chan []byte
	closed chan struct{}
	once   sync.Once
}

func newFakeConn() *fakeConn {
	return &fakeConn{in: make(chan []byte, 32), closed: make(chan struct{})}
}

func (c *fakeConn) WriteMessage(data []byte) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	cp := make([]byte, len(data))
	copy(cp, data)
	c.sent = append(c.sent, cp)
	return nil
}

func (c *fakeConn) ReadMessage() ([]byte, error) {
	select {
	case m := <-c.in:
		return m, nil
	case <-c.closed:
		return nil, os.ErrClosed
	}
}

func (c *fakeConn) Close() error {
	c.once.Do(func() { close(c.closed) })
	return nil
}

// take vide et renvoie les enveloppes envoyées.
func (c *fakeConn) take() []protocol.Envelope {
	c.mu.Lock()
	sent := c.sent
	c.sent = nil
	c.mu.Unlock()
	out := make([]protocol.Envelope, 0, len(sent))
	for _, raw := range sent {
		env, err := protocol.Decode(raw)
		if err != nil {
			continue
		}
		out = append(out, env)
	}
	return out
}

// --- Harnais ---

type harness struct {
	t     *testing.T
	e     *Engine
	clock *vlctest.Clock
	fake  *vlctest.Fake
	conn  *fakeConn
}

func newHarness(t *testing.T) *harness {
	t.Helper()
	clock := vlctest.NewClock(time.Time{})
	fake := vlctest.New(clock.Now)
	t.Cleanup(fake.Close)
	conn := newFakeConn()
	e := New(Config{
		Clock:   clock,
		Dialer:  DialerFunc(func(context.Context, string) (Conn, error) { return conn, nil }),
		Locator: func() (string, error) { return "/faux/vlc", nil },
		Launcher: func(context.Context, string) (vlc.Controller, error) {
			return vlc.NewHTTPClient(fake.URL(), fake.Password()), nil
		},
		InitialBackoff: 5 * time.Millisecond,
		MaxBackoff:     20 * time.Millisecond,
	})
	t.Cleanup(func() { _ = e.Close() })
	return &harness{t: t, e: e, clock: clock, fake: fake, conn: conn}
}

// connect simule une session établie (hello déjà envoyé), applique un welcome
// puis un pong (sans mesure d'offset, aucune correction n'est autorisée).
func (h *harness) connect(state protocol.RoomState) {
	h.t.Helper()
	h.e.mu.Lock()
	h.e.conn = h.conn
	h.e.req = ConnectRequest{URL: "ws://test/ws", Name: "thib", Room: "soirée"}
	h.e.mu.Unlock()
	h.server(protocol.TypeWelcome, protocol.Welcome{
		SelfID: "u1",
		Room:   "soirée",
		State:  state,
		Users:  []protocol.User{{ID: "u1", Name: "thib"}},
	})
	h.pong(0)
	h.conn.take()
}

// pong injecte un pong d'offset donné (RTT nul, pour rester déterministe).
func (h *harness) pong(offsetMs int64) {
	h.t.Helper()
	now := h.clock.Now().UnixMilli()
	h.server(protocol.TypePong, protocol.Pong{T: now, ServerMs: now + offsetMs})
}

// server injecte un message serveur.
func (h *harness) server(msgType string, data any) {
	h.t.Helper()
	raw, err := protocol.Encode(msgType, data)
	if err != nil {
		h.t.Fatalf("encode %s: %v", msgType, err)
	}
	h.e.handleRaw(raw)
}

// openFile charge un média dans le faux VLC via le moteur.
func (h *harness) openFile(name string, lengthSec float64) {
	h.t.Helper()
	path := filepath.Join(h.t.TempDir(), name)
	if err := os.WriteFile(path, []byte("données vidéo"), 0o600); err != nil {
		h.t.Fatal(err)
	}
	h.fake.LoadFile(name, lengthSec)
	if err := h.e.OpenFile(context.Background(), path); err != nil {
		h.t.Fatalf("OpenFile: %v", err)
	}
}

// tick avance l'horloge de d puis exécute une itération du moteur.
func (h *harness) tick(d time.Duration) {
	h.clock.Advance(d)
	h.e.tick(h.clock.Now())
}

// ticks exécute n itérations de 200 ms.
func (h *harness) ticks(n int) {
	for range n {
		h.tick(PollInterval)
	}
}

// playing construit un roomState « en lecture » référencé à l'instant courant.
func (h *harness) playing(pos float64) protocol.RoomState {
	return protocol.RoomState{
		Paused:      false,
		PositionSec: pos,
		Rate:        1,
		RefServerMs: h.clock.Now().UnixMilli(),
		SetBy:       "u2",
	}
}

func (h *harness) paused(pos float64) protocol.RoomState {
	return protocol.RoomState{
		Paused:      true,
		PositionSec: pos,
		Rate:        1,
		RefServerMs: h.clock.Now().UnixMilli(),
		SetBy:       "u2",
	}
}

func controls(envs []protocol.Envelope) []protocol.Control {
	var out []protocol.Control
	for _, env := range envs {
		if env.Type != protocol.TypeControl {
			continue
		}
		if c, err := protocol.DecodeData[protocol.Control](env); err == nil {
			out = append(out, c)
		}
	}
	return out
}

func countType(envs []protocol.Envelope, typ string) int {
	n := 0
	for _, env := range envs {
		if env.Type == typ {
			n++
		}
	}
	return n
}

// --- Tests ---

func TestOffsetHorlogeMedianeGlissante(t *testing.T) {
	h := newHarness(t)
	h.connect(h.paused(0))

	// Cinq pongs dont un aberrant ; l'offset retenu doit être la médiane.
	deltas := []int64{1000, 1200, 800, 60000, 1100}
	for _, delta := range deltas {
		now := h.clock.Now().UnixMilli()
		const rtt = 100
		h.server(protocol.TypePong, protocol.Pong{T: now - rtt, ServerMs: now + delta - rtt/2})
		h.clock.Advance(2 * time.Second)
	}
	h.e.mu.Lock()
	got := h.e.offsetMs
	lat := h.e.latency
	h.e.mu.Unlock()
	if got != 1100 {
		t.Fatalf("offset = %d ms, attendu la médiane 1100", got)
	}
	if lat != 50 {
		t.Fatalf("latence = %d ms, attendu 50", lat)
	}
}

func TestOffsetGardeCinqDernieresMesures(t *testing.T) {
	h := newHarness(t)
	h.connect(h.paused(0))
	for _, delta := range []int64{100, 100, 100, 100, 100, 500, 500, 500, 500, 500} {
		now := h.clock.Now().UnixMilli()
		h.server(protocol.TypePong, protocol.Pong{T: now, ServerMs: now + delta})
		h.clock.Advance(time.Second)
	}
	h.e.mu.Lock()
	defer h.e.mu.Unlock()
	if len(h.e.offsets) != offsetSamples {
		t.Fatalf("%d mesures conservées, attendu %d", len(h.e.offsets), offsetSamples)
	}
	if h.e.offsetMs != 500 {
		t.Fatalf("offset = %d, attendu 500 (les vieilles mesures ont été oubliées)", h.e.offsetMs)
	}
}

func TestPositionAttendueSuitLHorlogeServeur(t *testing.T) {
	h := newHarness(t)
	h.connect(h.playing(10))
	h.clock.Advance(3 * time.Second)
	h.e.mu.Lock()
	got := h.e.expectedPositionLocked(h.clock.Now())
	h.e.mu.Unlock()
	if math.Abs(got-13) > 1e-6 {
		t.Fatalf("position attendue = %v, attendu 13", got)
	}
}

func TestDeadZoneAucuneCorrection(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 1200)
	h.connect(h.playing(100))
	h.fake.SeekTo(100)
	h.fake.Play()
	h.ticks(10)

	if got := h.fake.Rate(); math.Abs(got-1) > 1e-6 {
		t.Fatalf("rate = %v, attendu 1 (drift dans la zone morte)", got)
	}
	if h.fake.Seeks() != 0 {
		t.Fatalf("%d seek(s) alors que la position est bonne", h.fake.Seeks())
	}
}

func TestNudgePuisRetourARate1(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 1200)
	h.connect(h.playing(100))
	// VLC est 0,6 s en avance : nudge à 0,95×.
	h.fake.SeekTo(100.6)
	h.fake.Play()
	h.ticks(4)

	if got := h.fake.Rate(); math.Abs(got-NudgeSlow) > 1e-6 {
		t.Fatalf("rate = %v, attendu %v (en avance → ralentir)", got, NudgeSlow)
	}
	if h.fake.Seeks() != 0 {
		t.Fatalf("un seek dur a été émis pour un drift < 2 s")
	}

	// Convergence : à 0,95× on rattrape 0,05 s par seconde. L'hystérésis
	// maintient le nudge tant que |drift| ≥ 0,03 s.
	converged := false
	for range 400 {
		h.tick(PollInterval)
		d := math.Abs(h.e.Snapshot().DriftSec)
		if d <= DeadZoneSec && d > NudgeExitSec && math.Abs(h.fake.Rate()-NudgeSlow) > 1e-6 {
			t.Fatalf("nudge relâché trop tôt (drift %v, rate %v)", d, h.fake.Rate())
		}
		if d < NudgeExitSec {
			converged = true
			break
		}
	}
	if !converged {
		t.Fatalf("pas de convergence, drift = %v", h.e.Snapshot().DriftSec)
	}
	h.ticks(3)
	if got := h.fake.Rate(); math.Abs(got-1) > 1e-6 {
		t.Fatalf("rate = %v, attendu un retour à 1 après convergence", got)
	}
	if h.fake.Seeks() != 0 {
		t.Fatalf("aucun seek dur ne devait être nécessaire")
	}
}

func TestHysteresisDuNudge(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 1200)
	h.connect(h.playing(100))
	// Drift de 0,05 s : entre les deux seuils, le nudge ne doit PAS s'engager.
	h.fake.SeekTo(100.05)
	h.fake.Play()
	h.ticks(4)
	if got := h.fake.Rate(); math.Abs(got-1) > 1e-6 {
		t.Fatalf("rate = %v : le nudge ne s'engage qu'au-delà de %v s", got, DeadZoneSec)
	}
	h.e.mu.Lock()
	nudging := h.e.nudging
	h.e.mu.Unlock()
	if nudging {
		t.Fatal("nudge engagé sous le seuil d'engagement")
	}
}

func TestNudgeAccelereSiEnRetard(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 1200)
	h.connect(h.playing(100))
	h.fake.SeekTo(99.4) // 0,6 s de retard
	h.fake.Play()
	h.ticks(4)
	if got := h.fake.Rate(); math.Abs(got-NudgeFast) > 1e-6 {
		t.Fatalf("rate = %v, attendu %v (en retard → accélérer)", got, NudgeFast)
	}
}

func TestSeekDurSiGrosDriftPuisAffinage(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 1200)
	h.connect(h.playing(300))
	h.fake.SeekTo(120) // 180 s de retard
	h.fake.Play()
	h.ticks(2)

	if h.fake.Seeks() != 1 {
		t.Fatalf("%d seek(s), attendu exactement 1 seek dur", h.fake.Seeks())
	}
	if got := h.fake.Position(); math.Abs(got-300) > 1.5 {
		t.Fatalf("position après seek = %v, attendu ≈300", got)
	}
	// Puis affinage par nudge jusqu'à la zone morte, sans nouveau seek.
	converged := false
	for range 400 {
		h.tick(PollInterval)
		if math.Abs(h.e.Snapshot().DriftSec) <= DeadZoneSec {
			converged = true
			break
		}
	}
	if !converged {
		t.Fatalf("pas d'affinage après le seek, drift = %v", h.e.Snapshot().DriftSec)
	}
	if h.fake.Seeks() != 1 {
		t.Fatalf("%d seeks au total, l'affinage aurait dû se faire au rate", h.fake.Seeks())
	}
}

func TestConvergenceApresSeekDistant(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 3600)
	h.connect(h.playing(10))
	h.fake.SeekTo(10)
	h.fake.Play()
	h.ticks(5)

	// Un autre membre saute à 1800 s : le serveur diffuse un nouveau roomState.
	h.server(protocol.TypeRoomState, h.playing(1800))
	converged := false
	for range 400 {
		h.tick(PollInterval)
		if math.Abs(h.e.Snapshot().DriftSec) <= DeadZoneSec {
			converged = true
			break
		}
	}
	if !converged {
		t.Fatalf("pas de convergence après seek distant, drift = %v", h.e.Snapshot().DriftSec)
	}
	if h.fake.State() != "playing" {
		t.Fatalf("VLC devrait jouer, état = %q", h.fake.State())
	}
	if got := h.fake.Position(); got < 1795 {
		t.Fatalf("position = %v, attendu ≈1800+", got)
	}
	// Aucun control ne doit avoir été émis : c'est une correction, pas une action.
	if c := controls(h.conn.take()); len(c) != 0 {
		t.Fatalf("controls parasites émis pendant la correction: %+v", c)
	}
}

func TestJamaisDeNudgeEnPause(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 1200)
	h.connect(h.paused(500))
	h.fake.SeekTo(480)
	h.ticks(6)

	if got := h.fake.Rate(); math.Abs(got-1) > 1e-6 {
		t.Fatalf("rate = %v : on ne nudge jamais en pause", got)
	}
	if h.fake.Seeks() == 0 {
		t.Fatal("aucun seek en pause alors que la position est fausse")
	}
	if got := h.fake.Position(); math.Abs(got-500) > 1 {
		t.Fatalf("position = %v, attendu ≈500", got)
	}
	if h.fake.State() != "paused" {
		t.Fatalf("VLC devrait être en pause, état = %q", h.fake.State())
	}
}

func TestPauseDistanteArreteVLC(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 1200)
	h.connect(h.playing(100))
	h.fake.SeekTo(100)
	h.fake.Play()
	h.ticks(3)
	if h.fake.State() != "playing" {
		t.Fatalf("état = %q avant la pause distante", h.fake.State())
	}
	h.conn.take()

	h.server(protocol.TypeRoomState, h.paused(120))
	h.ticks(2)
	if h.fake.State() != "paused" {
		t.Fatalf("VLC n'a pas été mis en pause, état = %q", h.fake.State())
	}
	// Anti-boucle : la pause appliquée par le moteur ne doit pas remonter
	// comme une action utilisateur.
	h.ticks(10)
	if c := controls(h.conn.take()); len(c) != 0 {
		t.Fatalf("boucle détectée : controls émis %+v", c)
	}
}

func TestDetectionPauseManuelle(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 1200)
	h.connect(h.playing(100))
	h.fake.SeekTo(100)
	h.fake.Play()
	h.ticks(6)
	h.conn.take()

	// L'utilisateur met VLC en pause lui-même.
	h.fake.Pause()
	h.ticks(2)

	got := controls(h.conn.take())
	if len(got) != 1 {
		t.Fatalf("%d control(s) émis, attendu 1: %+v", len(got), got)
	}
	if got[0].Action != protocol.ActionPause {
		t.Fatalf("action = %q, attendu pause", got[0].Action)
	}
	if math.Abs(got[0].PositionSec-h.fake.Position()) > 0.5 {
		t.Fatalf("position du control = %v, VLC à %v", got[0].PositionSec, h.fake.Position())
	}
}

func TestDetectionPlayManuel(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 1200)
	h.connect(h.paused(100))
	h.fake.SeekTo(100)
	h.ticks(4)
	h.conn.take()

	h.fake.Play()
	h.ticks(2)
	got := controls(h.conn.take())
	if len(got) != 1 || got[0].Action != protocol.ActionPlay {
		t.Fatalf("attendu un control play, obtenu %+v", got)
	}
}

func TestDetectionSautManuel(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 1200)
	h.connect(h.playing(100))
	h.fake.SeekTo(100)
	h.fake.Play()
	h.ticks(6)
	h.conn.take()

	h.fake.SeekTo(600) // l'utilisateur saute dans la timeline
	h.ticks(2)
	got := controls(h.conn.take())
	if len(got) != 1 || got[0].Action != protocol.ActionSeek {
		t.Fatalf("attendu un control seek, obtenu %+v", got)
	}
	if math.Abs(got[0].PositionSec-600) > 1 {
		t.Fatalf("position du control = %v, attendu ≈600", got[0].PositionSec)
	}
}

func TestPetitSautNonRemonte(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 1200)
	h.connect(h.playing(100))
	h.fake.SeekTo(100)
	h.fake.Play()
	h.ticks(6)
	h.conn.take()

	h.fake.SeekTo(101.5) // 1,5 s : sous le seuil de 3 s
	h.ticks(2)
	if c := controls(h.conn.take()); len(c) != 0 {
		t.Fatalf("un saut de 1,5 s ne doit pas être vu comme une action: %+v", c)
	}
}

func TestFenetreDeGraceApresRoomState(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 1200)
	h.connect(h.playing(100))
	h.fake.SeekTo(100)
	h.fake.Play()
	h.ticks(6)
	h.conn.take()

	// roomState reçu puis changement observé dans les 500 ms : ignoré.
	h.server(protocol.TypeRoomState, h.playing(100))
	h.fake.Pause()
	h.tick(100 * time.Millisecond)
	h.tick(100 * time.Millisecond)
	if c := controls(h.conn.take()); len(c) != 0 {
		t.Fatalf("fenêtre de grâce non respectée: %+v", c)
	}
}

func TestResyncSurWelcomeRejoin(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 3600)
	h.fake.SeekTo(0)
	h.fake.Play()

	// On rejoint une salle déjà lancée à 1200 s.
	h.connect(h.playing(1200))
	h.ticks(3)

	if h.fake.Seeks() == 0 {
		t.Fatal("aucun seek de resynchronisation après le welcome")
	}
	if got := h.fake.Position(); math.Abs(got-1200) > 2 {
		t.Fatalf("position = %v, attendu ≈1200", got)
	}
}

func TestWelcomeRedeclareFichierEtReady(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 1200)
	h.connect(h.paused(0))
	h.ticks(2) // laisse le moteur découvrir la durée
	h.e.SetReady(true)
	h.conn.take()

	// Reconnexion : nouveau welcome.
	h.server(protocol.TypeWelcome, protocol.Welcome{
		SelfID: "u1", Room: "soirée",
		State: h.paused(0),
		Users: []protocol.User{{ID: "u1", Name: "thib", Ready: true}},
	})
	envs := h.conn.take()
	if countType(envs, protocol.TypeSetFile) == 0 {
		t.Fatalf("setFile non renvoyé après le welcome: %+v", envs)
	}
	if countType(envs, protocol.TypeSetReady) == 0 {
		t.Fatalf("setReady non renvoyé après le welcome")
	}
	if countType(envs, protocol.TypePing) == 0 {
		t.Fatalf("aucun ping immédiat après le welcome")
	}
}

func TestSetFileEnvoyeAvecDuree(t *testing.T) {
	h := newHarness(t)
	h.connect(h.paused(0))
	h.conn.take()
	h.openFile("film.mkv", 5400)
	h.ticks(2)

	var last *protocol.SetFile
	for _, env := range h.conn.take() {
		if env.Type != protocol.TypeSetFile {
			continue
		}
		sf, err := protocol.DecodeData[protocol.SetFile](env)
		if err != nil {
			t.Fatalf("setFile illisible: %v", err)
		}
		last = &sf
	}
	if last == nil {
		t.Fatal("aucun setFile envoyé")
	}
	if last.Name != "film.mkv" || math.Abs(last.DurationSec-5400) > 1 {
		t.Fatalf("setFile = %+v", last)
	}
	if last.SizeBytes <= 0 {
		t.Fatalf("taille de fichier non renseignée: %+v", last)
	}
}

func TestPingEtReportPeriodiques(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 1200)
	h.connect(h.playing(0))
	h.fake.Play()
	h.conn.take()

	h.ticks(50) // 10 s simulées
	envs := h.conn.take()
	pings := countType(envs, protocol.TypePing)
	reports := countType(envs, protocol.TypeReport)
	if pings < 4 || pings > 6 {
		t.Fatalf("%d pings sur 10 s, attendu ≈5", pings)
	}
	if reports < 9 || reports > 11 {
		t.Fatalf("%d reports sur 10 s, attendu ≈10", reports)
	}
}

func TestReportSignaleLeBuffering(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 1200)
	h.connect(h.playing(0))
	h.fake.Play()
	h.ticks(10)
	h.conn.take()

	h.fake.SetStalled(true) // VLC bufferise : la position se fige
	h.ticks(15)

	var sawBuffering bool
	for _, env := range h.conn.take() {
		if env.Type != protocol.TypeReport {
			continue
		}
		if r, err := protocol.DecodeData[protocol.Report](env); err == nil && r.Buffering {
			sawBuffering = true
		}
	}
	if !sawBuffering {
		t.Fatal("buffering jamais remonté dans les reports")
	}
	if !h.e.Snapshot().VLC.Buffering {
		t.Fatal("buffering absent de l'état exposé à l'UI")
	}
}

func TestControlsUI(t *testing.T) {
	h := newHarness(t)
	h.openFile("ep1.mkv", 1200)
	h.connect(h.playing(100))
	h.fake.SeekTo(140)
	h.fake.Play()
	h.ticks(1)
	h.conn.take()

	h.e.Pause()
	h.e.Seek(42)
	h.e.Play()
	h.e.Chat("salut")
	h.e.SetReady(true)

	envs := h.conn.take()
	got := controls(envs)
	if len(got) != 3 {
		t.Fatalf("%d controls, attendu 3: %+v", len(got), got)
	}
	if got[0].Action != protocol.ActionPause || got[1].Action != protocol.ActionSeek || got[2].Action != protocol.ActionPlay {
		t.Fatalf("actions inattendues: %+v", got)
	}
	if math.Abs(got[1].PositionSec-42) > 1e-9 {
		t.Fatalf("seek UI à %v, attendu 42", got[1].PositionSec)
	}
	if countType(envs, protocol.TypeChat) != 1 || countType(envs, protocol.TypeSetReady) != 1 {
		t.Fatalf("chat/setReady manquants: %+v", envs)
	}
	if !h.e.Snapshot().Ready {
		t.Fatal("état ready non reflété")
	}
}

func TestToastEtChatPousseALUI(t *testing.T) {
	h := newHarness(t)
	events, cancel := h.e.Subscribe()
	defer cancel()
	drain := func() []Event {
		var out []Event
		for {
			select {
			case ev := <-events:
				out = append(out, ev)
			default:
				return out
			}
		}
	}
	drain()
	h.connect(h.paused(0))
	h.server(protocol.TypeToast, protocol.Toast{Level: protocol.LevelWarn, Text: "fichiers différents"})
	h.server(protocol.TypeChatEvent, protocol.ChatEvent{From: "ami", Text: "coucou", ServerMs: 42})

	var toast, chat bool
	for _, ev := range drain() {
		switch ev.Kind {
		case EventToast:
			if ev.Toast.Text == "fichiers différents" {
				toast = true
			}
		case EventChat:
			if ev.Chat.From == "ami" && ev.Chat.Text == "coucou" {
				chat = true
			}
		}
	}
	if !toast || !chat {
		t.Fatalf("toast=%v chat=%v", toast, chat)
	}
}

func TestUsersMetAJourLEtat(t *testing.T) {
	h := newHarness(t)
	h.connect(h.paused(0))
	h.server(protocol.TypeUsers, protocol.UsersMsg{Users: []protocol.User{
		{ID: "u1", Name: "thib", Ready: true, LatencyMs: 12},
		{ID: "u2", Name: "ami", LatencyMs: 80},
	}})
	snap := h.e.Snapshot()
	if len(snap.Users) != 2 {
		t.Fatalf("%d participants", len(snap.Users))
	}
	if !snap.Ready {
		t.Fatal("le ready du serveur n'a pas été adopté")
	}
}

func TestErreurFataleRemonte(t *testing.T) {
	h := newHarness(t)
	h.e.mu.Lock()
	h.e.conn = h.conn
	h.e.mu.Unlock()
	raw, _ := protocol.Encode(protocol.TypeError, protocol.ErrorMsg{Code: protocol.ErrBadPassword})
	_, fatal := h.e.handleRaw(raw)
	if !fatal {
		t.Fatal("bad_password devrait être fatal")
	}
	if snap := h.e.Snapshot(); snap.LastError == "" {
		t.Fatal("erreur non exposée à l'UI")
	}
	raw, _ = protocol.Encode(protocol.TypeError, protocol.ErrorMsg{Code: protocol.ErrProtocol, Text: "oups"})
	if _, fatal := h.e.handleRaw(raw); fatal {
		t.Fatal("une erreur `protocol` ne doit pas être fatale")
	}
}

func TestMessageInconnuIgnore(t *testing.T) {
	h := newHarness(t)
	h.connect(h.paused(0))
	if _, fatal := h.e.handleRaw([]byte(`{"type":"futur","data":{"x":1}}`)); fatal {
		t.Fatal("un message inconnu ne doit pas être fatal")
	}
	if _, fatal := h.e.handleRaw([]byte(`pas du json`)); fatal {
		t.Fatal("un message illisible ne doit pas être fatal")
	}
}

func TestSnapshotJSONStable(t *testing.T) {
	h := newHarness(t)
	h.connect(h.playing(12.5))
	raw, err := json.Marshal(h.e.Snapshot())
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	var m map[string]any
	if err := json.Unmarshal(raw, &m); err != nil {
		t.Fatal(err)
	}
	for _, key := range []string{"phase", "room", "users", "paused", "roomPositionSec", "driftSec", "vlc", "latencyMs"} {
		if _, ok := m[key]; !ok {
			t.Fatalf("clé %q absente de l'état poussé à l'UI: %s", key, raw)
		}
	}
}

func TestVLCIntrouvable(t *testing.T) {
	e := New(Config{
		Clock:   vlctest.NewClock(time.Time{}),
		Locator: func() (string, error) { return "", vlc.ErrNotFound },
	})
	defer func() { _ = e.Close() }()
	snap := e.Snapshot()
	if snap.VLC.Available {
		t.Fatal("VLC ne devrait pas être marqué disponible")
	}
	if snap.VLC.Error == "" {
		t.Fatal("aucune explication pour l'UI")
	}
}
