package ws

import (
	"bufio"
	"context"
	"crypto/rand"
	"crypto/tls"
	"encoding/base64"
	"errors"
	"fmt"
	"net"
	"net/http"
	"net/textproto"
	"net/url"
	"strings"
	"sync/atomic"
	"time"
)

// defaultHandshakeTimeout borne la connexion TCP/TLS + l'échange HTTP quand ni
// le contexte ni DialConfig ne fixent d'échéance.
const defaultHandshakeTimeout = 15 * time.Second

// DialConfig règle le dialer. Le zéro est utilisable.
type DialConfig struct {
	// TLSConfig sert aux URL wss://. Nil = configuration par défaut (nom de
	// serveur déduit de l'URL, vérification du certificat activée). La
	// configuration est clonée, jamais modifiée.
	TLSConfig *tls.Config
	// Header ajoute des en-têtes à la requête d'upgrade (Authorization,
	// Origin, User-Agent...). Les en-têtes du handshake lui-même
	// (Upgrade, Connection, Sec-WebSocket-*, Host) sont refusées.
	Header http.Header
	// HandshakeTimeout borne la durée totale du handshake.
	// 0 = defaultHandshakeTimeout.
	HandshakeTimeout time.Duration
}

// Dial ouvre une connexion WebSocket vers rawURL (schémas ws:// et wss://).
//
// Volontairement non géré : proxies HTTP(S), redirections (un statut != 101
// est une erreur), cookies, authentification automatique, extensions et
// sous-protocoles.
func Dial(ctx context.Context, rawURL string) (*Conn, error) {
	return DialWithConfig(ctx, rawURL, nil)
}

// DialWithConfig est [Dial] avec réglages ; cfg peut être nil.
func DialWithConfig(ctx context.Context, rawURL string, cfg *DialConfig) (*Conn, error) {
	if cfg == nil {
		cfg = &DialConfig{}
	}
	u, err := url.Parse(rawURL)
	if err != nil {
		return nil, fmt.Errorf("ws: URL invalide: %w", err)
	}
	var secure bool
	switch u.Scheme {
	case "ws":
	case "wss":
		secure = true
	default:
		return nil, fmt.Errorf("ws: schéma %q non supporté (ws ou wss)", u.Scheme)
	}
	if u.Host == "" {
		return nil, errors.New("ws: hôte manquant dans l'URL")
	}
	if err := checkDialHeader(cfg.Header); err != nil {
		return nil, err
	}

	port := u.Port()
	if port == "" {
		if secure {
			port = "443"
		} else {
			port = "80"
		}
	}
	addr := net.JoinHostPort(u.Hostname(), port)

	timeout := cfg.HandshakeTimeout
	if timeout <= 0 {
		timeout = defaultHandshakeTimeout
	}
	deadline := time.Now().Add(timeout)
	if d, ok := ctx.Deadline(); ok && d.Before(deadline) {
		deadline = d
	}
	dialCtx, cancel := context.WithDeadline(ctx, deadline)
	defer cancel()

	var d net.Dialer
	tcp, err := d.DialContext(dialCtx, "tcp", addr)
	if err != nil {
		return nil, fmt.Errorf("ws: connexion à %s: %w", addr, err)
	}
	// Chien de garde : annulation du contexte pendant le handshake. La
	// goroutine meurt à la sortie de la fonction (pas de goroutine résiduelle
	// attachée à la Conn). Le CAS `handed` arbitre l'égalité parfaite entre
	// « le contexte expire » et « le handshake réussit » : le transport est
	// soit fermé par le chien de garde, soit remis à l'appelant, jamais les
	// deux.
	var handed atomic.Bool
	done := make(chan struct{})
	defer close(done)
	go func() {
		select {
		case <-dialCtx.Done():
			if handed.CompareAndSwap(false, true) {
				_ = tcp.Close()
			}
		case <-done:
		}
	}()

	conn := net.Conn(tcp)
	if err := conn.SetDeadline(deadline); err != nil {
		_ = tcp.Close()
		return nil, err
	}
	if secure {
		tcfg := cfg.TLSConfig.Clone()
		if tcfg == nil {
			tcfg = &tls.Config{}
		}
		if tcfg.ServerName == "" {
			tcfg.ServerName = u.Hostname()
		}
		tc := tls.Client(conn, tcfg)
		if err := tc.HandshakeContext(dialCtx); err != nil {
			_ = tcp.Close()
			return nil, fmt.Errorf("ws: handshake TLS: %w", err)
		}
		conn = tc
	}

	var nonce [16]byte
	if _, err := rand.Read(nonce[:]); err != nil {
		_ = tcp.Close()
		return nil, err
	}
	key := base64.StdEncoding.EncodeToString(nonce[:])

	bw := bufio.NewWriterSize(conn, writeBufSize)
	if err := writeUpgradeRequest(bw, u, key, cfg.Header); err != nil {
		_ = tcp.Close()
		return nil, fmt.Errorf("ws: envoi de la requête d'upgrade: %w", err)
	}

	br := bufio.NewReaderSize(conn, readBufSize)
	resp, err := http.ReadResponse(br, &http.Request{Method: http.MethodGet})
	if err != nil {
		_ = tcp.Close()
		return nil, fmt.Errorf("ws: lecture de la réponse d'upgrade: %w", err)
	}
	if err := checkUpgradeResponse(resp, key); err != nil {
		_ = tcp.Close()
		return nil, err
	}
	// Prise de possession : à partir d'ici le chien de garde ne peut plus
	// fermer la connexion rendue à l'appelant.
	if !handed.CompareAndSwap(false, true) {
		_ = tcp.Close()
		if err := ctx.Err(); err != nil {
			return nil, fmt.Errorf("ws: handshake interrompu: %w", err)
		}
		return nil, errors.New("ws: handshake interrompu (échéance dépassée)")
	}
	if err := conn.SetDeadline(time.Time{}); err != nil {
		_ = tcp.Close()
		return nil, err
	}
	return newConn(conn, br, bw, false), nil
}

