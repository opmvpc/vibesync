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
	// MinSuspendGap est le délai minimal entre la fin d'une suspension et le
	// début de la suivante (défaut 1 s). Sans ce garde-fou, un moteur qui seeke
	// en boucle pour rattraper un lecteur figé ré-armerait la suspension à
	// chaque correction et le buffering ne serait jamais diagnostiqué
	// (docs/protocol.md §Comportements client, Buffering — anti-masquage).
	MinSuspendGap time.Duration

	have      bool
	stallFrom time.Time
	lastPos   float64
	lastAt    time.Time
	buffering bool
	// suspendUntil : instant jusqu'auquel la détection est neutralisée. Un seek
	// (ou une transition play/pause) fige mécaniquement la position le temps que
	// VLC cherche : sans cette fenêtre, chaque action de l'utilisateur passerait
	// pour un buffering (docs/protocol.md §Comportements client, Buffering).
	suspendUntil time.Time
}

// Reset oublie l'historique (après un seek, un changement de fichier…).
func (b *BufferingDetector) Reset() {
	b.have = false
	b.buffering = false
	// Sans cet oubli, un stall entamé avant le Reset ferait basculer en
	// buffering dès la deuxième observation qui suit.
	b.stallFrom = time.Time{}
}

// Suspend oublie le stall en cours et neutralise la détection jusqu'à now+d.
//
// Anti-masquage : la demande est ignorée tant qu'on n'est pas à MinSuspendGap
// de la fin de la suspension précédente. Une suspension n'est donc jamais ni
// prolongée ni raccourcie, et il reste toujours une fenêtre de vision entre
// deux — sans quoi un moteur qui corrige en boucle (seek toutes les ~2 s vers
// un lecteur figé) resterait aveugle indéfiniment.
//
// Le verdict courant, lui, est conservé : envoyer un seek ne prouve pas que la
// lecture est repartie.
func (b *BufferingDetector) Suspend(now time.Time, d time.Duration) {
	if !b.suspendUntil.IsZero() && now.Before(b.suspendUntil.Add(b.minGap())) {
		return
	}
	b.have = false
	b.stallFrom = time.Time{}
	b.suspendUntil = now.Add(d)
}

func (b *BufferingDetector) minGap() time.Duration {
	if b.MinSuspendGap <= 0 {
		return defaultSuspendGap
	}
	return b.MinSuspendGap
}

// defaultSuspendGap : fenêtre de vision minimale garantie entre deux
// suspensions (docs/protocol.md §Buffering).
const defaultSuspendGap = time.Second

// Suspended dit si la détection est encore neutralisée à cet instant.
func (b *BufferingDetector) Suspended(now time.Time) bool {
	return !b.suspendUntil.IsZero() && now.Before(b.suspendUntil)
}

// Observe intègre une observation et renvoie l'état de buffering courant.
func (b *BufferingDetector) Observe(st Status, now time.Time) bool {
	if b.Suspended(now) {
		// On continue d'ancrer la position pour que la reprise de la détection
		// ne voie pas d'un coup tout le saut accumulé pendant la suspension.
		// Aucun nouveau diagnostic n'est posé, l'ancien n'est pas levé.
		b.have = true
		b.lastPos, b.lastAt = st.PositionSec, now
		b.stallFrom = time.Time{}
		return b.buffering
	}
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
