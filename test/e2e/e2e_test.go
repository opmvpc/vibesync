// Package e2e_test assemble tout vibesync dans un seul process : un vrai
// serveur internal/server derrière httptest, deux moteurs internal/client
// complets (transport internal/ws réel), chacun pilotant son propre faux VLC
// (internal/vlc/vlctest).
//
// Horloge réelle assumée (c'est de l'intégration) : la synchronisation des
// tests se fait exclusivement par polling à échéance sur des états observables
// (position/état du faux VLC, messages reçus), jamais par une attente aveugle.
package e2e_test

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"math"
	"net"
	"net/http/httptest"
	"os"
	"path/filepath"
	"slices"
	"sort"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/thibsix/vibesync/internal/client"
	"github.com/thibsix/vibesync/internal/protocol"
	"github.com/thibsix/vibesync/internal/server"
	"github.com/thibsix/vibesync/internal/vlc"
	"github.com/thibsix/vibesync/internal/vlc/vlctest"
)

const (
	// waitTimeout borne chaque attente conditionnelle.
	waitTimeout = 12 * time.Second
	// convergeTimeout est plus large : après un seek, deux clients peuvent
	// arrondir vers deux secondes entières différentes et ne se rejoindre
	// qu'au rythme du nudge (5 %/s).
	convergeTimeout = 25 * time.Second
	// pollEvery est le pas de scrutation des conditions.
	pollEvery = 10 * time.Millisecond
	// driftMax est l'écart maximal toléré entre deux lecteurs synchronisés.
	driftMax = 0.5
	// filmDuration est la durée du média de référence.
	filmDuration = 3600.0
)

func quietLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}

// --- Horloge serveur : temps réel, décalable d'un saut ---

// skewClock donne l'heure réelle augmentée d'un décalage que le test peut
// avancer d'un coup. Le reste du système garde donc son comportement temporel
// normal ; seul « et maintenant, deux minutes plus tard » est simulé.
type skewClock struct {
	mu   sync.Mutex
	skew time.Duration
}

func (c *skewClock) Now() time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	return time.Now().Add(c.skew)
}

func (c *skewClock) Advance(d time.Duration) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.skew += d
}

// --- Dialer instrumenté : permet de couper brutalement une session ---

type recordingDialer struct {
	inner client.Dialer
	// resolve donne l'adresse du serveur au moment de composer. Le moteur, lui,
	// garde l'URL reçue au Connect : cette indirection permet de redémarrer le
	// httptest (forcément sur un autre port) sans que le client « sache » que
	// l'adresse a changé — l'équivalent d'un serveur qui revient chez lui.
	resolve func() string

	mu    sync.Mutex
	last  client.Conn
	dials int
}

func (d *recordingDialer) Dial(ctx context.Context, url string) (client.Conn, error) {
	d.mu.Lock()
	resolve := d.resolve
	d.mu.Unlock()
	if resolve != nil {
		url = resolve()
	}
	c, err := d.inner.Dial(ctx, url)
	if err != nil {
		return nil, err
	}
	d.mu.Lock()
	d.last = c
	d.dials++
	d.mu.Unlock()
	return c, nil
}

// setResolve change l'adresse que ce dialer composera désormais.
func (d *recordingDialer) setResolve(fn func() string) {
	d.mu.Lock()
	d.resolve = fn
	d.mu.Unlock()
}

// cutLast coupe la connexion courante sous les pieds du moteur.
func (d *recordingDialer) cutLast() error {
	d.mu.Lock()
	c := d.last
	d.mu.Unlock()
	if c == nil {
		return errors.New("aucune connexion à couper")
	}
	return c.Close()
}

func (d *recordingDialer) count() int {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.dials
}

// --- Pair : moteur + faux VLC + journal des messages reçus ---

type peer struct {
	name   string
	eng    *client.Engine
	fake   *vlctest.Fake
	dialer *recordingDialer
	fx     *fixture
	// dialCount expose le nombre de connexions ouvertes (diagnostic).
	dialCount func() int

	mu     sync.Mutex
	toasts []stampedToast
	chats  []protocol.ChatEvent
}

type stampedToast struct {
	at    time.Duration
	level string
	text  string
}

func (p *peer) snap() client.Snapshot { return p.eng.Snapshot() }

// pos est la position réellement affichée par le faux VLC.
func (p *peer) pos() float64 { return p.fake.Position() }

// vlcState est l'état réel du faux VLC ("playing", "paused", "stopped").
func (p *peer) vlcState() string { return p.fake.State() }

func (p *peer) collect(events <-chan client.Event) {
	for ev := range events {
		switch ev.Kind {
		case client.EventToast:
			if ev.Toast == nil {
				continue
			}
			p.mu.Lock()
			p.toasts = append(p.toasts, stampedToast{
				at: time.Since(p.fx.start), level: ev.Toast.Level, text: ev.Toast.Text,
			})
			p.mu.Unlock()
		case client.EventChat:
			if ev.Chat == nil {
				continue
			}
			p.mu.Lock()
			p.chats = append(p.chats, *ev.Chat)
			p.mu.Unlock()
		}
	}
}

