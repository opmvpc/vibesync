package webui

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/opmvpc/vibesync/internal/client"
	"github.com/opmvpc/vibesync/internal/protocol"
	"github.com/opmvpc/vibesync/internal/ws"
)

// stubEngine enregistre les appels de l'UI.
type stubEngine struct {
	mu        sync.Mutex
	connected []client.ConnectRequest
	opened    []string
	ready     []bool
	seeks     []float64
	chats     []string
	plays     int
	pauses    int
	discos    int
	openErr   error
	events    chan client.Event
}

func newStub() *stubEngine { return &stubEngine{events: make(chan client.Event, 16)} }

func (s *stubEngine) Connect(req client.ConnectRequest) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.connected = append(s.connected, req)
}
func (s *stubEngine) Disconnect() { s.mu.Lock(); s.discos++; s.mu.Unlock() }
func (s *stubEngine) OpenFile(_ context.Context, p string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.opened = append(s.opened, p)
	return s.openErr
}
func (s *stubEngine) SetReady(r bool) { s.mu.Lock(); s.ready = append(s.ready, r); s.mu.Unlock() }
func (s *stubEngine) Play()           { s.mu.Lock(); s.plays++; s.mu.Unlock() }
func (s *stubEngine) Pause()          { s.mu.Lock(); s.pauses++; s.mu.Unlock() }
func (s *stubEngine) Seek(p float64)  { s.mu.Lock(); s.seeks = append(s.seeks, p); s.mu.Unlock() }
func (s *stubEngine) Chat(t string)   { s.mu.Lock(); s.chats = append(s.chats, t); s.mu.Unlock() }
func (s *stubEngine) Snapshot() client.Snapshot {
	return client.Snapshot{Phase: client.PhaseIdle, Room: "soirée"}
}
func (s *stubEngine) Subscribe() (<-chan client.Event, func()) { return s.events, func() {} }

func newTestServer(t *testing.T) (*Server, *stubEngine, *httptest.Server) {
	t.Helper()
	stub := newStub()
	s, err := New(stub, Options{Token: "jeton-test"})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	ts := httptest.NewServer(s.Handler())
	t.Cleanup(ts.Close)
	return s, stub, ts
}

func TestIndexEtAssetsEmbarques(t *testing.T) {
	_, _, ts := newTestServer(t)
	resp, err := http.Get(ts.URL + "/")
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = resp.Body.Close() }()
	if resp.StatusCode != 200 {
		t.Fatalf("index: HTTP %d", resp.StatusCode)
	}
	buf := make([]byte, 4096)
	n, _ := resp.Body.Read(buf)
	body := string(buf[:n])
	if !strings.Contains(body, "jeton-test") {
		t.Fatal("le token n'a pas été injecté dans la page")
	}
	if strings.Contains(body, "__UI_TOKEN__") {
		t.Fatal("placeholder de token non remplacé")
	}
	for _, path := range []string{"/static/app.css", "/static/app.js"} {
		r, err := http.Get(ts.URL + path)
		if err != nil {
			t.Fatal(err)
		}
		_ = r.Body.Close()
		if r.StatusCode != 200 {
			t.Fatalf("%s: HTTP %d", path, r.StatusCode)
		}
	}
}

func TestAPIFSNavigation(t *testing.T) {
	_, _, ts := newTestServer(t)
	dir := t.TempDir()
	if err := os.Mkdir(filepath.Join(dir, "saison1"), 0o755); err != nil {
		t.Fatal(err)
	}
	for _, n := range []string{"ep1.mkv", "lisezmoi.txt"} {
		if err := os.WriteFile(filepath.Join(dir, n), []byte("x"), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	resp, err := http.Get(ts.URL + "/api/fs?token=jeton-test&path=" + dir)
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = resp.Body.Close() }()
	if resp.StatusCode != 200 {
		t.Fatalf("HTTP %d", resp.StatusCode)
	}
	var listing Listing
	if err := json.NewDecoder(resp.Body).Decode(&listing); err != nil {
		t.Fatal(err)
	}
	if len(listing.Entries) != 2 {
		t.Fatalf("entrées = %+v (attendu le dossier + ep1.mkv)", listing.Entries)
	}
	if !listing.Entries[0].IsDir || listing.Entries[1].Name != "ep1.mkv" {
		t.Fatalf("tri/filtre incorrects: %+v", listing.Entries)
	}
}

func TestAPIFSDossierInvalide(t *testing.T) {
	_, _, ts := newTestServer(t)
	resp, err := http.Get(ts.URL + "/api/fs?token=jeton-test&path=" + filepath.Join(t.TempDir(), "absent"))
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = resp.Body.Close() }()
	if resp.StatusCode != http.StatusBadRequest {
		t.Fatalf("HTTP %d, attendu 400", resp.StatusCode)
	}
}

