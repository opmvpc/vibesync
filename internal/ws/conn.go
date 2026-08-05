package ws

import (
	"bufio"
	"crypto/rand"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"sync"
	"time"
	"unicode/utf8"
)

const (
	readBufSize  = 4096
	writeBufSize = 4096
	// maskChunk : taille du tampon de masquage réutilisé côté client. Le
	// masquage ne doit pas modifier le buffer de l'appelant, on copie donc par
	// tranches de cette taille.
	maskChunk = 4096
	// closeDrainTimeout : délai max d'attente de l'écho de close du pair dans
	// [Conn.Close].
	closeDrainTimeout = 5 * time.Second
	// defaultReadLimit : plafond par défaut d'un message reçu. Une connexion
	// venant d'Internet ne doit jamais pouvoir faire grossir la mémoire du
	// processus sans borne ; [Conn.SetReadLimit] permet de l'ajuster.
	defaultReadLimit = 1 << 20
)

// Conn est une connexion WebSocket établie, côté serveur ([Upgrade]) ou côté
// client ([Dial]). Voir la doc du package pour le modèle de concurrence.
type Conn struct {
	nc net.Conn
	br *bufio.Reader
	bw *bufio.Writer

	// isServer : un serveur reçoit des trames masquées et en émet des non
	// masquées ; un client fait l'inverse (RFC 6455 §5.1).
	isServer bool

	// OnPong, s'il est non nil, est appelé depuis [Conn.ReadMessage] à chaque
	// pong reçu. Le slice n'est valide que pendant l'appel (buffer réutilisé).
	// À positionner avant la première lecture.
	OnPong func(payload []byte)

	// --- écriture (protégée par wmu) ---
	wmu       sync.Mutex
	whdr      [14]byte // en-tête de trame réutilisé
	wkey      [4]byte  // clé de masquage réutilisée
	wmask     []byte   // tampon de masquage réutilisé (client uniquement)
	sentClose bool
	writeErr  error

	// --- lecture (goroutine lectrice uniquement) ---
	readLimit int64
	rhdr      [8]byte
	ctrl      [maxControlPayload]byte
	gotClose  bool
	readErr   error

	// --- transport (n'importe quelle goroutine) ---
	closeOnce sync.Once
	closeErr  error
}

func newConn(nc net.Conn, br *bufio.Reader, bw *bufio.Writer, isServer bool) *Conn {
	if br == nil {
		br = bufio.NewReaderSize(nc, readBufSize)
	}
	if bw == nil {
		bw = bufio.NewWriterSize(nc, writeBufSize)
	}
	c := &Conn{nc: nc, br: br, bw: bw, isServer: isServer, readLimit: defaultReadLimit}
	if !isServer {
		// Seul un client masque : inutile de payer ce tampon côté serveur.
		c.wmask = make([]byte, maskChunk)
	}
	return c
}

// NetConn expose la connexion réseau sous-jacente (diagnostic, TLS state...).
// Ne pas y lire/écrire directement.
func (c *Conn) NetConn() net.Conn { return c.nc }

// LocalAddr renvoie l'adresse locale.
func (c *Conn) LocalAddr() net.Addr { return c.nc.LocalAddr() }

// RemoteAddr renvoie l'adresse du pair.
func (c *Conn) RemoteAddr() net.Addr { return c.nc.RemoteAddr() }

// SetReadDeadline fixe l'échéance des lectures (voir net.Conn).
func (c *Conn) SetReadDeadline(t time.Time) error { return c.nc.SetReadDeadline(t) }

// SetWriteDeadline fixe l'échéance des écritures (voir net.Conn).
func (c *Conn) SetWriteDeadline(t time.Time) error { return c.nc.SetWriteDeadline(t) }

