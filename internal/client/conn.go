package client

import (
	"context"
	"time"

	"github.com/thibsix/vibesync/internal/protocol"
)

// connLoop maintient la connexion au serveur : dial, hello, lecture, puis
// reconnexion avec backoff 1 s → 10 s (docs/protocol.md §Erreurs et robustesse).
func (e *Engine) connLoop(ctx context.Context, req ConnectRequest, gen uint64) {
	backoff := e.cfg.InitialBackoff
	for ctx.Err() == nil && e.genActive(gen) {
		e.setPhase(gen, PhaseConnecting)
		conn, err := e.cfg.Dialer.Dial(ctx, req.URL)
		if err != nil {
			e.setError(gen, "connexion impossible: "+err.Error())
			if !sleepCtx(ctx, backoff) {
				return
			}
			backoff = nextBackoff(backoff, e.cfg.MaxBackoff)
			continue
		}
		fatal, welcomed := e.session(ctx, conn, req, gen)
		if welcomed {
			backoff = e.cfg.InitialBackoff
		}
		if fatal || ctx.Err() != nil {
			e.setPhase(gen, PhaseIdle)
			return
		}
		e.onSessionEnd(gen)
		if !sleepCtx(ctx, backoff) {
			return
		}
		backoff = nextBackoff(backoff, e.cfg.MaxBackoff)
	}
}

// genActive dit si cette tentative de connexion est toujours celle en cours.
func (e *Engine) genActive(gen uint64) bool {
	e.mu.Lock()
	defer e.mu.Unlock()
	return e.connGen == gen
}

func nextBackoff(cur, max time.Duration) time.Duration {
	next := cur * 2
	if next > max {
		next = max
	}
	return next
}

func sleepCtx(ctx context.Context, d time.Duration) bool {
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-ctx.Done():
		return false
	case <-t.C:
		return true
	}
}

// session envoie le hello puis lit les messages jusqu'à la fin de la connexion.
// fatal indique une erreur qui ne sert à rien de réessayer (mauvais mot de
// passe, pseudo pris, version incompatible).
func (e *Engine) session(ctx context.Context, conn Conn, req ConnectRequest, gen uint64) (fatal, welcomed bool) {
	e.mu.Lock()
	if e.connGen != gen {
		e.mu.Unlock()
		_ = conn.Close()
		return true, false
	}
	e.conn = conn
	e.retrying = false
	e.lastPing = time.Time{}
	e.lastReport = time.Time{}
	e.mu.Unlock()

	done := make(chan struct{})
	defer close(done)
	go func() {
		select {
		case <-ctx.Done():
			_ = conn.Close()
		case <-done:
		}
	}()
	defer func() {
		e.mu.Lock()
		if e.conn == conn {
			e.conn = nil
		}
		e.mu.Unlock()
		_ = conn.Close()
	}()

	hello, err := protocol.Encode(protocol.TypeHello, protocol.Hello{
		Version:  protocol.Version,
		Name:     req.Name,
		Room:     req.Room,
		Password: req.Password,
	})
	if err != nil {
		e.setError(gen, err.Error())
		return true, false
	}
	e.writeMu.Lock()
	err = conn.WriteMessage(hello)
	e.writeMu.Unlock()
	if err != nil {
		e.setError(gen, "envoi du hello impossible: "+err.Error())
		return false, false
	}

	for {
		raw, err := conn.ReadMessage()
		if err != nil {
			if ctx.Err() == nil {
				e.setError(gen, "connexion perdue: "+err.Error())
			}
			return false, welcomed
		}
		if !e.genActive(gen) {
			return true, welcomed // session obsolète : plus rien ne doit être appliqué
		}
		msgType, isFatal := e.handleRaw(raw)
		if msgType == protocol.TypeWelcome {
			welcomed = true
		}
		if isFatal {
			return true, welcomed
		}
	}
}

// handleRaw traite un message serveur. Renvoie son type et true si l'erreur
// est définitive.
func (e *Engine) handleRaw(raw []byte) (string, bool) {
	env, err := protocol.Decode(raw)
	if err != nil {
		e.log.Debug("message serveur illisible", "err", err)
		return "", false
	}
	now := e.cfg.Clock.Now()
	var events []Event
	fatal := false

	e.mu.Lock()
	switch env.Type {
	case protocol.TypeWelcome:
		if w, err := protocol.DecodeData[protocol.Welcome](env); err == nil {
			e.selfID = w.SelfID
			e.users = w.Users
			e.phase = PhaseConnected
			e.retrying = false
			e.lastError = ""
			if w.Room != "" {
				e.req.Room = w.Room
			}
			// Le welcome est la référence d'une session neuve : aucun hold ni
			// roomState en attente ne lui survit.
			e.userHoldUntil = time.Time{}
			e.pendingRS = nil
			e.holdUntil = time.Time{}
			e.nudging = false
			e.applyRoomStateLocked(w.State, now)
			e.readyFromUsersLocked()
			// Re-déclarer notre état après un (re)join.
			if e.fileInfo != nil {
				e.queueLocked(protocol.TypeSetFile, protocol.SetFile{
					Name:        e.fileInfo.Name,
					DurationSec: e.fileInfo.DurationSec,
					SizeBytes:   e.fileInfo.SizeBytes,
				})
			}
			e.queueLocked(protocol.TypeSetReady, protocol.SetReady{Ready: e.ready})
			e.queueLocked(protocol.TypePing, protocol.Ping{T: now.UnixMilli()})
			e.lastPing = now
		} else {
			e.log.Debug("welcome illisible", "err", err)
		}
	case protocol.TypePong:
		if p, err := protocol.DecodeData[protocol.Pong](env); err == nil {
			e.applyPongLocked(p, now)
		}
	case protocol.TypeRoomState:
		if rs, err := protocol.DecodeData[protocol.RoomState](env); err == nil {
			e.applyRoomStateLocked(rs, now)
		}
	case protocol.TypeUsers:
		if u, err := protocol.DecodeData[protocol.UsersMsg](env); err == nil {
			e.users = u.Users
			e.readyFromUsersLocked()
		}
	case protocol.TypeToast:
		if t, err := protocol.DecodeData[protocol.Toast](env); err == nil {
			events = append(events, Event{Kind: EventToast, Toast: &t})
		}
	case protocol.TypeChatEvent, "chat":
		if c, err := protocol.DecodeData[protocol.ChatEvent](env); err == nil {
			events = append(events, Event{Kind: EventChat, Chat: &c})
		}
	case protocol.TypeError:
		if m, err := protocol.DecodeData[protocol.ErrorMsg](env); err == nil {
			e.lastError = errorText(m)
			fatal = fatalCode(m.Code)
			lvl := protocol.LevelWarn
			if fatal {
				lvl = protocol.LevelError
				e.phase = PhaseIdle
			}
			events = append(events, Event{Kind: EventToast, Toast: &protocol.Toast{Level: lvl, Text: e.lastError}})
		}
	default:
		e.log.Debug("message serveur inconnu", "type", env.Type)
	}
	e.mu.Unlock()

	for _, ev := range events {
		e.publish(ev)
	}
	e.flush()
	e.publishState()
	return env.Type, fatal
}

