// Package vlc pilote un lecteur VLC via son interface HTTP locale
// (`--extraintf=http`, cf. ADR-003). Le driver concret est caché derrière
// l'interface Controller pour rester remplaçable et testable (faux VLC httptest
// dans internal/vlc/vlctest).
package vlc

import (
	"context"
	"errors"
	"time"
)

// State est l'état de lecture rapporté par VLC.
type State string

const (
	StateStopped State = "stopped"
	StatePlaying State = "playing"
	StatePaused  State = "paused"
)

// Status est un instantané de l'état de VLC.
type Status struct {
	State State
	// PositionSec est la position fine, calculée `position × length` : le champ
	// `time` de status.json n'a qu'une résolution d'une seconde.
	PositionSec float64
	LengthSec   float64
	Rate        float64
	FileName    string
	// At est l'instant d'observation (rempli par l'appelant / le moteur).
	At time.Time
}

// Playing indique si VLC est en cours de lecture.
func (s Status) Playing() bool { return s.State == StatePlaying }

// Loaded indique qu'un média est chargé (lecture ou pause).
func (s Status) Loaded() bool { return s.State == StatePlaying || s.State == StatePaused }

// Controller est l'interface minimale dont le moteur de sync a besoin.
type Controller interface {
	// Status interroge /requests/status.json.
	Status(ctx context.Context) (Status, error)
	// Pause force la pause (pl_forcepause) — idempotent côté VLC.
	Pause(ctx context.Context) error
	// Resume force la reprise (pl_forceresume) — idempotent côté VLC.
	Resume(ctx context.Context) error
	// Seek saute à une position absolue. L'interface HTTP de VLC n'accepte
	// que des secondes entières : l'implémentation arrondit.
	Seek(ctx context.Context, positionSec float64) error
	// SetRate change la vitesse de lecture (1 = normal).
	SetRate(ctx context.Context, rate float64) error
	// Close libère les ressources (et arrête le process si on l'a lancé).
	Close() error
}

// ErrNotFound est renvoyée quand aucun exécutable VLC n'a été localisé.
var ErrNotFound = errors.New("vlc: exécutable introuvable")

// BufferingDetector déduit un état « bufferise » du fait que la position
// n'avance plus alors que VLC se déclare en lecture. C'est une heuristique
// « au mieux » : l'interface HTTP de VLC n'expose pas d'état de buffering.
type BufferingDetector struct {
	// Window est la durée pendant laquelle la position doit stagner avant de
	// déclarer un buffering (défaut 700 ms).
	Window time.Duration
	// MinProgressSec est l'avancée minimale considérée comme « ça avance »
	// rapportée à la durée écoulée (défaut : 25 % du temps écoulé).
	MinProgressRatio float64

	have      bool
	stallFrom time.Time
	lastPos   float64
	lastAt    time.Time
	buffering bool
}

// Reset oublie l'historique (après un seek, un changement de fichier…).
func (b *BufferingDetector) Reset() {
	b.have = false
	b.buffering = false
}

// Observe intègre une observation et renvoie l'état de buffering courant.
func (b *BufferingDetector) Observe(st Status, now time.Time) bool {
	window := b.Window
	if window <= 0 {
		window = 700 * time.Millisecond
	}
	ratio := b.MinProgressRatio
	if ratio <= 0 {
		ratio = 0.25
	}
	if st.State != StatePlaying {
		b.have = true
		b.lastPos, b.lastAt = st.PositionSec, now
		b.stallFrom = time.Time{}
		b.buffering = false
		return false
	}
	if !b.have {
		b.have = true
		b.lastPos, b.lastAt = st.PositionSec, now
		return false
	}
	elapsed := now.Sub(b.lastAt).Seconds()
	if elapsed <= 0 {
		return b.buffering
	}
	progressed := st.PositionSec - b.lastPos
	b.lastPos, b.lastAt = st.PositionSec, now
	if progressed >= elapsed*ratio {
		b.stallFrom = time.Time{}
		b.buffering = false
		return false
	}
	if b.stallFrom.IsZero() {
		b.stallFrom = now
		return b.buffering
	}
	if now.Sub(b.stallFrom) >= window {
		b.buffering = true
	}
	return b.buffering
}
