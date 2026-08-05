package ws

import (
	"bufio"
	"crypto/sha1"
	"encoding/base64"
	"fmt"
	"net/http"
	"strings"
	"time"
)

// secKeyGUID est le GUID de la RFC 6455 §1.3, concaténé à Sec-WebSocket-Key
// avant le SHA-1.
const secKeyGUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

// acceptKey calcule la valeur de l'en-tête Sec-WebSocket-Accept.
// Vecteur de la RFC : "dGhlIHNhbXBsZSBub25jZQ==" → "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=".
func acceptKey(key string) string {
	h := sha1.New()
	h.Write([]byte(key))
	h.Write([]byte(secKeyGUID))
	var sum [sha1.Size]byte
	return base64.StdEncoding.EncodeToString(h.Sum(sum[:0]))
}

// HandshakeError décrit l'échec d'un handshake, côté serveur (statut renvoyé
// au client) comme côté client (statut reçu du serveur).
type HandshakeError struct {
	Status int
	Msg    string
}

func (e *HandshakeError) Error() string {
	return fmt.Sprintf("ws: handshake refusé (%d): %s", e.Status, e.Msg)
}

// headerContainsToken teste la présence d'un token dans une en-tête à liste
// séparée par des virgules (comparaison insensible à la casse).
func headerContainsToken(h http.Header, name, token string) bool {
	for _, v := range h.Values(name) {
		for {
			i := strings.IndexByte(v, ',')
			var part string
			if i < 0 {
				part, v = v, ""
			} else {
				part, v = v[:i], v[i+1:]
			}
			if strings.EqualFold(strings.TrimSpace(part), token) {
				return true
			}
			if i < 0 {
				return false
			}
		}
	}
	return false
}

// validClientKey vérifie que Sec-WebSocket-Key est bien 16 octets en base64.
func validClientKey(key string) bool {
	b, err := base64.StdEncoding.DecodeString(key)
	return err == nil && len(b) == 16
}

// Upgrade valide la requête d'upgrade WebSocket, répond 101 et prend la main
// sur la connexion TCP. En cas de refus, une réponse d'erreur a déjà été
// écrite dans w et l'erreur renvoyée est un [HandshakeError] : le handler n'a
// plus rien à écrire.
//
// Aucune extension ni sous-protocole n'est négocié (pas de permessage-deflate).
// Le contrôle d'origine (en-tête Origin) est laissé à l'appelant : à faire
// avant d'appeler Upgrade.
func Upgrade(w http.ResponseWriter, r *http.Request) (*Conn, error) {
	if r.Method != http.MethodGet {
		return nil, upgradeFail(w, http.StatusMethodNotAllowed, "méthode "+r.Method+" (GET attendu)")
	}
	if !r.ProtoAtLeast(1, 1) {
		return nil, upgradeFail(w, http.StatusBadRequest, "HTTP/1.1 minimum requis")
	}
	if !headerContainsToken(r.Header, "Connection", "upgrade") {
		return nil, upgradeFail(w, http.StatusBadRequest, "en-tête Connection: Upgrade absente")
	}
	if !headerContainsToken(r.Header, "Upgrade", "websocket") {
		return nil, upgradeFail(w, http.StatusBadRequest, "en-tête Upgrade: websocket absente")
	}
	// Ces deux en-têtes doivent apparaître exactement une fois : plusieurs
	// occurrences laissent chaque intermédiaire choisir laquelle compte
	// (désynchronisation de requête).
	if len(r.Header.Values("Sec-WebSocket-Version")) != 1 {
		w.Header().Set("Sec-WebSocket-Version", "13")
		return nil, upgradeFail(w, http.StatusBadRequest, "Sec-WebSocket-Version absente ou dupliquée")
	}
	if v := r.Header.Get("Sec-WebSocket-Version"); v != "13" {
		w.Header().Set("Sec-WebSocket-Version", "13")
		return nil, upgradeFail(w, http.StatusUpgradeRequired, "version WebSocket non supportée ("+v+")")
	}
	if len(r.Header.Values("Sec-WebSocket-Key")) != 1 {
		return nil, upgradeFail(w, http.StatusBadRequest, "Sec-WebSocket-Key absente ou dupliquée")
	}
	key := r.Header.Get("Sec-WebSocket-Key")
	if !validClientKey(key) {
		return nil, upgradeFail(w, http.StatusBadRequest, "Sec-WebSocket-Key malformée")
	}

	hj, ok := w.(http.Hijacker)
	if !ok {
		return nil, upgradeFail(w, http.StatusInternalServerError, "connexion non hijackable (HTTP/2 ?)")
	}
	nc, brw, err := hj.Hijack()
	if err != nil {
		return nil, upgradeFail(w, http.StatusInternalServerError, "hijack impossible: "+err.Error())
	}
	// Le serveur HTTP a pu poser des échéances ; on repart de zéro.
	_ = nc.SetDeadline(time.Time{})

	if brw.Reader.Buffered() > 0 {
		// Le client a envoyé des octets avant d'avoir reçu le 101 : requête
		// pipelinée ou trames anticipées, on refuse.
		_, _ = nc.Write([]byte("HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n"))
		_ = nc.Close()
		return nil, &HandshakeError{Status: http.StatusBadRequest, Msg: "données reçues avant la fin du handshake"}
	}

	var buf [160]byte
	resp := append(buf[:0], "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "...)
	resp = append(resp, acceptKey(key)...)
	resp = append(resp, "\r\n\r\n"...)
	if _, err := nc.Write(resp); err != nil {
		_ = nc.Close()
		return nil, fmt.Errorf("ws: écriture de la réponse 101: %w", err)
	}
	return newConn(nc, brw.Reader, bufio.NewWriterSize(nc, writeBufSize), true), nil
}

func upgradeFail(w http.ResponseWriter, status int, msg string) error {
	http.Error(w, http.StatusText(status), status)
	return &HandshakeError{Status: status, Msg: msg}
}
