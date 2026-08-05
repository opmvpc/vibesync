package vlctest

import (
	"sync"
	"time"
)

// Clock est une horloge manuelle pour les tests (sûre en concurrence).
type Clock struct {
	mu sync.Mutex
	t  time.Time
}

// NewClock crée une horloge figée à t (zéro = date arbitraire déterministe).
func NewClock(t time.Time) *Clock {
	if t.IsZero() {
		t = time.Date(2026, 8, 5, 20, 0, 0, 0, time.UTC)
	}
	return &Clock{t: t}
}

// Now renvoie l'instant courant simulé.
func (c *Clock) Now() time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.t
}

// Advance fait avancer l'horloge.
func (c *Clock) Advance(d time.Duration) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.t = c.t.Add(d)
}
