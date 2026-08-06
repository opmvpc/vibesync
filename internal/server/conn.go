package server

import (
	"crypto/sha256"
	"crypto/subtle"
	"errors"
	"log/slog"
	"strings"
	"sync"
	"time"
	"unicode"

	"github.com/opmvpc/vibesync/internal/protocol"
	"github.com/opmvpc/vibesync/internal/ws"
)

const (
	// writeWait : délai maximal d'écriture d'une trame.
	writeWait = 10 * time.Second
	// pongWait : deadline de lecture (robustesse, cf. §Erreurs du protocole).
	pongWait = 60 * time.Second
	// pingPeriod : période des pings WebSocket de transport.
	pingPeriod = 30 * time.Second
	// maxMessageSize : taille maximale d'un message entrant.
	maxMessageSize = 64 << 10
	// outboxSize : profondeur de la file d'écriture par client.
	outboxSize = 64
	// closeLinger : délai maximal accordé à une fermeture propre (échange des
	// trames Close) afin que le client reçoive bien le dernier message `error`
	// avant la coupure de la socket.
	closeLinger = 2 * time.Second
)

// wsClient adapte une connexion WebSocket au `sink` attendu par les salles.
// Toutes les écritures passent par une unique goroutine (writePump).
type wsClient struct {
	srv  *Server
	conn *ws.Conn
	// log est immuable après construction : il est utilisé par send() et par
	// la goroutine d'écriture.
	log      *slog.Logger
	outbox   chan []byte
	done     chan struct{}
	readDone chan struct{}
	once     sync.Once

	// rlog, msgs et chats ne sont manipulés que par la goroutine readPump.
	rlog  *slog.Logger
	msgs  *tokenBucket // budget global de messages entrants
	chats *tokenBucket // budget spécifique au chat

	mu         sync.Mutex
	pingSentAt time.Time
	rtts       []time.Duration
	latencyMs  int64
	// closeCode/closeReason : trame Close à émettre (défaut : clôture normale).
	closeCode   uint16
	closeReason string

	// room/member ne sont manipulés que par la goroutine readPump.
	room   *Room
	member *member
}

func newWSClient(s *Server, conn *ws.Conn, remote string) *wsClient {
	log := s.log.With("remote", remote)
	now := s.clock.Now()
	// Les trames émises automatiquement par la boucle de lecture (pong de
	// réponse à un ping du client, écho de close) doivent avoir leur propre
	// échéance d'écriture : sinon elles héritent de celle posée par writePump,
	// expirée entre deux pings de transport.
	conn.AutoWriteTimeout = writeWait
	return &wsClient{
		srv:      s,
		conn:     conn,
		log:      log,
		rlog:     log,
		msgs:     newTokenBucket(msgRateBurst, msgRatePerSec, now),
		chats:    newTokenBucket(chatRateBurst, chatRatePerSec, now),
		outbox:   make(chan []byte, outboxSize),
		done:     make(chan struct{}),
		readDone: make(chan struct{}),
	}
}

// send encode et met en file un message. Non bloquant : si la file déborde, le
// client est trop lent et on ferme la connexion (il se reconnectera).
func (c *wsClient) send(msgType string, data any) {
	raw, err := protocol.Encode(msgType, data)
	if err != nil {
		c.log.Error("encodage impossible", "type", msgType, "err", err)
		return
	}
	select {
	case c.outbox <- raw:
	default:
		c.log.Warn("file d'écriture saturée, fermeture du client", "type", msgType)
		c.close()
	}
}

// close signale l'arrêt ; writePump vide la file puis ferme la connexion.
func (c *wsClient) close() {
	c.once.Do(func() { close(c.done) })
}

