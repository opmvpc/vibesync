package server

import "time"

// tokenBucket est un seau à jetons simple, piloté par l'horloge injectée du
// serveur. Il n'est manipulé que depuis la goroutine de lecture d'une
// connexion : aucun verrou n'est nécessaire.
type tokenBucket struct {
	capacity float64 // taille de la rafale
	perSec   float64 // débit de remplissage
	tokens   float64
	last     time.Time
}

func newTokenBucket(capacity, perSec float64, now time.Time) *tokenBucket {
	return &tokenBucket{capacity: capacity, perSec: perSec, tokens: capacity, last: now}
}

// allow consomme un jeton ; false si le budget est épuisé.
func (b *tokenBucket) allow(now time.Time) bool {
	if elapsed := now.Sub(b.last); elapsed > 0 {
		b.tokens += elapsed.Seconds() * b.perSec
		if b.tokens > b.capacity {
			b.tokens = b.capacity
		}
		b.last = now
	}
	if b.tokens < 1 {
		return false
	}
	b.tokens--
	return true
}
