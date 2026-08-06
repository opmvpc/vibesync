package vlc_test

import (
	"testing"
	"time"

	"github.com/thibsix/vibesync/internal/vlc"
)

func TestBufferingDetector(t *testing.T) {
	var d vlc.BufferingDetector
	now := time.Date(2026, 8, 5, 20, 0, 0, 0, time.UTC)
	pos := 10.0
	step := 200 * time.Millisecond

	// Lecture normale : jamais de buffering.
	for range 10 {
		now = now.Add(step)
		pos += step.Seconds()
		if d.Observe(vlc.Status{State: vlc.StatePlaying, PositionSec: pos}, now) {
			t.Fatal("buffering détecté en lecture normale")
		}
	}
	// La position se fige : buffering après la fenêtre.
	var detected bool
	for range 6 {
		now = now.Add(step)
		if d.Observe(vlc.Status{State: vlc.StatePlaying, PositionSec: pos}, now) {
			detected = true
		}
	}
	if !detected {
		t.Fatal("buffering non détecté alors que la position est figée")
	}
	// Ça repart : plus de buffering.
	now = now.Add(step)
	pos += step.Seconds()
	if d.Observe(vlc.Status{State: vlc.StatePlaying, PositionSec: pos}, now) {
		t.Fatal("buffering toujours signalé alors que la lecture a repris")
	}
}

// VS-017 : un seek fige la position le temps que VLC cherche ; pendant la
// fenêtre de suspension, cela ne doit jamais passer pour un buffering.
func TestBufferingSuspenduApresSeek(t *testing.T) {
	var d vlc.BufferingDetector
	now := time.Date(2026, 8, 6, 20, 0, 0, 0, time.UTC)
	step := 200 * time.Millisecond
	pos := 500.0

	// Lecture normale, puis rafale de seeks : à chaque seek la position recule
	// et la détection est suspendue pour 2 s.
	for range 5 {
		now = now.Add(step)
		pos += step.Seconds()
		d.Observe(vlc.Status{State: vlc.StatePlaying, PositionSec: pos}, now)
	}
	for range 10 {
		d.Suspend(now, 2*time.Second)
		now = now.Add(step)
		pos -= 5 // l'utilisateur saute en arrière dans VLC
		if d.Observe(vlc.Status{State: vlc.StatePlaying, PositionSec: pos}, now) {
			t.Fatal("une rafale de seeks ne doit pas passer pour un buffering")
		}
	}

	// La suspension est bornée : la position toujours figée 2 s plus tard est
	// bien un buffering.
	var detected bool
	for range 20 {
		now = now.Add(step)
		if d.Observe(vlc.Status{State: vlc.StatePlaying, PositionSec: pos}, now) {
			detected = true
		}
	}
	if !detected {
		t.Fatal("buffering non détecté après l'expiration de la suspension")
	}

	// Un seek de plus ne « guérit » pas le buffering en cours : seule la reprise
	// effective de la lecture le lève.
	d.Suspend(now, 2*time.Second)
	now = now.Add(step)
	if !d.Observe(vlc.Status{State: vlc.StatePlaying, PositionSec: pos}, now) {
		t.Fatal("un seek ne doit pas effacer un buffering déjà diagnostiqué")
	}
	now = now.Add(3 * time.Second)
	pos += 3
	if d.Observe(vlc.Status{State: vlc.StatePlaying, PositionSec: pos}, now) {
		t.Fatal("la lecture est repartie : le buffering doit retomber")
	}
}

// Reset doit aussi oublier le stall en cours, sinon la première observation
// suivante suffirait à déclarer un buffering.
func TestBufferingResetOublieLeStall(t *testing.T) {
	var d vlc.BufferingDetector
	now := time.Date(2026, 8, 6, 20, 0, 0, 0, time.UTC)
	for range 3 {
		now = now.Add(200 * time.Millisecond)
		d.Observe(vlc.Status{State: vlc.StatePlaying, PositionSec: 42}, now)
	}
	d.Reset()
	for range 2 {
		now = now.Add(200 * time.Millisecond)
		if d.Observe(vlc.Status{State: vlc.StatePlaying, PositionSec: 42}, now) {
			t.Fatal("le stall d'avant le Reset ne doit pas compter")
		}
	}
}

func TestBufferingIgnoreLaPause(t *testing.T) {
	var d vlc.BufferingDetector
	now := time.Date(2026, 8, 5, 20, 0, 0, 0, time.UTC)
	for range 10 {
		now = now.Add(300 * time.Millisecond)
		if d.Observe(vlc.Status{State: vlc.StatePaused, PositionSec: 12}, now) {
			t.Fatal("une pause ne doit pas être vue comme du buffering")
		}
	}
}