// evict ferme cette connexion parce qu'un autre hello, porteur du même jeton de
// session, vient de reprendre le pseudo (§Comportements serveur, point 6).
// Aucun message d'erreur n'est envoyé : ce n'est pas un refus, et un client qui
// le prendrait pour tel se reconnecterait pour rien.
func (c *wsClient) evict() {
	c.mu.Lock()
	c.closeCode, c.closeReason = ws.CloseGoingAway, "session reprise sur une nouvelle connexion"
	c.mu.Unlock()
	c.rlog.Info("connexion remplacée par une reprise de session")
	c.close()
}

// closeFrame donne le code et la raison à émettre dans la trame Close.
func (c *wsClient) closeFrame() (uint16, string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closeCode == 0 {
		return ws.CloseNormalClosure, ""
	}
	return c.closeCode, c.closeReason
}

// --- Boucle d'écriture ---

func (c *wsClient) writePump() {
	ticker := time.NewTicker(pingPeriod)
	defer func() {
		ticker.Stop()
		_ = c.conn.CloseNow()
	}()

	// Un premier ping immédiat donne une estimation de latence dès la connexion
	// (le ping applicatif du protocole est client→serveur et ne permet pas au
	// serveur de mesurer le RTT lui-même).
	if !c.writePing() {
		return
	}
	for {
		select {
		case raw := <-c.outbox:
			if !c.writeRaw(raw) {
				return
			}
		case <-ticker.C:
			if !c.writePing() {
				return
			}
		case <-c.done:
			c.drain()
			code, reason := c.closeFrame()
			_ = c.conn.SetWriteDeadline(time.Now().Add(writeWait))
			_ = c.conn.WriteClose(code, reason)
			// On laisse la boucle de lecture se terminer (échange des trames
			// Close) avant de couper la socket : sinon le dernier message écrit
			// peut être perdu par une fermeture abrupte.
			select {
			case <-c.readDone:
			case <-time.After(closeLinger):
			}
			return
		}
	}
}

func (c *wsClient) drain() {
	for {
		select {
		case raw := <-c.outbox:
			if !c.writeRaw(raw) {
				return
			}
		default:
			return
		}
	}
}

func (c *wsClient) writeRaw(raw []byte) bool {
	_ = c.conn.SetWriteDeadline(time.Now().Add(writeWait))
	if err := c.conn.WriteMessage(ws.TextMessage, raw); err != nil {
		c.log.Debug("écriture websocket échouée", "err", err)
		return false
	}
	return true
}

func (c *wsClient) writePing() bool {
	c.mu.Lock()
	c.pingSentAt = time.Now()
	c.mu.Unlock()
	_ = c.conn.SetWriteDeadline(time.Now().Add(writeWait))
	if err := c.conn.WritePing(nil); err != nil {
		c.log.Debug("ping websocket échoué", "err", err)
		return false
	}
	return true
}

// observePong met à jour la latence estimée à partir du RTT de la trame ping.
func (c *wsClient) observePong(at time.Time) {
	c.mu.Lock()
	sentAt := c.pingSentAt
	if sentAt.IsZero() {
		c.mu.Unlock()
		return
	}
	rtt := at.Sub(sentAt)
	if rtt < 0 {
		rtt = 0
	}
	c.rtts = append(c.rtts, rtt)
	if len(c.rtts) > rttWindow {
		c.rtts = c.rtts[len(c.rtts)-rttWindow:]
	}
	var sum time.Duration
	for _, v := range c.rtts {
		sum += v
	}
	c.latencyMs = (sum / time.Duration(len(c.rtts))).Milliseconds()
	c.mu.Unlock()

	if c.room != nil && c.member != nil {
		c.room.observeRTT(c.member, rtt)
	}
}

func (c *wsClient) currentLatencyMs() int64 {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.latencyMs
}

// --- Boucle de lecture ---