// hasToastPrefix dit si un toast commence exactement par ce préfixe : sert à
// distinguer la reprise décidée par le client (« Reprise à … ») de celle
// annoncée par le serveur (« Séance reprise à … »).
func (p *peer) hasToastPrefix(prefix string) bool {
	p.mu.Lock()
	defer p.mu.Unlock()
	for _, t := range p.toasts {
		if strings.HasPrefix(t.text, prefix) {
			return true
		}
	}
	return false
}

// chatTexts rend, dans l'ordre de réception, les messages de chat vus par ce pair.
func (p *peer) chatTexts() []string {
	p.mu.Lock()
	defer p.mu.Unlock()
	out := make([]string, 0, len(p.chats))
	for _, c := range p.chats {
		out = append(out, c.Text)
	}
	return out
}

// hasToast dit si un toast du niveau donné contenant substr a été reçu.
func (p *peer) hasToast(level, substr string) bool {
	p.mu.Lock()
	defer p.mu.Unlock()
	for _, t := range p.toasts {
		if t.level == level && strings.Contains(strings.ToLower(t.text), strings.ToLower(substr)) {
			return true
		}
	}
	return false
}

func (p *peer) allToasts() []stampedToast {
	p.mu.Lock()
	defer p.mu.Unlock()
	out := make([]stampedToast, len(p.toasts))
	copy(out, p.toasts)
	return out
}

// dials est le nombre de connexions ouvertes par ce pair (0 si non instrumenté).
func (p *peer) dials() int {
	if p.dialCount == nil {
		return 0
	}
	return p.dialCount()
}

// reconnect rouvre une session pour ce pair (même pseudo, même salle, même
// jeton : le moteur le conserve pour toute la vie du processus).
func (p *peer) reconnect() {
	p.eng.Connect(client.ConnectRequest{URL: p.fx.currentURL(), Name: p.name, Room: p.fx.room})
}

// couperReseau simule la perte de connectivité de ce pair seul : sa connexion
// est coupée et ses tentatives de reconnexion tombent dans le vide jusqu'à
// retablirReseau. Le serveur, lui, reste debout pour les autres — c'est ce qui
// distingue une coupure réseau d'un départ volontaire (que le moteur traite
// tout autrement : la file de chat y est jetée).
func (p *peer) couperReseau() {
	p.dialer.setResolve(func() string { return "ws://127.0.0.1:1/ws" })
	_ = p.dialer.cutLast()
}

func (p *peer) retablirReseau() {
	p.dialer.setResolve(p.fx.currentURL)
}

func (p *peer) ready() bool {
	for _, u := range p.snap().Users {
		if u.Name == p.name {
			return u.Ready
		}
	}
	return false
}

// --- Fixture : serveur + pairs ---

// trackingListener retient les connexions acceptées pour pouvoir les couper.
// httptest.Server.Close() « oublie » les connexions hijackées — c'est le cas de
// toutes les WebSocket — qui survivraient donc à l'arrêt du serveur : sans ça,
// un redémarrage simulé passerait totalement inaperçu des clients.
type trackingListener struct {
	net.Listener
	mu    sync.Mutex
	conns []net.Conn
}

func (l *trackingListener) Accept() (net.Conn, error) {
	c, err := l.Listener.Accept()
	if err != nil {
		return nil, err
	}
	l.mu.Lock()
	l.conns = append(l.conns, c)
	l.mu.Unlock()
	return c, nil
}

func (l *trackingListener) closeAll() {
	l.mu.Lock()
	conns := l.conns
	l.conns = nil
	l.mu.Unlock()
	for _, c := range conns {
		_ = c.Close()
	}
}

type fixture struct {
	t     *testing.T
	room  string
	start time.Time
	// clock est l'horloge du serveur : sert à simuler une longue absence.
	clock *skewClock
	cfg   server.Config

	srvMu sync.Mutex
	ts    *httptest.Server
	lis   *trackingListener
	url   string

	mu    sync.Mutex
	peers []*peer
}

func newFixture(t *testing.T) *fixture { return newFixtureWith(t, server.Config{}) }

func newFixtureWith(t *testing.T, cfg server.Config) *fixture {
	t.Helper()
	f := &fixture{
		t:     t,
		room:  strings.NewReplacer("/", "-", " ", "-").Replace(t.Name()),
		start: time.Now(),
		clock: &skewClock{},
		cfg:   cfg,
	}
	f.startServer()
	t.Cleanup(f.stopServer)
	return f
}

// startServer démarre une instance neuve et publie son adresse.
func (f *fixture) startServer() {
	f.t.Helper()
	srv := server.New(f.cfg, server.WithLogger(quietLogger()), server.WithClock(f.clock))
	ts := httptest.NewUnstartedServer(srv.Handler())
	lis := &trackingListener{Listener: ts.Listener}
	ts.Listener = lis
	ts.Start()

	f.srvMu.Lock()
	f.ts, f.lis = ts, lis
	f.url = "ws://" + strings.TrimPrefix(ts.URL, "http://") + "/ws"
	f.srvMu.Unlock()
}

