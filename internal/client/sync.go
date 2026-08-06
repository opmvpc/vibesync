package client

import (
	"context"
	"math"
	"time"

	"github.com/opmvpc/vibesync/internal/protocol"
	"github.com/opmvpc/vibesync/internal/vlc"
)

// tick est une itération complète du moteur : lecture de l'état de VLC,
// détection d'action utilisateur, correction de drift, tâches périodiques.
// Les E/S vers VLC et le serveur se font hors verrou.
func (e *Engine) tick(now time.Time) {
	e.mu.Lock()
	player := e.player
	gen := e.playerGen
	e.mu.Unlock()

	var (
		st  vlc.Status
		err error
	)
	if player != nil {
		ctx, cancel := context.WithTimeout(e.rootCtx, 800*time.Millisecond)
		st, err = player.Status(ctx)
		cancel()
	}

	e.mu.Lock()
	if player != nil && gen == e.playerGen {
		switch {
		case err != nil:
			e.vlcErr = err.Error()
			e.haveStatus = false
			e.expect.valid = false
		default:
			e.vlcErr = ""
			st.At = now
			e.ingestStatusLocked(st, now)
		}
	}
	e.expireUserHoldLocked(now)
	e.rememberSessionLocked(now)
	actions := e.planLocked(now)
	e.periodicLocked(now)
	e.mu.Unlock()

	e.flush()
	if player != nil && len(actions) > 0 {
		e.execActions(player, actions)
	}
	e.publishState()
}

// ingestStatusLocked intègre une observation de VLC.
func (e *Engine) ingestStatusLocked(st vlc.Status, now time.Time) {
	e.buffering = e.bufDetect.Observe(st, now)
	// Pendant la fenêtre de grâce, l'observation ne fait pas autorité : on ne
	// remplace pas l'attendu, sinon une action utilisateur survenue pendant la
	// grâce serait absorbée définitivement. Elle sera revue au poll suivant.
	inGrace := now.Before(e.graceUntil)
	if e.haveStatus && !inGrace {
		e.detectUserActionLocked(st, now)
	}
	e.status = st
	e.haveStatus = true
	// … sauf si l'on n'a encore aucune attente : il faut bien une référence de
	// départ, y compris si la première observation tombe pendant une grâce.
	if !inGrace || !e.expect.valid {
		e.expect = expectation{
			valid:  true,
			paused: st.State != vlc.StatePlaying,
			pos:    st.PositionSec,
			at:     now,
			rate:   st.Rate,
		}
	}
	e.declareFileLocked(st)
}

// declareFileLocked envoie setFile dès qu'un fichier (et sa durée) est connu.
func (e *Engine) declareFileLocked(st vlc.Status) {
	if !st.Loaded() || st.LengthSec <= 0 {
		return
	}
	name := st.FileName
	if name == "" && e.fileInfo != nil {
		name = e.fileInfo.Name
	}
	if e.fileInfo != nil && e.fileInfo.Name == name && math.Abs(e.fileInfo.DurationSec-st.LengthSec) < 0.5 {
		return
	}
	e.fileInfo = &protocol.FileInfo{Name: name, DurationSec: st.LengthSec, SizeBytes: e.fileSize}
	e.queueLocked(protocol.TypeSetFile, protocol.SetFile{
		Name:        name,
		DurationSec: st.LengthSec,
		SizeBytes:   e.fileSize,
	})
}

// detectUserActionLocked compare l'observation à ce que le moteur attendait ;
// tout écart non provoqué par lui est une action de l'utilisateur dans VLC.
func (e *Engine) detectUserActionLocked(st vlc.Status, now time.Time) {
	if !e.expect.valid || e.phase != PhaseConnected {
		return
	}
	if !st.Loaded() || !e.status.Loaded() {
		return // transitions depuis/vers "stopped" : fin de média, pas une action
	}
	nowPaused := st.State != vlc.StatePlaying
	if nowPaused != e.expect.paused {
		act := protocol.ActionPlay
		if nowPaused {
			act = protocol.ActionPause
		}
		e.userActionLocked(act, st.PositionSec, now)
		return
	}
	if math.Abs(st.PositionSec-e.expect.predict(now)) > UserSeekSec {
		e.userActionLocked(protocol.ActionSeek, st.PositionSec, now)
	}
}