func (c *wsClient) readPump() {
	defer func() {
		if c.room != nil && c.member != nil {
			c.srv.hub.leave(c.room, c.member)
		}
		close(c.readDone)
		c.close()
	}()

	c.conn.SetReadLimit(maxMessageSize)
	_ = c.conn.SetReadDeadline(time.Now().Add(pongWait))
	c.conn.OnPong = func([]byte) {
		_ = c.conn.SetReadDeadline(time.Now().Add(pongWait))
		c.observePong(time.Now())
	}

	for {
		msgType, raw, err := c.conn.ReadMessage()
		if err != nil {
			c.rlog.Debug("lecture websocket terminée", "err", err)
			return
		}
		_ = c.conn.SetReadDeadline(time.Now().Add(pongWait))
		if msgType != ws.TextMessage {
			c.rlog.Debug("message non texte ignoré", "msgType", msgType)
			continue
		}
		if !c.handleMessage(raw) {
			c.lingerRead()
			return
		}
	}
}

// lingerRead consomme ce qui arrive encore jusqu'à la fermeture par le pair (ou
// expiration), pour que la fermeture reste propre après un message `error`.
func (c *wsClient) lingerRead() {
	_ = c.conn.SetReadDeadline(time.Now().Add(closeLinger))
	for {
		if _, _, err := c.conn.ReadMessage(); err != nil {
			return
		}
	}
}

// handleMessage traite un message ; false demande la fermeture de la connexion.
func (c *wsClient) handleMessage(raw []byte) bool {
	// Anti-flood : tout message entrant consomme un jeton du budget global.
	// Budget épuisé = flood → error protocol + fermeture (spec §6).
	now := c.srv.clock.Now()
	if !c.msgs.allow(now) {
		c.rlog.Warn("flood détecté, fermeture de la connexion")
		c.fail(protocol.ErrProtocol, "trop de messages envoyés, connexion fermée")
		return false
	}

	env, err := protocol.Decode(raw)
	if err != nil {
		c.rlog.Debug("enveloppe invalide", "err", err)
		if c.member == nil {
			c.fail(protocol.ErrProtocol, "message JSON invalide")
			return false
		}
		return true
	}

	if c.member == nil {
		if env.Type != protocol.TypeHello {
			c.fail(protocol.ErrProtocol, "hello attendu en premier message")
			return false
		}
		return c.handleHello(env)
	}

	switch env.Type {
	case protocol.TypeHello:
		c.fail(protocol.ErrProtocol, "hello déjà reçu")
		return false
	case protocol.TypePing:
		if msg, ok := decodeOrLog[protocol.Ping](c, env); ok {
			c.room.handlePing(c.member, msg)
		}
	case protocol.TypeSetReady:
		if msg, ok := decodeOrLog[protocol.SetReady](c, env); ok {
			c.room.handleSetReady(c.member, msg)
		}
	case protocol.TypeSetFile:
		if msg, ok := decodeOrLog[protocol.SetFile](c, env); ok {
			c.room.handleSetFile(c.member, msg)
		}
	case protocol.TypeControl:
		if msg, ok := decodeOrLog[protocol.Control](c, env); ok {
			c.room.handleControl(c.member, msg)
		}
	case protocol.TypeReport:
		if msg, ok := decodeOrLog[protocol.Report](c, env); ok {
			c.room.handleReport(c.member, msg)
		}
	case protocol.TypeChat:
		// Le chat a son propre budget, plus serré. Le dépasser ne ferme pas la
		// connexion (un humain peut légitimement enchaîner quelques messages) :
		// le message est rejeté avec un toast, le budget global restant seul
		// juge du flood.
		if !c.chats.allow(now) {
			c.rlog.Debug("chat throttlé")
			c.send(protocol.TypeToast, protocol.Toast{
				Level: protocol.LevelWarn,
				Text:  "Trop de messages de chat d'un coup : celui-ci a été ignoré.",
			})
			return true
		}
		if msg, ok := decodeOrLog[protocol.Chat](c, env); ok {
			c.room.handleChat(c.member, msg)
		}
	default:
		// Forward-compat : tout type inconnu est ignoré (loggé en debug).
		c.rlog.Debug("message inconnu ignoré", "type", env.Type)
	}
	return true
}