func TestTokenExige(t *testing.T) {
	_, _, ts := newTestServer(t)
	for _, path := range []string{"/api/fs", "/api/fs?token=faux", "/ui", "/ui?token=faux"} {
		resp, err := http.Get(ts.URL + path)
		if err != nil {
			t.Fatal(err)
		}
		_ = resp.Body.Close()
		if resp.StatusCode != http.StatusUnauthorized {
			t.Fatalf("%s: HTTP %d, attendu 401", path, resp.StatusCode)
		}
	}
}

func dialUI(t *testing.T, ts *httptest.Server) *ws.Conn {
	t.Helper()
	url := "ws" + strings.TrimPrefix(ts.URL, "http") + "/ui?token=jeton-test"
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	c, err := ws.Dial(ctx, url)
	if err != nil {
		t.Fatalf("dial /ui: %v", err)
	}
	t.Cleanup(func() { _ = c.CloseNow() })
	return c
}

func readUI(t *testing.T, c *ws.Conn) protocol.Envelope {
	t.Helper()
	_ = c.SetReadDeadline(time.Now().Add(3 * time.Second))
	_, raw, err := c.ReadMessage()
	if err != nil {
		t.Fatalf("lecture /ui: %v", err)
	}
	env, err := protocol.Decode(raw)
	if err != nil {
		t.Fatalf("enveloppe illisible: %v", err)
	}
	return env
}

func sendUI(t *testing.T, c *ws.Conn, msgType string, data any) {
	t.Helper()
	raw, err := protocol.Encode(msgType, data)
	if err != nil {
		t.Fatal(err)
	}
	if err := c.WriteMessage(ws.TextMessage, raw); err != nil {
		t.Fatal(err)
	}
}

func TestUIHandshakeEtEtatInitial(t *testing.T) {
	_, _, ts := newTestServer(t)
	c := dialUI(t, ts)

	env := readUI(t, c)
	if env.Type != MsgHello {
		t.Fatalf("premier message = %q, attendu %q", env.Type, MsgHello)
	}
	hello, err := protocol.DecodeData[HelloMsg](env)
	if err != nil || hello.UIVersion != UIVersion || hello.ProtocolVersion != protocol.Version {
		t.Fatalf("hello = %+v (err=%v)", hello, err)
	}
	// Puis l'état complet (au moins une fois, l'abonnement en pousse un aussi).
	for range 3 {
		env = readUI(t, c)
		if env.Type == MsgState {
			snap, err := protocol.DecodeData[client.Snapshot](env)
			if err != nil {
				t.Fatalf("état illisible: %v", err)
			}
			if snap.Room != "soirée" {
				t.Fatalf("état inattendu: %+v", snap)
			}
			return
		}
	}
	t.Fatal("aucun message d'état reçu")
}