// userActionLocked remonte une action utilisateur et arme le hold de 2 s.
func (e *Engine) userActionLocked(act string, pos float64, now time.Time) {
	e.queueLocked(protocol.TypeControl, protocol.Control{
		Action:      act,
		PositionSec: clampPosition(pos, e.durationLocked()),
	})
	e.graceUntil = now.Add(GraceWindow)
	e.userHoldUntil = now.Add(UserHold)
	e.pendingRS = nil
	// Seek ou transition play/pause faite à la main dans VLC : la position se
	// fige le temps que VLC obéisse, ce n'est pas un buffering.
	e.suspendBufferingLocked(now)
}

// suspendBufferingLocked neutralise la détection de buffering pendant
// BufferingSuspend : seek (commandé ou utilisateur) et transitions play/pause
// figent mécaniquement la position (docs/protocol.md §Comportements client,
// Buffering). Un buffering déjà diagnostiqué n'est pas levé pour autant : il ne
// retombera qu'une fois la position réellement repartie.
func (e *Engine) suspendBufferingLocked(now time.Time) {
	e.bufDetect.Suspend(now, BufferingSuspend)
}

// expireUserHoldLocked lève le hold post-action à échéance. Faute d'écho du
// serveur, le dernier roomState reçu pendant le hold (celui d'autrui, qui
// précédait forcément notre control) est appliqué à ce moment-là.
func (e *Engine) expireUserHoldLocked(now time.Time) {
	if e.userHoldUntil.IsZero() || now.Before(e.userHoldUntil) {
		return
	}
	e.userHoldUntil = time.Time{}
	if e.pendingRS == nil {
		return
	}
	rs := *e.pendingRS
	e.pendingRS = nil
	e.log.Debug("hold expiré sans écho, application du roomState mémorisé", "setBy", rs.SetBy)
	e.adoptRoomStateLocked(rs, now)
}

// rememberSessionLocked mémorise où en est la séance de la salle courante. Cette
// mémoire est la seule chose qui autorise une reprise si le serveur revient
// amnésique (docs/protocol.md §Salle vierge) : elle n'est alimentée que tant
// qu'on est connecté avec un état de salle valide, elle se fige donc à la
// dernière position connue quand la connexion tombe.
func (e *Engine) rememberSessionLocked(now time.Time) {
	if e.phase != PhaseConnected || !e.haveState || e.req.Room == "" {
		return
	}
	e.resumeRoom = e.req.Room
	e.resumePos = e.expectedPositionLocked(now)
	e.resumeKnown = true
}

// planLocked décide des corrections à appliquer à VLC.
//
// Conditions de correction (docs/protocol.md) : état connecté, état de salle de
// référence valide, et au moins une mesure d'offset d'horloge. Hors de ça,
// aucune commande n'est envoyée à VLC.
func (e *Engine) planLocked(now time.Time) []action {
	e.correcting = ""
	if e.player == nil || !e.haveStatus || !e.status.Loaded() {
		e.drift = 0
		return nil
	}
	if e.phase != PhaseConnected || !e.haveState {
		e.drift = 0
		e.nudging = false
		return nil
	}
	base := e.roomRateLocked()
	expected := clampPosition(e.expectedPositionLocked(now), e.status.LengthSec)
	drift := e.status.PositionSec - expected
	e.drift = drift
	if !e.haveOffset {
		return nil // pas encore d'horloge serveur fiable
	}
	if now.Before(e.userHoldUntil) {
		return nil // hold post-action : on attend l'écho du serveur
	}
	if now.Before(e.holdUntil) {
		return nil // une correction est déjà en vol
	}

	var acts []action
	rateAction := func(target float64) {
		if math.Abs(e.appliedRat-target) > 1e-3 || math.Abs(e.status.Rate-target) > 1e-3 {
			acts = append(acts, action{kind: actRate, val: target})
		}
	}

	if e.roomState.Paused {
		// En pause : jamais de nudge, seek uniquement au-delà du seuil (le seek
		// HTTP est arrondi à la seconde, il rapproche toujours sous ce seuil).
		e.nudging = false
		if e.status.State == vlc.StatePlaying {
			acts = append(acts, action{kind: actPause})
		}
		rateAction(base)
		if math.Abs(drift) >= PausedSeekSec {
			acts = append(acts, action{kind: actSeek, val: expected})
			e.correcting = "seek"
		}
		return e.armLocked(acts, now)
	}

	if e.status.State == vlc.StatePaused {
		// Départ (ou reprise) de lecture : on cale la position AVANT de lancer
		// VLC (docs/protocol.md §Départ et reprise de lecture). Compter sur le
		// nudge coûterait ~10 s pour un demi-seconde d'écart au démarrage.
		if math.Abs(drift) >= StartAlignSec {
			acts = append(acts, action{kind: actSeek, val: expected})
			e.correcting = "seek"
		}
		acts = append(acts, action{kind: actResume})
		e.nudging = false
		rateAction(base)
		return e.armLocked(acts, now)
	}
	abs := math.Abs(drift)
	switch {
	case abs >= HardSeekSec:
		e.correcting = "seek"
		e.nudging = false
		acts = append(acts, action{kind: actSeek, val: expected})
		rateAction(base)
	case (e.nudging && abs >= NudgeExitSec) || (!e.nudging && abs > DeadZoneSec):
		// Hystérésis : on engage au-delà de 0,1 s, on ne lâche qu'en dessous
		// de 0,03 s — sinon le rate oscillerait autour du seuil.
		e.nudging = true
		e.correcting = "nudge"
		target := base * NudgeFast // en retard : accélérer
		if drift > 0 {
			target = base * NudgeSlow // en avance : ralentir
		}
		rateAction(target)
	default:
		e.nudging = false
		rateAction(base)
	}
	return e.armLocked(acts, now)
}