// SetReadLimit fixe la taille maximale (en octets) d'un message reçu, une fois
// les fragments réassemblés. Au-delà, [Conn.ReadMessage] renvoie un
// [ProtocolError] de code [CloseMessageTooBig] après avoir envoyé la close
// correspondante. La limite est vérifiée à chaque trame, avant lecture : un
// message fragmenté ne peut pas la dépasser en s'accumulant.
//
// La valeur par défaut est de 1 Mio. n <= 0 lève toute limite : à ne faire que
// face à un pair de confiance. Même sans limite, la lecture reste
// incrémentale : une longueur annoncée mensongère n'alloue que ce qui arrive
// réellement sur le fil.
func (c *Conn) SetReadLimit(n int64) { c.readLimit = n }

// ---------------------------------------------------------------- écriture

// WriteMessage envoie un message complet (non fragmenté) de type
// [TextMessage] ou [BinaryMessage]. Sûr depuis plusieurs goroutines.
func (c *Conn) WriteMessage(msgType int, data []byte) error {
	switch msgType {
	case TextMessage:
		if !utf8.Valid(data) {
			return errors.New("ws: message texte invalide (UTF-8)")
		}
	case BinaryMessage:
	default:
		return fmt.Errorf("ws: type de message invalide (%d)", msgType)
	}
	c.wmu.Lock()
	defer c.wmu.Unlock()
	if c.sentClose {
		return ErrClosed
	}
	return c.writeFrameLocked(byte(msgType), true, data)
}

// WriteFragments envoie un message découpé en trames de continuation : une
// trame par fragment, la dernière portant le bit FIN. Un appel sans fragment
// envoie un message vide.
func (c *Conn) WriteFragments(msgType int, frags ...[]byte) error {
	switch msgType {
	case TextMessage:
		var acc utf8Acc
		for _, f := range frags {
			if !acc.write(f) {
				return errors.New("ws: message texte invalide (UTF-8)")
			}
		}
		if !acc.complete() {
			return errors.New("ws: message texte invalide (UTF-8)")
		}
	case BinaryMessage:
	default:
		return fmt.Errorf("ws: type de message invalide (%d)", msgType)
	}
	if len(frags) == 0 {
		return c.WriteMessage(msgType, nil)
	}
	c.wmu.Lock()
	defer c.wmu.Unlock()
	if c.sentClose {
		return ErrClosed
	}
	op := byte(msgType)
	for i, f := range frags {
		if err := c.writeFrameLocked(op, i == len(frags)-1, f); err != nil {
			return err
		}
		op = opContinuation
	}
	return nil
}

// WritePing envoie une trame ping (payload <= 125 octets).
func (c *Conn) WritePing(payload []byte) error { return c.writeControl(opPing, payload) }

// WritePong envoie une trame pong non sollicitée. Les pings reçus reçoivent
// déjà une réponse automatique pendant [Conn.ReadMessage].
func (c *Conn) WritePong(payload []byte) error { return c.writeControl(opPong, payload) }

func (c *Conn) writeControl(opcode byte, payload []byte) error {
	if len(payload) > maxControlPayload {
		return fmt.Errorf("ws: trame de contrôle de %d octets (max %d)", len(payload), maxControlPayload)
	}
	c.wmu.Lock()
	defer c.wmu.Unlock()
	if c.sentClose {
		return ErrClosed
	}
	return c.writeFrameLocked(opcode, true, payload)
}

// WriteClose envoie la trame de fermeture sans attendre l'écho du pair et sans
// fermer la socket. Sûr depuis plusieurs goroutines, y compris pendant qu'une
// autre goroutine lit. Un code nul envoie une trame close sans corps. Les
// appels suivants sont des no-op.
func (c *Conn) WriteClose(code uint16, reason string) error {
	if code != 0 && !validCloseCode(code, c.isServer) {
		return fmt.Errorf("ws: code de fermeture invalide (%d)", code)
	}
	if !utf8.ValidString(reason) {
		return errors.New("ws: raison de close invalide (UTF-8)")
	}
	c.wmu.Lock()
	defer c.wmu.Unlock()
	if c.sentClose {
		return nil
	}
	c.sentClose = true
	var buf [maxControlPayload]byte
	n := 0
	if code != 0 {
		binary.BigEndian.PutUint16(buf[:2], code)
		n = 2 + copy(buf[2:], truncateUTF8(reason, maxControlPayload-2))
	}
	return c.writeFrameLocked(opClose, true, buf[:n])
}

