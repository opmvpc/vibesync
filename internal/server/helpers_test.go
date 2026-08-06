package server

import (
	"io"
	"log/slog"
	"sync"
	"testing"
	"time"

	"github.com/opmvpc/vibesync/internal/protocol"
)

// --- Horloge injectable ---

type fakeClock struct {
	mu  sync.Mutex
	now time.Time
}

func newFakeClock() *fakeClock {
	return &fakeClock{now: time.Date(2026, 8, 5, 20, 0, 0, 0, time.UTC)}
}

func (c *fakeClock) Now() time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.now
}

func (c *fakeClock) Advance(d time.Duration) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.now = c.now.Add(d)
}

// --- Sink de test ---

type recMsg struct {
	Type string
	Data any
}

type recorder struct {
	mu      sync.Mutex
	msgs    []recMsg
	evicted int
}

func (r *recorder) send(msgType string, data any) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.msgs = append(r.msgs, recMsg{Type: msgType, Data: data})
}

// evict enregistre la fermeture demandée par une reprise de session.
func (r *recorder) evict() {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.evicted++
}

// evictions compte les fermetures pour reprise de session.
func (r *recorder) evictions() int {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.evicted
}

func (r *recorder) all() []recMsg {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := make([]recMsg, len(r.msgs))
	copy(out, r.msgs)
	return out
}

func (r *recorder) reset() {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.msgs = nil
}

func (r *recorder) countOf(msgType string) int {
	n := 0
	for _, m := range r.all() {
		if m.Type == msgType {
			n++
		}
	}
	return n
}

func (r *recorder) lastOf(t *testing.T, msgType string) recMsg {
	t.Helper()
	msgs := r.all()
	for i := len(msgs) - 1; i >= 0; i-- {
		if msgs[i].Type == msgType {
			return msgs[i]
		}
	}
	t.Fatalf("aucun message de type %q reçu (reçus: %v)", msgType, typesOf(msgs))
	return recMsg{}
}

func (r *recorder) lastState(t *testing.T) protocol.RoomState {
	t.Helper()
	st, ok := r.lastOf(t, protocol.TypeRoomState).Data.(protocol.RoomState)
	if !ok {
		t.Fatalf("payload roomState inattendu")
	}
	return st
}

func (r *recorder) lastToast(t *testing.T) protocol.Toast {
	t.Helper()
	toast, ok := r.lastOf(t, protocol.TypeToast).Data.(protocol.Toast)
	if !ok {
		t.Fatalf("payload toast inattendu")
	}
	return toast
}

func (r *recorder) lastUsers(t *testing.T) []protocol.User {
	t.Helper()
	msg, ok := r.lastOf(t, protocol.TypeUsers).Data.(protocol.UsersMsg)
	if !ok {
		t.Fatalf("payload users inattendu")
	}
	return msg.Users
}

func typesOf(msgs []recMsg) []string {
	out := make([]string, 0, len(msgs))
	for _, m := range msgs {
		out = append(out, m.Type)
	}
	return out
}

func testLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, &slog.HandlerOptions{Level: slog.LevelDebug}))
}

// testBuildInfo est ce que les salles de test annoncent dans leur welcome.
var testBuildInfo = buildInfo{version: "1.2.3", downloadURL: "https://exemple.test/dl"}

// newTestRoom construit une salle isolée avec horloge injectée.
func newTestRoom() (*Room, *fakeClock) {
	clk := newFakeClock()
	return newRoom("salon", clk, testLogger(), testBuildInfo), clk
}

// joinTest ajoute un membre à la salle et renvoie son enregistreur.
func joinTest(t *testing.T, r *Room, name string) (*member, *recorder) {
	t.Helper()
	m, rec, replaced := joinSession(t, r, name, "")
	if replaced != nil {
		t.Fatalf("join(%q) a évincé une connexion sans jeton de session", name)
	}
	return m, rec
}

// joinSession ajoute un membre avec un jeton de reprise de session et rend
// aussi la connexion éventuellement remplacée.
func joinSession(t *testing.T, r *Room, name, session string) (*member, *recorder, sink) {
	t.Helper()
	rec := &recorder{}
	m, replaced, err := r.join(name, session, 0, rec)
	if err != nil {
		t.Fatalf("join(%q): %v", name, err)
	}
	return m, rec, replaced
}
