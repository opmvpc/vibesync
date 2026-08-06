package vlc_test

import (
	"context"
	"testing"
	"time"

	"github.com/thibsix/vibesync/internal/vlc"
	"github.com/thibsix/vibesync/internal/vlc/vlctest"
)

// Le faux VLC démarre en lecture à l'ouverture, comme le vrai : Prepare doit
// ramener le média en pause au début avant de rendre la main
// (docs/protocol.md §Chargement de fichier).

func TestPrepareArreteLeMediaAuDebut(t *testing.T) {
	fake, clock, c := newFake(t)
	fake.LoadFile("ep1.mkv", 600)
	if fake.State() != "playing" {
		t.Fatalf("le faux VLC devrait démarrer en lecture (autoplay), état = %q", fake.State())
	}
	clock.Advance(3 * time.Second) // VLC a déjà lu 3 s avant qu'on réagisse

	if err := vlc.Prepare(context.Background(), c, 3*time.Second); err != nil {
		t.Fatalf("Prepare: %v", err)
	}
	if got := fake.State(); got != "paused" {
		t.Fatalf("état = %q, attendu paused", got)
	}
	if got := fake.Position(); got >= vlc.StartTolerance {
		t.Fatalf("position = %v, attendu le début du média", got)
	}
}

func TestPrepareAttendLOuvertureDuMedia(t *testing.T) {
	// Horloge réelle : le délai d'ouverture doit réellement s'écouler.
	fake := vlctest.New(time.Now)
	t.Cleanup(fake.Close)
	fake.SetLoadDelay(150 * time.Millisecond)
	fake.LoadFile("ep1.mkv", 600)
	if got := fake.State(); got != "stopped" {
		t.Fatalf("état = %q pendant l'ouverture, attendu stopped", got)
	}

	c := vlc.NewHTTPClient(fake.URL(), fake.Password())
	if err := vlc.Prepare(context.Background(), c, 5*time.Second); err != nil {
		t.Fatalf("Prepare: %v", err)
	}
	if got := fake.State(); got != "paused" {
		t.Fatalf("état = %q, attendu paused", got)
	}
	st, err := c.Status(context.Background())
	if err != nil {
		t.Fatalf("Status: %v", err)
	}
	if !st.Loaded() || st.LengthSec <= 0 {
		t.Fatalf("le média devrait être chargé et sa durée connue: %+v", st)
	}
}

func TestPrepareSansAutoplay(t *testing.T) {
	fake, _, c := newFake(t)
	fake.SetAutoplay(false)
	fake.LoadFile("ep1.mkv", 600)
	if err := vlc.Prepare(context.Background(), c, time.Second); err != nil {
		t.Fatalf("Prepare: %v", err)
	}
	if got := fake.State(); got != "paused" {
		t.Fatalf("état = %q", got)
	}
}

func TestPrepareTimeout(t *testing.T) {
	c := vlc.NewHTTPClient("http://127.0.0.1:1", "x")
	if err := vlc.Prepare(context.Background(), c, 200*time.Millisecond); err == nil {
		t.Fatal("attendu un timeout quand VLC ne répond pas")
	}
}

func TestPrepareContexteAnnule(t *testing.T) {
	fake, _, c := newFake(t)
	fake.LoadFile("ep1.mkv", 600)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if err := vlc.Prepare(ctx, c, 5*time.Second); err == nil {
		t.Fatal("attendu une erreur sur contexte annulé")
	}
}