func TestUICommandes(t *testing.T) {
	_, stub, ts := newTestServer(t)
	c := dialUI(t, ts)
	readUI(t, c)

	sendUI(t, c, CmdConnect, ConnectCmd{Server: "exemple.fr:9000", Name: "thib", Room: "soirée", Password: "p"})
	sendUI(t, c, CmdSetReady, SetReadyCmd{Ready: true})
	sendUI(t, c, CmdPlay, struct{}{})
	sendUI(t, c, CmdPause, struct{}{})
	sendUI(t, c, CmdSeek, SeekCmd{PositionSec: 61.5})
	sendUI(t, c, CmdChat, ChatCmd{Text: "salut"})
	sendUI(t, c, CmdDisconnect, struct{}{})

	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		stub.mu.Lock()
		done := stub.discos > 0
		stub.mu.Unlock()
		if done {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	stub.mu.Lock()
	defer stub.mu.Unlock()
	if len(stub.connected) != 1 || stub.connected[0].URL != "wss://exemple.fr:9000/ws" {
		t.Fatalf("connect = %+v (adresse non normalisée ?)", stub.connected)
	}
	if stub.connected[0].Name != "thib" || stub.connected[0].Room != "soirée" || stub.connected[0].Password != "p" {
		t.Fatalf("connect = %+v", stub.connected[0])
	}
	if len(stub.ready) != 1 || !stub.ready[0] {
		t.Fatalf("setReady = %+v", stub.ready)
	}
	if stub.plays != 1 || stub.pauses != 1 || stub.discos != 1 {
		t.Fatalf("play=%d pause=%d disconnect=%d", stub.plays, stub.pauses, stub.discos)
	}
	if len(stub.seeks) != 1 || stub.seeks[0] != 61.5 {
		t.Fatalf("seeks = %+v", stub.seeks)
	}
	if len(stub.chats) != 1 || stub.chats[0] != "salut" {
		t.Fatalf("chats = %+v", stub.chats)
	}
}

func TestUIBrowse(t *testing.T) {
	_, _, ts := newTestServer(t)
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "ep1.mkv"), []byte("x"), 0o600); err != nil {
		t.Fatal(err)
	}
	c := dialUI(t, ts)
	sendUI(t, c, CmdBrowse, BrowseCmd{Path: dir})

	for range 6 {
		env := readUI(t, c)
		if env.Type != MsgBrowse {
			continue
		}
		listing, err := protocol.DecodeData[Listing](env)
		if err != nil {
			t.Fatalf("listing illisible: %v", err)
		}
		if listing.Path != dir || len(listing.Entries) != 1 {
			t.Fatalf("listing = %+v", listing)
		}
		return
	}
	t.Fatal("aucune réponse browse")
}

func TestUIAdresseInvalideRemonteUneErreur(t *testing.T) {
	_, stub, ts := newTestServer(t)
	c := dialUI(t, ts)
	sendUI(t, c, CmdConnect, ConnectCmd{Server: "ftp://nope", Name: "thib", Room: "r"})

	for range 6 {
		env := readUI(t, c)
		if env.Type != MsgError {
			continue
		}
		msg, err := protocol.DecodeData[ErrorMsg](env)
		if err != nil || msg.Code != "badServer" {
			t.Fatalf("erreur = %+v (err=%v)", msg, err)
		}
		stub.mu.Lock()
		defer stub.mu.Unlock()
		if len(stub.connected) != 0 {
			t.Fatal("le moteur a été appelé malgré une adresse invalide")
		}
		return
	}
	t.Fatal("aucune erreur remontée")
}

func TestUICommandeInconnue(t *testing.T) {
	_, _, ts := newTestServer(t)
	c := dialUI(t, ts)
	sendUI(t, c, "futur", struct{}{})
	for range 6 {
		if env := readUI(t, c); env.Type == MsgError {
			return
		}
	}
	t.Fatal("commande inconnue non signalée")
}

func TestUIRelaieLesEvenements(t *testing.T) {
	_, stub, ts := newTestServer(t)
	c := dialUI(t, ts)
	stub.events <- client.Event{Kind: client.EventToast, Toast: &protocol.Toast{Level: protocol.LevelWarn, Text: "fichiers différents"}}
	stub.events <- client.Event{Kind: client.EventChat, Chat: &protocol.ChatEvent{From: "ami", Text: "coucou"}}

	var toast, chat bool
	for range 8 {
		env := readUI(t, c)
		switch env.Type {
		case MsgToast:
			if m, err := protocol.DecodeData[protocol.Toast](env); err == nil && m.Text == "fichiers différents" {
				toast = true
			}
		case MsgChat:
			if m, err := protocol.DecodeData[protocol.ChatEvent](env); err == nil && m.From == "ami" {
				chat = true
			}
		}
		if toast && chat {
			return
		}
	}
	t.Fatalf("relais incomplet: toast=%v chat=%v", toast, chat)
}

func TestStartEtURL(t *testing.T) {
	stub := newStub()
	s, err := New(stub, Options{})
	if err != nil {
		t.Fatal(err)
	}
	if err := s.Start(Options{Addr: "127.0.0.1:0"}); err != nil {
		t.Fatalf("Start: %v", err)
	}
	defer func() { _ = s.Shutdown(context.Background()) }()
	if s.Port() == 0 {
		t.Fatal("port non attribué")
	}
	if !strings.Contains(s.URL(), "127.0.0.1") || !strings.Contains(s.URL(), s.Token()) {
		t.Fatalf("URL = %q", s.URL())
	}
	if len(s.Token()) < 16 {
		t.Fatalf("token trop court: %q", s.Token())
	}
}
