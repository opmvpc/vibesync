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
	"net/http/httptest"
	"os"
	"path/filepath"
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

// --- Dialer instrumenté : permet de couper brutalement une session ---

type recordingDialer struct {
	inner client.Dialer

	mu    sync.Mutex
	last  client.Conn
	dials int
}

func (d *recordingDialer) Dial(ctx context.Context, url string) (client.Conn, error) {
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

func (p *peer) ready() bool {
	for _, u := range p.snap().Users {
		if u.Name == p.name {
			return u.Ready
		}
	}
	return false
}

// --- Fixture : serveur + pairs ---

type fixture struct {
	t     *testing.T
	url   string
	room  string
	start time.Time

	mu    sync.Mutex
	peers []*peer
}

func newFixture(t *testing.T) *fixture {
	t.Helper()
	srv := server.New(server.Config{}, server.WithLogger(quietLogger()))
	ts := httptest.NewServer(srv.Handler())
	t.Cleanup(ts.Close)
	room := strings.NewReplacer("/", "-", " ", "-").Replace(t.Name())
	return &fixture{
		t:     t,
		url:   "ws://" + strings.TrimPrefix(ts.URL, "http://") + "/ws",
		room:  room,
		start: time.Now(),
	}
}

// newPeer branche un moteur complet sur son faux VLC et le connecte à la salle.
func (f *fixture) newPeer(name string, durationSec float64) *peer {
	t := f.t
	t.Helper()

	fake := vlctest.New(time.Now)
	t.Cleanup(fake.Close)
	fake.LoadFile("film.mkv", durationSec)

	dialer := &recordingDialer{inner: client.WSDialer{}}
	eng := client.New(client.Config{
		Dialer:  dialer,
		Logger:  quietLogger(),
		Locator: func() (string, error) { return "/faux/vlc", nil },
		Launcher: func(context.Context, string) (vlc.Controller, error) {
			return vlc.NewHTTPClient(fake.URL(), fake.Password()), nil
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

	eng.Connect(client.ConnectRequest{URL: f.url, Name: name, Room: f.room})
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
