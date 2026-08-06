package vlc

import (
	"context"
	"fmt"
	"time"
)

const (
	// DefaultPrepareTimeout borne la mise en pause initiale du média.
	DefaultPrepareTimeout = 15 * time.Second
	// preparePoll est le pas de scrutation pendant la préparation.
	preparePoll = 20 * time.Millisecond
	// StartTolerance : position en deçà de laquelle le média est considéré
	// « au début ». Le seek HTTP étant arrondi à la seconde, viser mieux que
	// la demi-seconde n'aurait pas de sens.
	StartTolerance = 0.5
)

// Prepare met un média fraîchement ouvert en pause à la position 0, et ne rend
// la main qu'une fois cet état **observé** (docs/protocol.md §Chargement de
// fichier).
//
// C'est indispensable parce que VLC démarre la lecture automatiquement à
// l'ouverture d'un fichier : sans cette étape, deux clients qui ouvrent leur
// média à quelques centaines de millisecondes d'écart démarrent déjà désynchro,
// et le rattrapage au rate (5 %/s) mettrait une dizaine de secondes.
//
// La boucle est volontairement idempotente : on redemande pause et seek 0 tant
// que l'état visé n'est pas constaté, ce qui absorbe aussi bien le délai
// d'ouverture du média que les commandes perdues.
func Prepare(ctx context.Context, c Controller, timeout time.Duration) error {
	if timeout <= 0 {
		timeout = DefaultPrepareTimeout
	}
	deadline := time.Now().Add(timeout)
	var last error
	for {
		st, err := c.Status(ctx)
		switch {
		case err != nil:
			last = err
		case !st.Loaded():
			// Le média n'est pas encore ouvert : rien à commander pour l'instant.
			last = fmt.Errorf("vlc: aucun média chargé (état %q)", st.State)
		default:
			atStart := st.PositionSec < StartTolerance
			if st.State == StatePaused && atStart {
				return nil
			}
			last = fmt.Errorf("vlc: média %s à %.2f s", st.State, st.PositionSec)
			if st.State == StatePlaying {
				if err := c.Pause(ctx); err != nil {
					last = err
				}
			}
			if !atStart {
				if err := c.Seek(ctx, 0); err != nil {
					last = err
				}
			}
		}
		if ctx.Err() != nil {
			return ctx.Err()
		}
		if time.Now().After(deadline) {
			return fmt.Errorf("vlc: média non arrêté au début après %s: %w", timeout, last)
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(preparePoll):
		}
	}
}