const (
	actPause  = "pause"
	actResume = "resume"
	actSeek   = "seek"
	actRate   = "rate"
)

// armLocked met à jour ce que le moteur attend de VLC après ces commandes et
// arme les fenêtres anti-boucle.
func (e *Engine) armLocked(acts []action, now time.Time) []action {
	if len(acts) == 0 {
		return nil
	}
	hold := false
	for _, a := range acts {
		switch a.kind {
		case actPause:
			e.expect.pos = e.expect.predict(now)
			e.expect.paused = true
			e.expect.at = now
			e.suspendBufferingLocked(now)
			hold = true
		case actResume:
			e.expect.pos = e.expect.predict(now)
			e.expect.paused = false
			e.expect.at = now
			e.suspendBufferingLocked(now)
			hold = true
		case actSeek:
			e.expect.pos = math.Round(a.val) // VLC arrondit à la seconde
			e.expect.at = now
			e.suspendBufferingLocked(now)
			hold = true
		case actRate:
			e.expect.pos = e.expect.predict(now)
			e.expect.at = now
			e.expect.rate = a.val
			e.appliedRat = a.val
		}
	}
	e.graceUntil = now.Add(GraceWindow)
	if hold {
		e.holdUntil = now.Add(GraceWindow)
	}
	return acts
}

// periodicLocked programme ping (2 s) et report (1 s).
func (e *Engine) periodicLocked(now time.Time) {
	if e.phase != PhaseConnected {
		return
	}
	if now.Sub(e.lastPing) >= pingEvery {
		e.lastPing = now
		e.queueLocked(protocol.TypePing, protocol.Ping{T: now.UnixMilli()})
	}
	if e.player != nil && e.haveStatus && now.Sub(e.lastReport) >= reportEvery {
		e.lastReport = now
		e.queueLocked(protocol.TypeReport, protocol.Report{
			PositionSec: e.status.PositionSec,
			Paused:      e.status.State != vlc.StatePlaying,
			Buffering:   e.buffering,
		})
	}
}

// execActions applique les commandes à VLC (hors verrou).
func (e *Engine) execActions(player vlc.Controller, acts []action) {
	for _, a := range acts {
		ctx, cancel := context.WithTimeout(e.rootCtx, time.Second)
		var err error
		switch a.kind {
		case actPause:
			err = player.Pause(ctx)
		case actResume:
			err = player.Resume(ctx)
		case actSeek:
			err = player.Seek(ctx, a.val)
		case actRate:
			err = player.SetRate(ctx, a.val)
		}
		cancel()
		if err != nil {
			e.log.Warn("commande VLC en échec", "commande", a.kind, "err", err)
			e.mu.Lock()
			e.vlcErr = err.Error()
			e.mu.Unlock()
		}
	}
}