// writeFrameLocked sérialise une trame. wmu doit être tenu.
func (c *Conn) writeFrameLocked(opcode byte, fin bool, payload []byte) error {
	if c.writeErr != nil {
		return c.writeErr
	}
	c.whdr[0] = opcode
	if fin {
		c.whdr[0] |= 0x80
	}
	var maskBit byte
	if !c.isServer {
		maskBit = 0x80
	}
	n := 0
	switch l := len(payload); {
	case l < 126:
		c.whdr[1] = maskBit | byte(l)
		n = 2
	case l <= 0xFFFF:
		c.whdr[1] = maskBit | 126
		binary.BigEndian.PutUint16(c.whdr[2:4], uint16(l))
		n = 4
	default:
		c.whdr[1] = maskBit | 127
		binary.BigEndian.PutUint64(c.whdr[2:10], uint64(l))
		n = 10
	}
	if maskBit != 0 {
		if _, err := rand.Read(c.wkey[:]); err != nil {
			return c.failWrite(err)
		}
		n += copy(c.whdr[n:], c.wkey[:])
	}
	if _, err := c.bw.Write(c.whdr[:n]); err != nil {
		return c.failWrite(err)
	}
	if maskBit == 0 {
		if len(payload) > 0 {
			if _, err := c.bw.Write(payload); err != nil {
				return c.failWrite(err)
			}
		}
	} else {
		pos := 0
		for len(payload) > 0 {
			m := len(payload)
			if m > len(c.wmask) {
				m = len(c.wmask)
			}
			copy(c.wmask[:m], payload[:m])
			pos = maskBytes(c.wkey, pos, c.wmask[:m])
			if _, err := c.bw.Write(c.wmask[:m]); err != nil {
				return c.failWrite(err)
			}
			payload = payload[m:]
		}
	}
	if err := c.bw.Flush(); err != nil {
		return c.failWrite(err)
	}
	return nil
}

// failWrite rend l'erreur d'écriture collante : le flux est désynchronisé, on
// n'écrira plus rien de valide dessus.
func (c *Conn) failWrite(err error) error {
	if c.writeErr == nil {
		c.writeErr = err
	}
	return c.writeErr
}

// ---------------------------------------------------------------- lecture

// ReadMessage lit le prochain message applicatif, en réassemblant les trames
// de continuation. Les trames de contrôle intercalées sont traitées de façon
// transparente : réponse automatique aux pings, appel de [Conn.OnPong] sur les
// pongs, écho + fermeture du transport + erreur [CloseError] sur les close.
//
// Le slice renvoyé appartient à l'appelant (une allocation par message). Sur
// erreur, la connexion n'est plus utilisable en lecture : toutes les lectures
// suivantes renvoient la même erreur.
func (c *Conn) ReadMessage() (msgType int, data []byte, err error) {
	if c.readErr != nil {
		return 0, nil, c.readErr
	}
	msgType, data, err = c.readMessage()
	if err != nil {
		c.readErr = err
		var pe *ProtocolError
		if errors.As(err, &pe) {
			_ = c.WriteClose(pe.Code, pe.Msg)
		}
		return 0, nil, err
	}
	return msgType, data, nil
}