func (f *fixture) stopServer() {
	f.srvMu.Lock()
	ts, lis := f.ts, f.lis
	f.ts, f.lis = nil, nil
	f.srvMu.Unlock()
	if ts == nil {
		return
	}
	ts.Close()
	lis.closeAll()
}

// restartServer coupe le serveur et en démarre un neuf : salles perdues, état
// oublié — exactement ce que fait un redéploiement Coolify.
func (f *fixture) restartServer() {
	f.t.Helper()
	f.stopServer()
	f.startServer()
}

// currentURL est l'adresse du serveur en cours (résolue à chaque dial).
func (f *fixture) currentURL() string {
	f.srvMu.Lock()
	defer f.srvMu.Unlock()
	return f.url
}

// newPeer branche un moteur complet sur son faux VLC et le connecte à la salle.
func (f *fixture) newPeer(name string, durationSec float64) *peer {
	t := f.t
	t.Helper()

	fake := vlctest.New(time.Now)
	t.Cleanup(fake.Close)
	fake.LoadFile("film.mkv", durationSec)

	dialer := &recordingDialer{inner: client.WSDialer{}, resolve: f.currentURL}
	eng := client.New(client.Config{
		Dialer:  dialer,
		Logger:  quietLogger(),
		Locator: func() (string, error) { return "/faux/vlc", nil },
		Launcher: func(ctx context.Context, _ string) (vlc.Controller, error) {
			// Séquence de production : le driver arrête le média au début avant
			// de le déclarer chargé (le faux VLC autoplay comme le vrai).
			c := vlc.NewHTTPClient(fake.URL(), fake.Password())
			if err := vlc.Prepare(ctx, c, 10*time.Second); err != nil {
				return nil, err
			}
			return c, nil
		},
		// Reconnexion rapide : on veut voir le rejoin dans le budget du test.
		InitialBackoff: 300 * time.Millisecond,
		MaxBackoff:     time.Second,
	})
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(func() {
		cancel()
		_ = eng.Close()
	})
	go eng.Run(ctx)

	p := &peer{name: name, eng: eng, fake: fake, dialer: dialer, fx: f, dialCount: dialer.count}
	events, unsub := eng.Subscribe()
	t.Cleanup(unsub)
	go p.collect(events)

	f.mu.Lock()
	f.peers = append(f.peers, p)
	f.mu.Unlock()

	eng.Connect(client.ConnectRequest{URL: f.currentURL(), Name: name, Room: f.room})
	f.waitFor(name+" connecté", func() bool { return p.snap().Phase == client.PhaseConnected })

	path := filepath.Join(t.TempDir(), "film.mkv")
	if err := os.WriteFile(path, []byte("données vidéo"), 0o600); err != nil {
		t.Fatalf("écriture du fichier factice: %v", err)
	}
	if err := eng.OpenFile(ctx, path); err != nil {
		t.Fatalf("%s: OpenFile: %v", name, err)
	}
	f.waitFor(name+" a déclaré son fichier", func() bool { return p.snap().VLC.DurationSec > 0 })
	return p
}

func (f *fixture) list() []*peer {
	f.mu.Lock()
	defer f.mu.Unlock()
	out := make([]*peer, len(f.peers))
	copy(out, f.peers)
	return out
}

// waitFor scrute une condition jusqu'à échéance, puis échoue avec un
// diagnostic complet (états des pairs + timeline des toasts).
func (f *fixture) waitFor(desc string, cond func() bool) {
	f.t.Helper()
	f.waitUpTo(waitTimeout, desc, cond)
}

func (f *fixture) waitUpTo(timeout time.Duration, desc string, cond func() bool) {
	f.t.Helper()
	deadline := time.Now().Add(timeout)
	for {
		if cond() {
			return
		}
		if time.Now().After(deadline) {
			f.t.Fatalf("délai dépassé (%s) en attendant : %s\n%s", timeout, desc, f.dump())
		}
		time.Sleep(pollEvery)
	}
}

// holds vérifie qu'une propriété reste vraie pendant toute une durée (au lieu
// d'un sleep suivi d'une assertion ponctuelle).
func (f *fixture) holds(d time.Duration, desc string, cond func() bool) {
	f.t.Helper()
	deadline := time.Now().Add(d)
	for time.Now().Before(deadline) {
		if !cond() {
			f.t.Fatalf("propriété non tenue : %s\n%s", desc, f.dump())
		}
		time.Sleep(pollEvery)
	}
}

