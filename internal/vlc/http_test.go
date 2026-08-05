package vlc_test

import (
	"context"
	"io"
	"math"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/thibsix/vibesync/internal/vlc"
	"github.com/thibsix/vibesync/internal/vlc/vlctest"
)

func newFake(t *testing.T) (*vlctest.Fake, *vlctest.Clock, *vlc.HTTPClient) {
	t.Helper()
	clock := vlctest.NewClock(time.Time{})
	fake := vlctest.New(clock.Now)
	t.Cleanup(fake.Close)
	return fake, clock, vlc.NewHTTPClient(fake.URL(), fake.Password())
}

func TestStatusPositionFine(t *testing.T) {
	fake, clock, c := newFake(t)
	fake.LoadFile("ep1.mkv", 1200)
	fake.Play()
	clock.Advance(2500 * time.Millisecond)

	st, err := c.Status(context.Background())
	if err != nil {
		t.Fatalf("Status: %v", err)
	}
	if st.State != vlc.StatePlaying {
		t.Fatalf("état = %q, attendu playing", st.State)
	}
	// `time` serait 2 ; position × length doit donner 2,5.
	if math.Abs(st.PositionSec-2.5) > 1e-6 {
		t.Fatalf("position = %v, attendu 2.5", st.PositionSec)
	}
	if st.LengthSec != 1200 {
		t.Fatalf("length = %v", st.LengthSec)
	}
	if st.FileName != "ep1.mkv" {
		t.Fatalf("fichier = %q", st.FileName)
	}
}

func TestCommands(t *testing.T) {
	fake, clock, c := newFake(t)
	fake.LoadFile("ep1.mkv", 600)
	ctx := context.Background()

	if err := c.Resume(ctx); err != nil {
		t.Fatalf("Resume: %v", err)
	}
	if fake.State() != "playing" {
		t.Fatalf("état après resume = %q", fake.State())
	}
	clock.Advance(time.Second)
	if err := c.Pause(ctx); err != nil {
		t.Fatalf("Pause: %v", err)
	}
	if fake.State() != "paused" {
		t.Fatalf("état après pause = %q", fake.State())
	}
	if err := c.Seek(ctx, 42.4); err != nil {
		t.Fatalf("Seek: %v", err)
	}
	// L'API HTTP de VLC n'accepte que des secondes entières.
	if got := fake.Position(); math.Abs(got-42) > 1e-9 {
		t.Fatalf("position après seek = %v, attendu 42", got)
	}
	if err := c.SetRate(ctx, 1.05); err != nil {
		t.Fatalf("SetRate: %v", err)
	}
	if got := fake.Rate(); math.Abs(got-1.05) > 1e-9 {
		t.Fatalf("rate = %v", got)
	}
}

func TestStatusBadPassword(t *testing.T) {
	fake, _, _ := newFake(t)
	c := vlc.NewHTTPClient(fake.URL(), "mauvais")
	if _, err := c.Status(context.Background()); err == nil {
		t.Fatal("mot de passe erroné accepté")
	}
}

func TestStatusServerError(t *testing.T) {
	fake, _, c := newFake(t)
	fake.FailNext(1)
	if _, err := c.Status(context.Background()); err == nil {
		t.Fatal("erreur HTTP 500 ignorée")
	}
	if _, err := c.Status(context.Background()); err != nil {
		t.Fatalf("requête suivante: %v", err)
	}
}

func TestWaitReady(t *testing.T) {
	fake, _, c := newFake(t)
	fake.FailNext(2)
	if err := c.WaitReady(context.Background(), 3*time.Second); err != nil {
		t.Fatalf("WaitReady: %v", err)
	}
}

func TestWaitReadyTimeout(t *testing.T) {
	c := vlc.NewHTTPClient("http://127.0.0.1:1", "x")
	if err := c.WaitReady(context.Background(), 250*time.Millisecond); err == nil {
		t.Fatal("attendu un timeout")
	}
}

func TestSeekNegatifEtRateInvalide(t *testing.T) {
	fake, _, c := newFake(t)
	fake.LoadFile("ep1.mkv", 600)
	fake.SeekTo(100)
	ctx := context.Background()
	if err := c.Seek(ctx, -5); err != nil {
		t.Fatalf("Seek: %v", err)
	}
	if got := fake.Position(); got != 0 {
		t.Fatalf("position = %v, attendu 0", got)
	}
	if err := c.SetRate(ctx, 0); err != nil {
		t.Fatalf("SetRate: %v", err)
	}
	if got := fake.Rate(); got != 1 {
		t.Fatalf("rate = %v, attendu 1", got)
	}
}

// TestStatusAssainissement vérifie que les valeurs aberrantes de VLC sont
// ramenées à des valeurs sûres (docs/protocol.md §Assainissement).
func TestStatusAssainissement(t *testing.T) {
	cases := []struct {
		nom     string
		json    string
		wantPos float64
		wantLen float64
		wantRat float64
	}{
		{"fraction > 1", `{"state":"playing","position":2.5,"length":100,"rate":1}`, 100, 100, 1},
		{"fraction < 0", `{"state":"playing","position":-3,"length":100,"rate":1}`, 0, 100, 1},
		{"longueur négative", `{"state":"playing","position":0.5,"length":-10,"time":7,"rate":1}`, 7, 0, 1},
		{"rate nul", `{"state":"playing","position":0.5,"length":100,"rate":0}`, 50, 100, 1},
		{"rate négatif", `{"state":"paused","position":0.1,"length":100,"rate":-2}`, 10, 100, 1},
		{"champs absents", `{"state":"stopped"}`, 0, 0, 1},
	}
	for _, tc := range cases {
		t.Run(tc.nom, func(t *testing.T) {
			srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
				w.Header().Set("Content-Type", "application/json")
				_, _ = io.WriteString(w, tc.json)
			}))
			defer srv.Close()
			st, err := vlc.NewHTTPClient(srv.URL, "").Status(context.Background())
			if err != nil {
				t.Fatalf("Status: %v", err)
			}
			if math.Abs(st.PositionSec-tc.wantPos) > 1e-9 {
				t.Errorf("position = %v, attendu %v", st.PositionSec, tc.wantPos)
			}
			if math.Abs(st.LengthSec-tc.wantLen) > 1e-9 {
				t.Errorf("length = %v, attendu %v", st.LengthSec, tc.wantLen)
			}
			if math.Abs(st.Rate-tc.wantRat) > 1e-9 {
				t.Errorf("rate = %v, attendu %v", st.Rate, tc.wantRat)
			}
		})
	}
}
