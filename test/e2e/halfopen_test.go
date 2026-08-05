package e2e_test

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"github.com/thibsix/vibesync/internal/client"
	"github.com/thibsix/vibesync/internal/protocol"
	"github.com/thibsix/vibesync/internal/vlc"
	"github.com/thibsix/vibesync/internal/vlc/vlctest"
)

// halfOpenConn simule une coupure réseau *silencieuse* (câble arraché, Wi-Fi
// perdu, NAT qui oublie la session) : le moteur voit sa connexion mourir, mais
// la socket reste ouverte côté serveur, qui continue donc de compter le membre
// jusqu'à l'expiration de son deadline de lecture (60 s).
type halfOpenConn struct {
	inner client.Conn
	msgs  chan []byte
	errCh chan error
	cutCh chan struct{}
	once  sync.Once
}

func newHalfOpenConn(inner client.Conn) *halfOpenConn {
	h := &halfOpenConn{
		inner: inner,
		msgs:  make(chan []byte, 32),
		errCh: make(chan error, 1),
		cutCh: make(chan struct{}),
	}
	go func() {
		for {
			m, err := inner.ReadMessage()
			if err != nil {
				select {
				case h.errCh <- err:
				default:
				}
				return
			}
			select {
			case h.msgs <- m:
			default: // après la coupure on jette : la socket reste drainée
			}
		}
	}()
	return h
}

func (h *halfOpenConn) ReadMessage() ([]byte, error) {
	select {
	case <-h.cutCh:
		return nil, errors.New("coupure réseau silencieuse (simulation)")
	case err := <-h.errCh:
		return nil, err
	case m := <-h.msgs:
		return m, nil
	}
}

func (h *halfOpenConn) WriteMessage(data []byte) error {
	select {
	case <-h.cutCh:
		return errors.New("coupure réseau silencieuse (simulation)")
	default:
	}
	return h.inner.WriteMessage(data)
}

// Close ne ferme pas la socket sous-jacente : c'est tout l'intérêt de la
// simulation (le serveur ne voit rien).
func (h *halfOpenConn) Close() error {
	h.cut()
	return nil
}

func (h *halfOpenConn) cut() { h.once.Do(func() { close(h.cutCh) }) }

// closeForReal libère vraiment la socket (nettoyage de fin de test).
func (h *halfOpenConn) closeForReal() { h.cut(); _ = h.inner.Close() }

// halfOpenDialer enveloppe la première connexion pour pouvoir la « perdre »
// silencieusement ; les suivantes (reconnexions) sont normales.
type halfOpenDialer struct {
	inner client.Dialer

	mu    sync.Mutex
	first *halfOpenConn
	all   []*halfOpenConn
	dials int
}

func (d *halfOpenDialer) Dial(ctx context.Context, url string) (client.Conn, error) {
	c, err := d.inner.Dial(ctx, url)
	if err != nil {
		return nil, err
	}
	h := newHalfOpenConn(c)
	d.mu.Lock()
	if d.first == nil {
		d.first = h
	}
	d.all = append(d.all, h)
	d.dials++
	d.mu.Unlock()
	return h, nil
}

func (d *halfOpenDialer) count() int {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.dials
}

func (d *halfOpenDialer) cutFirst() {
	d.mu.Lock()
	first := d.first
	d.mu.Unlock()
	if first != nil {
		first.cut()
	}
}

func (d *halfOpenDialer) cleanup() {
	d.mu.Lock()
	all := append([]*halfOpenConn(nil), d.all...)
	d.mu.Unlock()
	for _, h := range all {
		h.closeForReal()
	}
}

// TestRejoinApresCoupureSilencieuse valide la reprise de session
// (docs/protocol.md §Comportements serveur, point 6) : après une coupure réseau
// silencieuse, le serveur tient encore l'ancien membre (jusqu'à 60 s, son
// deadline de lecture). Le client se reconnecte avec le même pseudo ET le même
// jeton `session` : le serveur ferme la connexion zombie et lui rend sa place,
// sans attendre l'expiration et sans `name_taken`.
//
// Historique : ce scénario a d'abord servi de reproduction au bug 1 découvert
// pendant l'e2e (name_taken fatal, abandon définitif) ; voir
// docs/research/2026-08-05-rapport-e2e.md §Fix reprise de session.
func TestRejoinApresCoupureSilencieuse(t *testing.T) {
	f := newFixture(t)
	a := f.newPeer("alice", filmDuration)

	// Bob, avec un dialer capable de perdre silencieusement la connexion.
	fake := vlctest.New(time.Now)
	t.Cleanup(fake.Close)
	fake.LoadFile("film.mkv", filmDuration)

	dialer := &halfOpenDialer{inner: client.WSDialer{}}
	t.Cleanup(dialer.cleanup)
	eng := client.New(client.Config{
		Dialer:  dialer,
		Logger:  quietLogger(),
		Locator: func() (string, error) { return "/faux/vlc", nil },
		Launcher: func(context.Context, string) (vlc.Controller, error) {
			return vlc.NewHTTPClient(fake.URL(), fake.Password()), nil
		},
		InitialBackoff: 300 * time.Millisecond,
		MaxBackoff:     time.Second,
	})
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(func() {
		cancel()
		_ = eng.Close()
	})
	go eng.Run(ctx)

	b := &peer{name: "bob", eng: eng, fake: fake, fx: f, dialCount: dialer.count}
	events, unsub := eng.Subscribe()
	t.Cleanup(unsub)
	go b.collect(events)
	f.mu.Lock()
	f.peers = append(f.peers, b)
	f.mu.Unlock()

	eng.Connect(client.ConnectRequest{URL: f.url, Name: "bob", Room: f.room})
	f.waitFor("bob connecté", func() bool { return b.snap().Phase == client.PhaseConnected })
	path := filepath.Join(t.TempDir(), "film.mkv")
	if err := os.WriteFile(path, []byte("données vidéo"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := eng.OpenFile(ctx, path); err != nil {
		t.Fatalf("OpenFile: %v", err)
	}
	f.waitFor("les deux membres se voient", func() bool {
		return len(a.snap().Users) == 2 && len(b.snap().Users) == 2
	})

	// Le réseau de bob disparaît sans que le serveur s'en aperçoive.
	dialsBefore := dialer.count()
	dialer.cutFirst()

	f.waitFor("bob a retenté une connexion", func() bool { return dialer.count() > dialsBefore })

	// Reprise de session : bob retrouve sa place tout de suite, sans attendre
	// l'expiration du fantôme (60 s) et sans erreur fatale.
	f.waitFor("bob retrouve la salle par reprise de session", func() bool {
		return b.snap().Phase == client.PhaseConnected
	})
	if err := b.snap().LastError; err != "" {
		t.Fatalf("erreur remontée alors que la reprise doit être transparente: %q\n%s", err, f.dump())
	}
	f.waitFor("la salle ne compte toujours que deux membres", func() bool {
		return len(a.snap().Users) == 2 && len(b.snap().Users) == 2
	})
	// Le fantôme a bien été remplacé, pas dupliqué : alice n'a jamais vu bob partir.
	if a.hasToast(protocol.LevelInfo, "a quitté la salle") {
		t.Fatalf("toast de départ émis alors que bob a repris sa session\n%s", f.dump())
	}
	// Et bob est de nouveau pilotable : il commande la salle.
	b.eng.Pause()
	f.waitFor("le control de bob est pris en compte", func() bool { return a.snap().Paused })
}