func (c *Conn) readMessage() (int, []byte, error) {
	var msgOp byte
	var data []byte
	for {
		h, err := c.readFrameHeader()
		if err != nil {
			return 0, nil, err
		}
		if h.opcode >= opClose {
			if err := c.handleControl(h); err != nil {
				return 0, nil, err
			}
			continue
		}
		switch h.opcode {
		case opText, opBinary:
			if msgOp != 0 {
				return 0, nil, protoErr(CloseProtocolError, "trame de données au milieu d'un message fragmenté")
			}
			msgOp = h.opcode
		case opContinuation:
			if msgOp == 0 {
				return 0, nil, protoErr(CloseProtocolError, "continuation sans message en cours")
			}
		default:
			return 0, nil, protoErr(CloseProtocolError, fmt.Sprintf("opcode de données réservé 0x%X", h.opcode))
		}
		// Comparaison sans addition : int64(len(data))+h.length déborderait
		// pour une continuation annonçant une longueur proche de MaxInt64,
		// et la somme négative contournerait la limite.
		if c.readLimit > 0 {
			got := int64(len(data))
			if got > c.readLimit || h.length > c.readLimit-got {
				return 0, nil, protoErr(CloseMessageTooBig, "message trop grand")
			}
		}
		if data, err = c.readPayload(data, h); err != nil {
			return 0, nil, err
		}
		if h.fin {
			if msgOp == opText && !utf8.Valid(data) {
				return 0, nil, protoErr(CloseInvalidFramePayloadData, "message texte invalide (UTF-8)")
			}
			if data == nil {
				data = []byte{}
			}
			return int(msgOp), data, nil
		}
	}
}

type frameHeader struct {
	fin    bool
	masked bool
	opcode byte
	key    [4]byte
	length int64
}

func (c *Conn) readFrameHeader() (frameHeader, error) {
	var h frameHeader
	if _, err := io.ReadFull(c.br, c.rhdr[:2]); err != nil {
		return h, err
	}
	if c.rhdr[0]&0x70 != 0 {
		return h, protoErr(CloseProtocolError, "bits RSV non nuls (aucune extension négociée)")
	}
	h.fin = c.rhdr[0]&0x80 != 0
	h.opcode = c.rhdr[0] & 0x0F
	h.masked = c.rhdr[1]&0x80 != 0
	// RFC 6455 §5.2 : la longueur doit être codée sur le plus petit format
	// possible. Accepter les formats étendus non minimaux ouvre la porte aux
	// désaccords d'interprétation entre implémentations.
	switch n := c.rhdr[1] & 0x7F; n {
	case 126:
		if _, err := io.ReadFull(c.br, c.rhdr[:2]); err != nil {
			return h, err
		}
		h.length = int64(binary.BigEndian.Uint16(c.rhdr[:2]))
		if h.length < 126 {
			return h, protoErr(CloseProtocolError, "longueur 16 bits non minimale")
		}
	case 127:
		if _, err := io.ReadFull(c.br, c.rhdr[:8]); err != nil {
			return h, err
		}
		u := binary.BigEndian.Uint64(c.rhdr[:8])
		if u > 1<<63-1 {
			return h, protoErr(CloseProtocolError, "longueur de trame 64 bits invalide")
		}
		h.length = int64(u)
		if h.length <= 0xFFFF {
			return h, protoErr(CloseProtocolError, "longueur 64 bits non minimale")
		}
	default:
		h.length = int64(n)
	}
	if h.masked {
		// Lecture via rhdr (champ de la Conn, déjà sur le tas) : passer
		// h.key[:] à io.ReadFull ferait fuir h vers le tas à chaque trame.
		if _, err := io.ReadFull(c.br, c.rhdr[:4]); err != nil {
			return h, err
		}
		h.key = [4]byte(c.rhdr[:4])
	}
	if c.isServer && !h.masked {
		return h, protoErr(CloseProtocolError, "trame client non masquée")
	}
	if !c.isServer && h.masked {
		return h, protoErr(CloseProtocolError, "trame serveur masquée")
	}
	if h.opcode >= opClose {
		if !h.fin {
			return h, protoErr(CloseProtocolError, "trame de contrôle fragmentée")
		}
		if h.length > maxControlPayload {
			return h, protoErr(CloseProtocolError, "trame de contrôle de plus de 125 octets")
		}
	}
	return h, nil
}

