package server

import (
	"testing"
	"time"
)

func TestTokenBucketRafalePuisRefus(t *testing.T) {
	clk := newFakeClock()
	b := newTokenBucket(msgRateBurst, msgRatePerSec, clk.Now())

	for i := 0; i < msgRateBurst; i++ {
		if !b.allow(clk.Now()) {
			t.Fatalf("le jeton %d de la rafale doit être accordé", i+1)
		}
	}
	if b.allow(clk.Now()) {
		t.Fatal("le budget doit être épuisé après la rafale")
	}
}

func TestTokenBucketSeRemplitAvecLeTemps(t *testing.T) {
	clk := newFakeClock()
	b := newTokenBucket(chatRateBurst, chatRatePerSec, clk.Now())
	for i := 0; i < chatRateBurst; i++ {
		b.allow(clk.Now())
	}
	if b.allow(clk.Now()) {
		t.Fatal("budget attendu épuisé")
	}

	// 5 jetons/s → 400 ms redonnent 2 jetons.
	clk.Advance(400 * time.Millisecond)
	for i := 1; i <= 2; i++ {
		if !b.allow(clk.Now()) {
			t.Fatalf("jeton %d attendu après 400 ms", i)
		}
	}
	if b.allow(clk.Now()) {
		t.Fatal("pas plus de 2 jetons après 400 ms")
	}

	// Le remplissage est plafonné à la capacité.
	clk.Advance(time.Hour)
	n := 0
	for b.allow(clk.Now()) {
		n++
		if n > chatRateBurst+1 {
			break
		}
	}
	if n != chatRateBurst {
		t.Fatalf("capacité attendue %d jetons, obtenu %d", chatRateBurst, n)
	}
}

func TestTokenBucketDebitSoutenu(t *testing.T) {
	clk := newFakeClock()
	b := newTokenBucket(msgRateBurst, msgRatePerSec, clk.Now())
	// 20 msg/s pendant 10 s : jamais refusé.
	for i := 0; i < 200; i++ {
		clk.Advance(50 * time.Millisecond)
		if !b.allow(clk.Now()) {
			t.Fatalf("un débit de %d msg/s doit passer (message %d)", msgRatePerSec, i)
		}
	}
}
