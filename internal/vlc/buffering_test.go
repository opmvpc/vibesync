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