func decodeOrLog[T any](c *wsClient, env protocol.Envelope) (T, bool) {
	msg, err := protocol.DecodeData[T](env)
	if err != nil {
		c.rlog.Debug("data invalide, message ignoré", "type", env.Type, "err", err)
		var zero T
		return zero, false
	}
	return msg, true
}

func (c *wsClient) handleHello(env protocol.Envelope) bool {
	hello, err := protocol.DecodeData[protocol.Hello](env)
	if err != nil {
		c.fail(protocol.ErrProtocol, "hello invalide")
		return false
	}
	if hello.Version != protocol.Version {
		c.fail(protocol.ErrVersionMismatch,
			"version de protocole incompatible, mettez à jour le client")
		return false
	}
	if c.srv.hasPassword {
		// Comparaison de condensats de taille fixe : ni la longueur ni le
		// contenu du secret ne fuient par le temps de comparaison.
		got := sha256.Sum256([]byte(hello.Password))
		if subtle.ConstantTimeCompare(c.srv.passwordHash[:], got[:]) != 1 {
			c.fail(protocol.ErrBadPassword, "mot de passe incorrect")
			return false
		}
	}
	name, ok := cleanLabel(hello.Name, maxNameLen)
	if !ok {
		c.fail(protocol.ErrProtocol, "pseudo invalide")
		return false
	}
	roomName, ok := cleanLabel(hello.Room, maxRoomLen)
	if !ok {
		c.fail(protocol.ErrProtocol, "nom de salle invalide")
		return false
	}

	session := strings.TrimSpace(hello.Session)
	if len(session) > maxSessionLen {
		c.fail(protocol.ErrProtocol, "jeton de session invalide")
		return false
	}

	room, m, replaced, err := c.srv.hub.join(roomName, name, session, c.currentLatencyMs(), c)
	if err != nil {
		switch {
		case errors.Is(err, errNameTaken):
			c.fail(protocol.ErrNameTaken, "ce pseudo est déjà utilisé dans la salle")
		case errors.Is(err, errRoomFull):
			// Plafond atteint : refus explicite, le client n'entre pas dans la
			// salle (un simple toast le laisserait dans le vide).
			c.fail(protocol.ErrProtocol, "salle pleine, réessayez plus tard")
		case errors.Is(err, errTooManyRooms):
			c.fail(protocol.ErrProtocol, "trop de salles ouvertes sur ce serveur")
		default:
			c.fail(protocol.ErrProtocol, "impossible de rejoindre la salle")
		}
		return false
	}
	// Reprise de session : la connexion zombie est fermée maintenant, hors des
	// verrous du hub et de la salle.
	if replaced != nil {
		replaced.evict()
	}
	c.room, c.member = room, m
	c.rlog = c.rlog.With("room", roomName, "user", m.id)
	return true
}

// fail envoie un message d'erreur puis demande la fermeture (la file est vidée
// par writePump avant l'envoi de la trame Close).
func (c *wsClient) fail(code, text string) {
	c.send(protocol.TypeError, protocol.ErrorMsg{Code: code, Text: text})
	c.rlog.Debug("connexion refusée", "code", code)
	c.close()
}

// cleanLabel valide un pseudo ou un nom de salle : non vide, longueur bornée,
// sans caractères de contrôle ni de format Unicode (zero-width, marques bidi…)
// qui permettraient d'usurper visuellement le pseudo d'un autre (spec §6).
func cleanLabel(s string, max int) (string, bool) {
	s = strings.TrimSpace(s)
	if s == "" {
		return "", false
	}
	if len([]rune(s)) > max {
		return "", false
	}
	for _, r := range s {
		if unicode.IsControl(r) || unicode.Is(unicode.Cf, r) {
			return "", false
		}
	}
	return s, true
}
