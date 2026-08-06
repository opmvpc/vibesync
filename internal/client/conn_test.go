package client

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/opmvpc/vibesync/internal/protocol"
	"github.com/opmvpc/vibesync/internal/vlc"
	"github.com/opmvpc/vibesync/internal/ws"
)

// fakeServer est un serveur WebSocket factice minimal (le vrai serveur est
// implémenté ailleurs ; ces tests ne doivent pas en dépendre).
type fakeServer struct {
	srv *httptest.Server

	mu     sync.Mutex
	hellos []protocol.Hello

	// handle est appelé après réception du hello ; n est le numéro de session.
	handle func(c *ws.Conn, hello protocol.Hello, n int)
}

func newFakeServer(t *testing.T, handle func(c *ws.Conn, hello protocol.Hello, n int)) *fakeServer {
	t.Helper()
	fs := &fakeServer{handle: handle}
	mux := http.NewServeMux()
	mux.HandleFunc("/ws", func(w http.ResponseWriter, r *http.Request) {
		c, err := ws.Upgrade(w, r)
		if err != nil {
			return
		}
		defer func() { _ = c.CloseNow() }()
		_, raw, err := c.ReadMessage()
		if err != nil {
			return
		}
		env, err := protocol.Decode(raw)
		if err != nil || env.Type != protocol.TypeHello {
			return
		}
		hello, err := protocol.DecodeData[protocol.Hello](env)
		if err != nil {
			return
		}
		fs.mu.Lock()
		fs.hellos = append(fs.hellos, hello)
		n := len(fs.hellos)
		fs.mu.Unlock()
		if fs.handle != nil {
			fs.handle(c, hello, n)
		}
	})
	fs.srv = httptest.NewServer(mux)
	t.Cleanup(fs.srv.Close)
	return fs
}

func (fs *fakeServer) url() string {
	return "ws" + strings.TrimPrefix(fs.srv.URL, "http") + "/ws"
}

func (fs *fakeServer) helloCount() int {
	fs.mu.Lock()
	defer fs.mu.Unlock()
	return len(fs.hellos)
}

func (fs *fakeServer) lastHello() protocol.Hello {
	fs.mu.Lock()
	defer fs.mu.Unlock()
	if len(fs.hellos) == 0 {
		return protocol.Hello{}
	}
	return fs.hellos[len(fs.hellos)-1]
}

func sendMsg(t *testing.T, c *ws.Conn, msgType string, data any) {
	t.Helper()
	raw, err := protocol.Encode(msgType, data)
	if err != nil {
		t.Errorf("encode %s: %v", msgType, err)
		return
	}
	if err := c.WriteMessage(ws.TextMessage, raw); err != nil {
		return
	}
}

func newNetEngine(t *testing.T) *Engine {
	t.Helper()
	e := New(Config{
		Locator:        func() (string, error) { return "", vlc.ErrNotFound },
		InitialBackoff: 10 * time.Millisecond,
		MaxBackoff:     40 * time.Millisecond,
		PollInterval:   time.Hour, // pas de VLC dans ces tests
	})
	t.Cleanup(func() { _ = e.Close() })
	return e
}

func waitFor(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatalf("délai dépassé en attendant: %s", what)
}

func TestConnexionHelloPuisWelcome(t *testing.T) {
	fs := newFakeServer(t, func(c *ws.Conn, _ protocol.Hello, _ int) {
		sendMsg(t, c, protocol.TypeWelcome, protocol.Welcome{
			SelfID: "u7", Room: "soirée",
			State: protocol.RoomState{Paused: true, Rate: 1},
			Users: []protocol.User{{ID: "u7", Name: "thib"}},
		})
		for {
			if _, _, err := c.ReadMessage(); err != nil {
				return
			}
		}
	})
	e := newNetEngine(t)
	e.Connect(ConnectRequest{URL: fs.url(), Name: "thib", Room: "soirée", Password: "s3cr3t"})

	waitFor(t, "phase connectée", func() bool { return e.Snapshot().Phase == PhaseConnected })
	h := fs.lastHello()
	if h.Version != protocol.Version || h.Name != "thib" || h.Room != "soirée" || h.Password != "s3cr3t" {
		t.Fatalf("hello inattendu: %+v", h)
	}
	if got := e.Snapshot().SelfID; got != "u7" {
		t.Fatalf("selfId = %q", got)
	}
}