// readPayload lit le corps de la trame et le démasque. dst n'est agrandi qu'à
// hauteur des octets **déjà arrivés** dans le tampon de lecture : une longueur
// annoncée mensongère (2^40 octets suivis de trois octets) n'alloue jamais
// plus que ce que le pair a réellement envoyé.
func (c *Conn) readPayload(dst []byte, h frameHeader) ([]byte, error) {
	pos, remaining := 0, h.length
	for remaining > 0 {
		avail := int64(c.br.Buffered())
		if avail == 0 {
			// Bloque jusqu'à l'arrivée d'au moins un octet, sans rien allouer.
			if _, err := c.br.Peek(1); err != nil {
				if err == io.EOF {
					err = io.ErrUnexpectedEOF
				}
				return dst, err
			}
			avail = int64(c.br.Buffered())
		}
		if avail > remaining {
			avail = remaining
		}
		start := len(dst)
		dst = growSlice(dst, int(avail))
		if _, err := io.ReadFull(c.br, dst[start:]); err != nil {
			return dst, err
		}
		if h.masked {
			pos = maskBytes(h.key, pos, dst[start:])
		}
		remaining -= avail
	}
	return dst, nil
}

func (c *Conn) handleControl(h frameHeader) error {
	buf := c.ctrl[:h.length]
	if h.length > 0 {
		if _, err := io.ReadFull(c.br, buf); err != nil {
			return err
		}
		if h.masked {
			maskBytes(h.key, 0, buf)
		}
	}
	switch h.opcode {
	case opPing:
		// Après notre propre close, on ne répond plus : on attend juste
		// l'écho du pair, sans casser la lecture pour autant.
		if err := c.WritePong(buf); err != nil && !errors.Is(err, ErrClosed) {
			return err
		}
		return nil
	case opPong:
		if c.OnPong != nil {
			c.OnPong(buf)
		}
		return nil
	case opClose:
		return c.handleClose(buf)
	default:
		return protoErr(CloseProtocolError, fmt.Sprintf("opcode de contrôle réservé 0x%X", h.opcode))
	}
}

func (c *Conn) handleClose(payload []byte) error {
	c.gotClose = true
	code, reason := CloseNoStatusReceived, ""
	switch len(payload) {
	case 0:
	case 1:
		return protoErr(CloseProtocolError, "corps de close de 1 octet")
	default:
		code = binary.BigEndian.Uint16(payload)
		// Si nous sommes client, le code vient d'un serveur.
		if !validCloseCode(code, !c.isServer) {
			return protoErr(CloseProtocolError, "code de fermeture invalide")
		}
		reason = string(payload[2:])
		if !utf8.ValidString(reason) {
			return protoErr(CloseInvalidFramePayloadData, "raison de close invalide (UTF-8)")
		}
	}
	echo := code
	if echo == CloseNoStatusReceived {
		echo = CloseNormalClosure
	}
	_ = c.WriteClose(echo, "")
	// Close handshake terminé (reçu + écho envoyé) : plus rien de valide ne
	// peut transiter, on rend la socket au système sans attendre.
	_ = c.closeTransport()
	return &CloseError{Code: code, Reason: reason}
}

// ---------------------------------------------------------------- fermeture

// Close effectue le close handshake complet : envoi de la trame de fermeture,
// attente de l'écho du pair (au plus 5 s, en jetant les messages restants),
// puis fermeture de la socket. À appeler depuis la goroutine lectrice, ou
// quand aucune lecture n'est en cours (voir la doc du package).
//
// Si le pair a déjà initié la fermeture, [Conn.ReadMessage] a fait le travail
// (écho + fermeture du transport) : Close est alors un no-op qui renvoie nil.
func (c *Conn) Close(code uint16, reason string) error {
	werr := c.WriteClose(code, reason)
	if werr == nil && !c.gotClose {
		_ = c.nc.SetReadDeadline(time.Now().Add(closeDrainTimeout))
		for !c.gotClose {
			if _, _, err := c.readMessage(); err != nil {
				break
			}
		}
	}
	if c.readErr == nil {
		c.readErr = ErrClosed
	}
	cerr := c.closeTransport()
	if werr != nil {
		return werr
	}
	return cerr
}