func errorText(m protocol.ErrorMsg) string {
	if m.Text != "" {
		return m.Text
	}
	switch m.Code {
	case protocol.ErrVersionMismatch:
		return "version de protocole incompatible avec le serveur"
	case protocol.ErrBadPassword:
		return "mot de passe du serveur incorrect"
	case protocol.ErrNameTaken:
		return "ce pseudo est déjà pris dans la salle"
	default:
		return "erreur serveur: " + m.Code
	}
}

func fatalCode(code string) bool {
	switch code {
	case protocol.ErrVersionMismatch, protocol.ErrBadPassword, protocol.ErrNameTaken:
		return true
	default:
		return false
	}
}

// applyRoomStateLocked traite un roomState entrant en respectant le hold
// post-action (docs/protocol.md §Hold post-action) :
//   - hors hold : application immédiate ;
//   - pendant le hold, écho de notre propre control (setBy = soi) : le hold est
//     levé et l'état appliqué ;
//   - pendant le hold, roomState d'autrui : mémorisé (le dernier gagne), il ne
//     s'appliquera qu'à l'expiration du hold si aucun écho n'arrive.
func (e *Engine) applyRoomStateLocked(rs protocol.RoomState, now time.Time) {
	rs, ok := sanitizeRoomState(rs)
	if !ok {
		e.log.Debug("roomState invalide ignoré", "state", rs)
		return
	}
	if !e.userHoldUntil.IsZero() && now.Before(e.userHoldUntil) {
		if e.selfID != "" && rs.SetBy == e.selfID {
			e.userHoldUntil = time.Time{}
			e.pendingRS = nil
			e.adoptRoomStateLocked(rs, now)
			return
		}
		cp := rs
		e.pendingRS = &cp
		return
	}
	e.adoptRoomStateLocked(rs, now)
}

// adoptRoomStateLocked installe l'état de référence et arme la fenêtre de grâce
// anti-boucle (500 ms).
func (e *Engine) adoptRoomStateLocked(rs protocol.RoomState, now time.Time) {
	e.roomState = rs
	e.haveState = true
	e.graceUntil = now.Add(GraceWindow)
}

// sanitizeRoomState valide un état de salle entrant (§Assainissement).
func sanitizeRoomState(rs protocol.RoomState) (protocol.RoomState, bool) {
	if !isFinite(rs.PositionSec) || rs.PositionSec < 0 {
		return rs, false
	}
	if !isFinite(rs.Rate) || rs.Rate < MinRate || rs.Rate > MaxRate {
		return rs, false
	}
	if !rs.Paused && rs.RefServerMs <= 0 {
		return rs, false
	}
	return rs, true
}

// readyFromUsersLocked resynchronise notre drapeau ready avec la vue serveur.
func (e *Engine) readyFromUsersLocked() {
	for _, u := range e.users {
		if u.ID == e.selfID {
			e.ready = u.Ready
			return
		}
	}
}

func (e *Engine) setPhase(gen uint64, p Phase) {
	e.mu.Lock()
	if e.connGen != gen {
		e.mu.Unlock()
		return
	}
	e.phase = p
	if p != PhaseConnected {
		e.invalidateReferenceLocked()
	}
	e.mu.Unlock()
	e.publishState()
}

func (e *Engine) setError(gen uint64, msg string) {
	e.mu.Lock()
	if e.connGen != gen {
		e.mu.Unlock()
		return
	}
	e.lastError = msg
	e.retrying = true
	e.mu.Unlock()
	e.publishState()
}

// onSessionEnd invalide l'état de référence : pendant la reconnexion, plus
// aucune correction ne doit être appliquée à VLC (§Conditions de correction).
func (e *Engine) onSessionEnd(gen uint64) {
	e.mu.Lock()
	if e.connGen != gen {
		e.mu.Unlock()
		return
	}
	e.retrying = true
	e.phase = PhaseConnecting
	e.invalidateReferenceLocked()
	e.mu.Unlock()
	e.publishState()
}