func TestPongMetAJourLaLatence(t *testing.T) {
	fs := newFakeServer(t, func(c *ws.Conn, _ protocol.Hello, _ int) {
		sendMsg(t, c, protocol.TypeWelcome, protocol.Welcome{SelfID: "u1", State: protocol.RoomState{Paused: true, Rate: 1}})
		for {
			_, raw, err := c.ReadMessage()
			if err != nil {
				return
			}
			env, err := protocol.Decode(raw)
			if err != nil || env.Type != protocol.TypePing {
				continue
			}
			p, err := protocol.DecodeData[protocol.Ping](env)
			if err != nil {
				continue
			}
			sendMsg(t, c, protocol.TypePong, protocol.Pong{T: p.T, ServerMs: time.Now().UnixMilli()})
		}
	})
	e := newNetEngine(t)
	e.Connect(ConnectRequest{URL: fs.url(), Name: "thib", Room: "r"})
	// Le welcome déclenche un ping immédiat.
	waitFor(t, "offset d'horloge calculé", func() bool {
		e.mu.Lock()
		defer e.mu.Unlock()
		return len(e.offsets) > 0
	})
}

func TestReconnexionAvecReHello(t *testing.T) {
	fs := newFakeServer(t, func(c *ws.Conn, _ protocol.Hello, n int) {
		sendMsg(t, c, protocol.TypeWelcome, protocol.Welcome{
			SelfID: "u1", Room: "r",
			State: protocol.RoomState{Paused: true, Rate: 1},
		})
		if n < 3 {
			return // coupure brutale : le client doit se reconnecter
		}
		for {
			if _, _, err := c.ReadMessage(); err != nil {
				return
			}
		}
	})
	e := newNetEngine(t)
	e.Connect(ConnectRequest{URL: fs.url(), Name: "thib", Room: "r"})

	waitFor(t, "3 hellos (reconnexions)", func() bool { return fs.helloCount() >= 3 })
	waitFor(t, "reconnexion stabilisée", func() bool { return e.Snapshot().Phase == PhaseConnected })
	if h := fs.lastHello(); h.Name != "thib" || h.Room != "r" {
		t.Fatalf("re-hello incorrect: %+v", h)
	}
}

func TestReconnexionSurServeurInjoignable(t *testing.T) {
	e := newNetEngine(t)
	e.Connect(ConnectRequest{URL: "ws://127.0.0.1:1/ws", Name: "thib", Room: "r"})
	waitFor(t, "tentatives de reconnexion signalées", func() bool {
		s := e.Snapshot()
		return s.Retrying && s.LastError != ""
	})
	e.Disconnect()
	if got := e.Snapshot().Phase; got != PhaseIdle {
		t.Fatalf("phase après Disconnect = %q", got)
	}
}

func TestErreurFataleStoppeLaReconnexion(t *testing.T) {
	fs := newFakeServer(t, func(c *ws.Conn, _ protocol.Hello, _ int) {
		sendMsg(t, c, protocol.TypeError, protocol.ErrorMsg{Code: protocol.ErrBadPassword, Text: "mot de passe incorrect"})
		time.Sleep(20 * time.Millisecond)
	})
	e := newNetEngine(t)
	e.Connect(ConnectRequest{URL: fs.url(), Name: "thib", Room: "r"})
	waitFor(t, "erreur remontée", func() bool { return e.Snapshot().LastError != "" })
	time.Sleep(300 * time.Millisecond)
	if n := fs.helloCount(); n != 1 {
		t.Fatalf("%d tentatives, une erreur fatale ne doit pas être réessayée", n)
	}
	if got := e.Snapshot().Phase; got != PhaseIdle {
		t.Fatalf("phase = %q, attendu idle", got)
	}
}

func TestDisconnectFermeLaSession(t *testing.T) {
	closed := make(chan struct{}, 4)
	fs := newFakeServer(t, func(c *ws.Conn, _ protocol.Hello, _ int) {
		sendMsg(t, c, protocol.TypeWelcome, protocol.Welcome{SelfID: "u1", State: protocol.RoomState{Paused: true, Rate: 1}})
		for {
			if _, _, err := c.ReadMessage(); err != nil {
				closed <- struct{}{}
				return
			}
		}
	})
	e := newNetEngine(t)
	e.Connect(ConnectRequest{URL: fs.url(), Name: "thib", Room: "r"})
	waitFor(t, "connexion établie", func() bool { return e.Snapshot().Phase == PhaseConnected })
	e.Disconnect()
	select {
	case <-closed:
	case <-time.After(2 * time.Second):
		t.Fatal("le serveur n'a pas vu la fermeture")
	}
	time.Sleep(200 * time.Millisecond)
	if n := fs.helloCount(); n != 1 {
		t.Fatalf("%d hellos, aucune reconnexion ne devait suivre un Disconnect", n)
	}
}