// CloseNow ferme brutalement la socket sans close handshake. Sûr depuis
// n'importe quelle goroutine : c'est le moyen de débloquer un lecteur.
func (c *Conn) CloseNow() error { return c.closeTransport() }

// closeTransport ferme la socket au plus une fois et mémorise le résultat :
// [Conn.Close], [Conn.CloseNow] et la fin du close handshake peuvent tous y
// mener, sans qu'un « use of closed network connection » ne remonte à
// l'appelant.
func (c *Conn) closeTransport() error {
	c.closeOnce.Do(func() { c.closeErr = c.nc.Close() })
	return c.closeErr
}

// ---------------------------------------------------------------- utilitaires

// maskBytes applique le masque XOR de la RFC 6455 §5.3 à b, sachant que b
// commence à l'offset pos dans le corps de la trame. Renvoie le nouvel offset.
func maskBytes(key [4]byte, pos int, b []byte) int {
	for pos&3 != 0 && len(b) > 0 {
		b[0] ^= key[pos&3]
		pos++
		b = b[1:]
	}
	if len(b) >= 8 {
		k := uint64(binary.LittleEndian.Uint32(key[:]))
		k |= k << 32
		for len(b) >= 8 {
			binary.LittleEndian.PutUint64(b, binary.LittleEndian.Uint64(b)^k)
			b = b[8:]
			pos += 8
		}
	}
	for i := range b {
		b[i] ^= key[pos&3]
		pos++
	}
	return pos
}

// growSlice rallonge b de n octets utilisables, en réutilisant sa capacité.
func growSlice(b []byte, n int) []byte {
	if cap(b)-len(b) >= n {
		return b[:len(b)+n]
	}
	need := len(b) + n
	capacity := cap(b) * 2
	if capacity < need {
		capacity = need
	}
	nb := make([]byte, need, capacity)
	copy(nb, b)
	return nb
}

// truncateUTF8 coupe s à max octets sans casser de rune.
func truncateUTF8(s string, max int) string {
	if len(s) <= max {
		return s
	}
	for max > 0 && !utf8.RuneStart(s[max]) {
		max--
	}
	return s[:max]
}

// utf8Acc valide un flux UTF-8 morceau par morceau : une rune peut être coupée
// entre deux fragments d'un même message.
type utf8Acc struct {
	carry [4]byte
	n     int
}

// write valide le fragment p ; renvoie false dès qu'une séquence est invalide.
func (a *utf8Acc) write(p []byte) bool {
	if a.n > 0 && len(p) > 0 {
		k := copy(a.carry[a.n:], p)
		buf := a.carry[:a.n+k]
		if !utf8.FullRune(buf) {
			a.n += k
			return true
		}
		r, size := utf8.DecodeRune(buf)
		if r == utf8.RuneError && size <= 1 {
			return false
		}
		p = p[size-a.n:]
		a.n = 0
	}
	for len(p) > 0 {
		if p[0] < utf8.RuneSelf {
			p = p[1:]
			continue
		}
		if !utf8.FullRune(p) {
			a.n = copy(a.carry[:], p)
			return true
		}
		r, size := utf8.DecodeRune(p)
		if r == utf8.RuneError && size <= 1 {
			return false
		}
		p = p[size:]
	}
	return true
}

// complete indique qu'aucune rune n'est restée incomplète en fin de flux.
func (a *utf8Acc) complete() bool { return a.n == 0 }