// dump décrit l'état courant de tous les pairs et la timeline des toasts.
func (f *fixture) dump() string {
	var b strings.Builder
	b.WriteString("--- état des pairs ---\n")
	for _, p := range f.list() {
		s := p.snap()
		fmt.Fprintf(&b, "  %-6s phase=%-10s vlc=%-7s pos=%8.2f durée=%6.0f drift=%+6.2f corr=%-5q "+
			"buffering=%v ready=%v | salle: pos=%8.2f paused=%v rate=%.2f | latence=%d ms conn=%d err=%q\n",
			p.name, s.Phase, p.vlcState(), p.pos(), s.VLC.DurationSec, s.DriftSec, s.Correcting,
			s.VLC.Buffering, s.Ready, s.RoomPosition, s.Paused, s.RoomRate,
			s.LatencyMs, p.dials(), s.LastError)
		for _, u := range s.Users {
			file := "—"
			if u.File != nil {
				file = fmt.Sprintf("%s (%.0fs)", u.File.Name, u.File.DurationSec)
			}
			fmt.Fprintf(&b, "        vu par %s: %s ready=%v pos=%.2f fichier=%s\n",
				p.name, u.Name, u.Ready, u.PositionSec, file)
		}
	}
	b.WriteString("--- timeline des toasts ---\n")
	type line struct {
		at   time.Duration
		text string
	}
	var lines []line
	for _, p := range f.list() {
		for _, t := range p.allToasts() {
			lines = append(lines, line{t.at, fmt.Sprintf("  %7.2fs [%s] %s: %s",
				t.at.Seconds(), t.level, p.name, t.text)})
		}
	}
	sort.Slice(lines, func(i, j int) bool { return lines[i].at < lines[j].at })
	for _, l := range lines {
		b.WriteString(l.text + "\n")
	}
	if len(lines) == 0 {
		b.WriteString("  (aucun)\n")
	}
	return b.String()
}

// --- Étapes réutilisables ---

// joinBoth crée deux pairs et attend qu'ils se voient mutuellement.
func joinBoth(f *fixture, durA, durB float64) (*peer, *peer) {
	f.t.Helper()
	a := f.newPeer("alice", durA)
	b := f.newPeer("bob", durB)
	f.waitFor("les deux membres sont visibles de tous", func() bool {
		return len(a.snap().Users) == 2 && len(b.snap().Users) == 2
	})
	return a, b
}

// markReady met les pairs prêts et attend la confirmation du serveur.
func markReady(f *fixture, peers ...*peer) {
	f.t.Helper()
	for _, p := range peers {
		p.eng.SetReady(true)
	}
	for _, p := range peers {
		f.waitFor(p.name+" est prêt côté serveur", p.ready)
	}
}

// startPlayback fait démarrer la lecture par p et attend que tout le monde joue.
func startPlayback(f *fixture, initiator *peer, all ...*peer) {
	f.t.Helper()
	initiator.eng.Play()
	f.waitFor("tous les VLC jouent", func() bool {
		for _, p := range all {
			if p.vlcState() != "playing" {
				return false
			}
		}
		return true
	})
}

// pairDrift est l'écart entre les positions réellement affichées.
func pairDrift(a, b *peer) float64 { return math.Abs(a.pos() - b.pos()) }