// reservedHeaders liste (en forme canonique) les en-têtes que le dialer
// construit lui-même et qu'un appelant ne peut donc pas fournir.
var reservedHeaders = map[string]bool{
	"Host":                     true,
	"Upgrade":                  true,
	"Connection":               true,
	"Sec-Websocket-Key":        true,
	"Sec-Websocket-Version":    true,
	"Sec-Websocket-Extensions": true,
	"Sec-Websocket-Protocol":   true,
	"Sec-Websocket-Accept":     true,
}

// checkDialHeader refuse les en-têtes réservées quelle que soit la casse : une
// map http.Header construite à la main n'est pas forcément canonique.
func checkDialHeader(h http.Header) error {
	for name := range h {
		if reservedHeaders[textproto.CanonicalMIMEHeaderKey(name)] {
			return fmt.Errorf("ws: en-tête %s réservée au handshake", name)
		}
	}
	return nil
}

func writeUpgradeRequest(bw *bufio.Writer, u *url.URL, key string, extra http.Header) error {
	_, _ = bw.WriteString("GET ")
	_, _ = bw.WriteString(u.RequestURI())
	_, _ = bw.WriteString(" HTTP/1.1\r\nHost: ")
	_, _ = bw.WriteString(u.Host)
	_, _ = bw.WriteString("\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ")
	_, _ = bw.WriteString(key)
	_, _ = bw.WriteString("\r\n")
	for name, values := range extra {
		for _, v := range values {
			if strings.ContainsAny(name, "\r\n:") || strings.ContainsAny(v, "\r\n") {
				return fmt.Errorf("ws: en-tête %q invalide", name)
			}
			_, _ = bw.WriteString(name)
			_, _ = bw.WriteString(": ")
			_, _ = bw.WriteString(v)
			_, _ = bw.WriteString("\r\n")
		}
	}
	if _, err := bw.WriteString("\r\n"); err != nil {
		return err
	}
	return bw.Flush()
}

func checkUpgradeResponse(resp *http.Response, key string) error {
	if resp.StatusCode != http.StatusSwitchingProtocols {
		return &HandshakeError{Status: resp.StatusCode, Msg: "101 attendu, reçu " + resp.Status}
	}
	if resp.ProtoMajor != 1 || resp.ProtoMinor != 1 {
		return &HandshakeError{Status: resp.StatusCode, Msg: "réponse en " + resp.Proto + ", HTTP/1.1 attendu"}
	}
	if len(resp.Header.Values("Sec-WebSocket-Accept")) != 1 {
		return &HandshakeError{Status: resp.StatusCode, Msg: "Sec-WebSocket-Accept absente ou dupliquée"}
	}
	if !headerContainsToken(resp.Header, "Upgrade", "websocket") {
		return &HandshakeError{Status: resp.StatusCode, Msg: "en-tête Upgrade: websocket absente de la réponse"}
	}
	if !headerContainsToken(resp.Header, "Connection", "upgrade") {
		return &HandshakeError{Status: resp.StatusCode, Msg: "en-tête Connection: Upgrade absente de la réponse"}
	}
	if got := resp.Header.Get("Sec-WebSocket-Accept"); got != acceptKey(key) {
		return &HandshakeError{Status: resp.StatusCode, Msg: "Sec-WebSocket-Accept incorrecte"}
	}
	if v := resp.Header.Get("Sec-WebSocket-Extensions"); v != "" {
		return &HandshakeError{Status: resp.StatusCode, Msg: "extension non négociée imposée: " + v}
	}
	if v := resp.Header.Get("Sec-WebSocket-Protocol"); v != "" {
		return &HandshakeError{Status: resp.StatusCode, Msg: "sous-protocole non négocié imposé: " + v}
	}
	return nil
}
