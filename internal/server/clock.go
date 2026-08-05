package server

import "time"

// Clock abstrait la source de temps de la logique de salle afin de rendre les
// tests unitaires déterministes (aucun time.Sleep dans la logique métier).
type Clock interface {
	Now() time.Time
}

// systemClock est l'implémentation de production.
type systemClock struct{}

func (systemClock) Now() time.Time { return time.Now() }

// msOf convertit un instant en millisecondes epoch (unité du protocole).
func msOf(t time.Time) int64 { return t.UnixMilli() }