func TestE2E(t *testing.T) {
	t.Run("01-nominal-lecture-synchrone", func(t *testing.T) {
		f := newFixture(t)
		a, b := joinBoth(f, filmDuration, filmDuration)
		markReady(f, a, b)
		startPlayback(f, a, a, b)

		f.waitFor("la lecture progresse réellement des deux côtés", func() bool {
			return a.pos() > 1 && b.pos() > 1
		})
		f.waitFor("drift < 0,5 s", func() bool { return pairDrift(a, b) < driftMax })
		// Chaque moteur doit se savoir dans la zone morte par rapport à la
		// position de référence de la salle (et pas simplement « les deux
		// lecteurs avancent en parallèle »).
		f.waitFor("chaque moteur est aligné sur la salle", func() bool {
			return math.Abs(a.snap().DriftSec) < 0.2 && math.Abs(b.snap().DriftSec) < 0.2
		})
		f.holds(2*time.Second, "la synchronisation tient dans la durée", func() bool {
			return pairDrift(a, b) < driftMax &&
				a.vlcState() == "playing" && b.vlcState() == "playing"
		})
	})

	t.Run("02-pause-reprise-croisees", func(t *testing.T) {
		f := newFixture(t)
		a, b := joinBoth(f, filmDuration, filmDuration)
		markReady(f, a, b)
		startPlayback(f, a, a, b)
		f.waitFor("lecture engagée", func() bool { return a.pos() > 0.5 && b.pos() > 0.5 })

		// A met en pause : le VLC de B doit suivre.
		a.eng.Pause()
		f.waitFor("le VLC de B se met en pause", func() bool { return b.vlcState() == "paused" })
		f.waitFor("le VLC de A est aussi en pause", func() bool { return a.vlcState() == "paused" })
		f.holds(700*time.Millisecond, "la pause tient des deux côtés", func() bool {
			return a.vlcState() == "paused" && b.vlcState() == "paused"
		})
		f.waitFor("les positions restent alignées en pause", func() bool {
			return pairDrift(a, b) < driftMax
		})

		// B relance : le VLC de A doit suivre.
		b.eng.Play()
		f.waitFor("le VLC de A repart", func() bool { return a.vlcState() == "playing" })
		f.waitFor("le VLC de B joue aussi", func() bool { return b.vlcState() == "playing" })
		f.waitUpTo(convergeTimeout, "drift < 0,5 s après la reprise", func() bool {
			return pairDrift(a, b) < driftMax
		})
	})

	t.Run("03-seek-converge", func(t *testing.T) {
		f := newFixture(t)
		a, b := joinBoth(f, filmDuration, filmDuration)
		markReady(f, a, b)
		startPlayback(f, a, a, b)
		f.waitFor("lecture engagée", func() bool { return a.pos() > 1 && b.pos() > 1 })

		beforeA, beforeB := a.pos(), b.pos()
		a.eng.Seek(a.pos() + 300)

		// Seek dur : les deux lecteurs doivent réellement bondir (~+300 s), pas
		// simplement dériver.
		f.waitFor("B a sauté d'environ +300 s (seek dur)", func() bool {
			return b.pos() > beforeB+250
		})
		f.waitFor("A a sauté d'environ +300 s", func() bool {
			return a.pos() > beforeA+250
		})
		f.waitUpTo(convergeTimeout, "affinage : drift A/B < 0,5 s", func() bool {
			return pairDrift(a, b) < driftMax
		})
		f.waitUpTo(convergeTimeout, "chacun est aligné sur la salle", func() bool {
			return math.Abs(a.snap().DriftSec) < driftMax && math.Abs(b.snap().DriftSec) < driftMax
		})
		if a.vlcState() != "playing" || b.vlcState() != "playing" {
			t.Fatalf("la lecture ne s'est pas poursuivie après le seek\n%s", f.dump())
		}
	})

	t.Run("04-ready-gate", func(t *testing.T) {
		f := newFixture(t)
		a, b := joinBoth(f, filmDuration, filmDuration)
		markReady(f, a) // B reste « pas prêt »

		a.eng.Play()
		f.waitFor("A reçoit le toast de blocage", func() bool {
			return a.hasToast(protocol.LevelWarn, "lecture bloquée")
		})
		f.holds(time.Second, "aucune lecture ne démarre tant que B n'est pas prêt", func() bool {
			return a.vlcState() != "playing" && b.vlcState() != "playing" && a.snap().Paused
		})

		// B se déclare prêt : la lecture est acceptée.
		markReady(f, b)
		a.eng.Play()
		f.waitFor("la lecture démarre une fois tout le monde prêt", func() bool {
			return a.vlcState() == "playing" && b.vlcState() == "playing"
		})
		f.waitFor("drift < 0,5 s", func() bool { return pairDrift(a, b) < driftMax })
	})

	t.Run("05-buffering-pause-auto", func(t *testing.T) {
		f := newFixture(t)
		a, b := joinBoth(f, filmDuration, filmDuration)
		markReady(f, a, b)
		startPlayback(f, a, a, b)
		f.waitFor("lecture engagée", func() bool { return a.pos() > 1 && b.pos() > 1 })

		// Le VLC de B se fige alors qu'il se déclare toujours en lecture.
		b.fake.SetStalled(true)

		f.waitFor("B détecte son buffering", func() bool { return b.snap().VLC.Buffering })
		f.waitFor("A reçoit le toast de pause auto", func() bool {
			return a.hasToast(protocol.LevelWarn, "bufferise")
		})
		f.waitFor("la salle est en pause pour tout le monde", func() bool {
			return a.snap().Paused && b.snap().Paused
		})
		f.waitFor("le VLC de A est effectivement en pause", func() bool {
			return a.vlcState() == "paused"
		})
	})

	t.Run("06-deconnexion-et-rejoin", func(t *testing.T) {
		f := newFixture(t)
		a, b := joinBoth(f, filmDuration, filmDuration)
		markReady(f, a, b)
		startPlayback(f, a, a, b)
		f.waitFor("lecture engagée", func() bool { return a.pos() > 1 && b.pos() > 1 })
		dialsBefore := b.dialer.count()

		// Coupure brutale de la session de B, sous les pieds du moteur.
		if err := b.dialer.cutLast(); err != nil {
			t.Fatalf("coupure de la connexion de B: %v", err)
		}

		f.waitFor("A est prévenu du départ de B", func() bool {
			return a.hasToast(protocol.LevelWarn, "déconnecté") ||
				a.hasToast(protocol.LevelInfo, "a quitté la salle")
		})
		f.waitFor("pause automatique chez A", func() bool {
			return a.snap().Paused && a.vlcState() == "paused"
		})

		// B se reconnecte tout seul (backoff) et resynchronise sur la salle.
		f.waitFor("B a rouvert une connexion", func() bool { return b.dialer.count() > dialsBefore })
		f.waitFor("B est de nouveau connecté", func() bool {
			return b.snap().Phase == client.PhaseConnected && len(b.snap().Users) == 2
		})
		f.waitUpTo(convergeTimeout, "B est recalé sur la position de la salle", func() bool {
			return b.vlcState() == "paused" && math.Abs(b.snap().DriftSec) < driftMax
		})
		f.waitUpTo(convergeTimeout, "drift entre A et B < 0,5 s", func() bool {
			return pairDrift(a, b) < driftMax
		})
	})

	// Le faux VLC démarre en lecture à l'ouverture, comme le vrai : les deux
	// médias ne s'ouvrent jamais exactement au même instant, et un écart de
	// départ de l'ordre de la demi-seconde est la norme. Il doit être résorbé
	// par le calage au départ (seek), pas par le nudge — qui mettrait ~11 s
	// pour 0,55 s à 5 %/s (docs/protocol.md §Départ et reprise de lecture).
	t.Run("08-depart-avec-autoplay", func(t *testing.T) {
		f := newFixture(t)
		a, b := joinBoth(f, filmDuration, filmDuration)
		markReady(f, a, b)

		// On place la salle à 100 s, à l'arrêt, les deux lecteurs calés dessus.
		a.eng.Seek(100)
		f.waitFor("les deux lecteurs sont calés sur 100 s à l'arrêt", func() bool {
			return math.Abs(a.pos()-100) < 0.6 && math.Abs(b.pos()-100) < 0.6 &&
				a.vlcState() != "playing" && b.vlcState() != "playing"
		})

		// Le média de bob a pris du retard à l'ouverture : 0,55 s, juste sous le
		// seuil de correction en pause (0,6 s), donc rien ne le recale.
		b.fake.SeekTo(99.45)
		f.holds(600*time.Millisecond, "l'écart de départ n'est pas corrigé en pause", func() bool {
			return b.vlcState() != "playing" && b.pos() < 99.9
		})

		start := time.Now()
		startPlayback(f, a, a, b)
		// Discriminant : chaque moteur doit être calé sur la salle presque tout
		// de suite. Sans le seek de calage, bob resterait à ~0,55 s et mettrait
		// plus de 5 s à repasser sous 0,3 s au rythme du nudge.
		f.waitUpTo(1500*time.Millisecond, "chaque moteur est calé sur la salle dès le départ",
			func() bool {
				return math.Abs(a.snap().DriftSec) < client.StartAlignSec &&
					math.Abs(b.snap().DriftSec) < client.StartAlignSec
			})
		f.waitUpTo(2*time.Second, "les deux lecteurs sont synchronisés",
			func() bool { return pairDrift(a, b) < driftMax })
		t.Logf("synchronisation atteinte en %s (le nudge seul aurait mis ~11 s pour 0,55 s)",
			time.Since(start).Round(time.Millisecond))
		f.holds(2*time.Second, "la synchronisation tient après le départ décalé", func() bool {
			return pairDrift(a, b) < driftMax && a.vlcState() == "playing" && b.vlcState() == "playing"
		})
	})

	// VS-017, reproduction du bug terrain : Thibault manipulait VLC (seek,
	// pause, retour) et voyait ses commandes écrasées par des « Pause auto : X
	// bufferise ». Le seek fige la position, le détecteur criait au buffering,
	// le serveur mettait la salle en pause.
	t.Run("09-seeks-utilisateur-sans-pause-auto", func(t *testing.T) {
		f := newFixture(t)
		a, b := joinBoth(f, filmDuration, filmDuration)
		markReady(f, a, b)
		startPlayback(f, a, a, b)
		f.waitFor("lecture engagée", func() bool { return a.pos() > 1 && b.pos() > 1 })

		// Loin dans le film : les sauts en arrière ne butent pas sur le début.
		a.eng.Seek(1200)
		f.waitFor("les deux lecteurs sont vers 1200 s", func() bool {
			return a.pos() > 1150 && b.pos() > 1150
		})
		aucunePauseAuto := func() bool {
			for _, p := range f.list() {
				if p.hasToast(protocol.LevelWarn, "pause auto") {
					return false
				}
			}
			return true
		}

		// Rafale de seeks faits directement dans VLC. Chaque saut fige la
		// position le temps que le lecteur cherche — plus longtemps que les
		// 700 ms du détecteur de buffering, c'est tout le problème de VS-017.
		for range 3 {
			b.fake.SetStalled(true)
			b.fake.SeekTo(b.pos() - 30)
			f.holds(1200*time.Millisecond, "aucune pause auto pendant la recherche", aucunePauseAuto)
			b.fake.SetStalled(false)
			f.holds(800*time.Millisecond, "aucune pause auto une fois la recherche finie", aucunePauseAuto)
		}

		if !aucunePauseAuto() {
			f.t.Fatalf("pause auto déclenchée par les seeks de l'utilisateur\n%s", f.dump())
		}
		f.waitFor("la lecture n'a jamais été interrompue", func() bool {
			return a.vlcState() == "playing" && b.vlcState() == "playing" && !a.snap().Paused
		})

		// Deuxième temps : hors de toute fenêtre de seek, un vrai blocage doit
		// toujours figer la salle — la suspension est bornée, pas définitive.
		f.waitUpTo(convergeTimeout, "les deux lecteurs se sont recalés après la rafale", func() bool {
			return math.Abs(a.snap().DriftSec) < 0.3 && math.Abs(b.snap().DriftSec) < 0.3
		})
		f.holds(1500*time.Millisecond, "plus aucune correction en cours", func() bool {
			return a.vlcState() == "playing" && b.vlcState() == "playing"
		})

		b.fake.SetStalled(true)
		f.waitFor("B diagnostique son blocage", func() bool { return b.snap().VLC.Buffering })
		f.waitFor("A reçoit le toast de pause auto", func() bool {
			return a.hasToast(protocol.LevelWarn, "bufferise")
		})
		f.waitFor("la salle est en pause pour tout le monde", func() bool {
			return a.snap().Paused && b.snap().Paused
		})
	})

	// VS-021 : tout le monde crashe en pleine séance et revient deux minutes
	// plus tard — la salle a gardé le timecode.
	t.Run("10-reprise-de-seance-apres-crash", func(t *testing.T) {
		f := newFixture(t)
		a, b := joinBoth(f, filmDuration, filmDuration)
		markReady(f, a, b)
		startPlayback(f, a, a, b)
		f.waitFor("lecture engagée", func() bool { return a.pos() > 2 && b.pos() > 2 })

		// Crash de B : le serveur met la salle en pause (déconnexion en lecture).
		b.eng.Disconnect()
		f.waitFor("A est seul et la salle est figée", func() bool {
			s := a.snap()
			return len(s.Users) == 1 && s.Paused && a.vlcState() == "paused"
		})
		want := a.snap().RoomPosition
		if want < 2 {
			t.Fatalf("position de séance inattendue avant le crash: %v\n%s", want, f.dump())
		}

		// Crash de A : la salle se vide, la séance est mise de côté.
		a.eng.Disconnect()
		f.waitFor("A est déconnecté", func() bool { return a.snap().Phase == client.PhaseIdle })

		// Deux minutes passent, personne n'est là.
		f.clock.Advance(2 * time.Minute)

		a.reconnect()
		f.waitFor("A est de retour", func() bool { return a.snap().Phase == client.PhaseConnected })
		b.reconnect()
		f.waitFor("les deux sont de retour", func() bool {
			return len(a.snap().Users) == 2 && len(b.snap().Users) == 2 &&
				b.snap().Phase == client.PhaseConnected
		})

		for _, p := range []*peer{a, b} {
			s := p.snap()
			if !s.Paused {
				t.Fatalf("%s : la séance reprise doit être en pause\n%s", p.name, f.dump())
			}
			if math.Abs(s.RoomPosition-want) > 1 {
				t.Fatalf("%s : position reprise %.2f s, attendue %.2f s (à la seconde près)\n%s",
					p.name, s.RoomPosition, want, f.dump())
			}
		}
		if !a.hasToast(protocol.LevelInfo, "séance reprise à") {
			t.Fatalf("le revenant doit être informé du timecode retrouvé\n%s", f.dump())
		}
		// Les lecteurs eux-mêmes retournent au bon endroit.
		f.waitUpTo(convergeTimeout, "les deux VLC sont recalés sur la séance retrouvée", func() bool {
			return math.Abs(a.pos()-want) < 1 && math.Abs(b.pos()-want) < 1
		})
	})

	// VS-024 (a) : le serveur disparaît en pleine séance (redéploiement Coolify)
	// et revient tout neuf, sans mémoire des salles. Les lecteurs ne doivent ni
	// s'arrêter ni repartir de zéro : le premier revenant propose sa position.
	t.Run("11-redemarrage-du-serveur", func(t *testing.T) {
		f := newFixture(t)
		a, b := joinBoth(f, filmDuration, filmDuration)
		markReady(f, a, b)
		startPlayback(f, a, a, b)
		// Au-delà du seuil de reprise : en deçà, la séance est considérée comme
		// n'ayant pas commencé et le client se range derrière la salle vierge.
		f.waitFor("la séance a dépassé le seuil de reprise", func() bool {
			return a.pos() > client.VirginResumeSec+1 && b.pos() > client.VirginResumeSec+1
		})
		avant := a.pos()

		f.restartServer()

		// Tant que la connexion n'est pas revenue, VLC continue tout seul.
		f.holds(300*time.Millisecond, "la lecture continue localement pendant la coupure",
			func() bool {
				if a.snap().Phase == client.PhaseConnected {
					return true // déjà revenu : l'assertion ne s'applique plus
				}
				return a.vlcState() == "playing" && b.vlcState() == "playing"
			})

		f.waitFor("les deux clients sont revenus", func() bool {
			return a.snap().Phase == client.PhaseConnected && b.snap().Phase == client.PhaseConnected &&
				len(a.snap().Users) == 2 && len(b.snap().Users) == 2
		})
		// La reprise vise la dernière position de salle connue de chacun : elles
		// sont quasi identiques, et surtout ce n'est pas un retour à zéro.
		f.waitFor("la séance est reprise là où elle en était", func() bool {
			ra, rb := a.snap().RoomPosition, b.snap().RoomPosition
			return ra > client.VirginResumeSec && math.Abs(ra-avant) < 2 &&
				math.Abs(ra-rb) < 1
		})
		if !a.hasToastPrefix("Reprise à ") && !b.hasToastPrefix("Reprise à ") {
			t.Fatalf("aucun client n'a annoncé la reprise de la séance\n%s", f.dump())
		}
		f.waitUpTo(convergeTimeout, "les deux lecteurs sont recalés sur la séance reprise",
			func() bool {
				return math.Abs(a.pos()-a.snap().RoomPosition) < 1 &&
					math.Abs(b.pos()-b.snap().RoomPosition) < 1
			})
		// Le ready survit au redémarrage : la salle neuve doit pouvoir repartir
		// sans que personne ne reclique.
		f.waitFor("les deux membres sont de nouveau prêts côté serveur", func() bool {
			return a.ready() && b.ready()
		})
		f.holds(time.Second, "aucun lecteur n'est revenu au début", func() bool {
			return a.pos() > avant-driftMax && b.pos() > avant-driftMax
		})
	})

	// VS-024 (b) : ce qu'on écrit pendant une coupure RÉSEAU part au retour, dans
	// l'ordre. C'est bien une reconnexion automatique : un départ volontaire
	// jetterait la file (elle appartient à la salle qu'on quitte).
	t.Run("12-chat-compose-hors-ligne", func(t *testing.T) {
		f := newFixture(t)
		a, b := joinBoth(f, filmDuration, filmDuration)

		b.couperReseau()
		f.waitFor("B est hors ligne", func() bool {
			return b.snap().Phase != client.PhaseConnected
		})

		envoyes := []string{"je perds la connexion", "vous m'entendez ?", "je reviens"}
		for _, msg := range envoyes {
			b.eng.Chat(msg)
		}
		if got := b.snap().PendingChats; len(got) != len(envoyes) {
			t.Fatalf("file « en attente » = %v, attendu %v\n%s", got, envoyes, f.dump())
		}
		f.holds(400*time.Millisecond, "rien n'arrive à A tant que B est hors ligne", func() bool {
			return len(a.chatTexts()) == 0
		})

		b.retablirReseau()
		f.waitFor("A a reçu les trois messages", func() bool { return len(a.chatTexts()) >= 3 })
		if got := a.chatTexts(); !slices.Equal(got, envoyes) {
			t.Fatalf("messages reçus %v, attendus %v (dans l'ordre)\n%s", got, envoyes, f.dump())
		}
		if got := b.snap().PendingChats; len(got) != 0 {
			t.Fatalf("file non vidée après la reconnexion: %v", got)
		}
	})

	// VS-024 (c) : le serveur est vivant, c'est la connexion du client qui
	// flanche. À son retour il se cale sur la salle — et surtout il ne lui
	// impose pas son propre timecode.
	t.Run("13-coupure-client-sans-ecrasement", func(t *testing.T) {
		f := newFixture(t)
		a := f.newPeer("alice", filmDuration)
		markReady(f, a)
		startPlayback(f, a, a)
		f.waitFor("lecture engagée", func() bool { return a.pos() > 2 })

		a.eng.Disconnect()
		f.waitFor("A est hors ligne", func() bool { return a.snap().Phase == client.PhaseIdle })
		// La salle a figé la séance à ce moment-là ; VLC, lui, continue.
		fige := a.pos()
		f.holds(1500*time.Millisecond, "VLC continue de jouer hors ligne", func() bool {
			return a.vlcState() == "playing"
		})
		if a.pos() <= fige {
			t.Fatalf("le lecteur n'avance plus hors ligne (%v)\n%s", a.pos(), f.dump())
		}
		// Trente secondes d'absence côté serveur.
		f.clock.Advance(30 * time.Second)

		a.reconnect()
		f.waitFor("A est revenu", func() bool { return a.snap().Phase == client.PhaseConnected })
		f.waitUpTo(convergeTimeout, "A s'est recalé sur la séance conservée", func() bool {
			return math.Abs(a.pos()-fige) < 1.5 && a.vlcState() == "paused"
		})
		if got := a.snap().RoomPosition; math.Abs(got-fige) > 1 {
			t.Fatalf("la salle a été écrasée : position %.2f, séance figée à %.2f\n%s",
				got, fige, f.dump())
		}
		if a.hasToastPrefix("Reprise à ") {
			t.Fatalf("reprise « salle vierge » émise alors que la salle avait sa séance\n%s", f.dump())
		}
	})

	t.Run("07-fichiers-de-durees-differentes", func(t *testing.T) {
		f := newFixture(t)
		// B déclare 10 s de plus que A : le serveur avertit sans bloquer.
		a, b := joinBoth(f, filmDuration, filmDuration+10)
		f.waitFor("A est averti de la différence de durée", func() bool {
			return a.hasToast(protocol.LevelWarn, "durées différentes")
		})
		f.waitFor("B reçoit le même avertissement", func() bool {
			return b.hasToast(protocol.LevelWarn, "durées différentes")
		})
		// Non bloquant : la lecture reste possible.
		markReady(f, a, b)
		startPlayback(f, a, a, b)
	})
}
