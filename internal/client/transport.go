package client

import (
	"context"
	"errors"
	"fmt"
	"sync"
	"time"

	"github.com/opmvpc/vibesync/internal/ws"
)

// Conn est une connexion message-orientée vers le serveur vibesync.
// Abstraite pour rendre le moteur testable sans réseau.
type Conn interface {
	ReadMessage() ([]byte, error)
	WriteMessage(data []byte) error
	Close() error
}

// Dialer ouvre une Conn.
type Dialer interface {
	Dial(ctx context.Context, url string) (Conn, error)
}

// DialerFunc adapte une fonction en Dialer.
type DialerFunc func(ctx context.Context, url string) (Conn, error)

// Dial implémente Dialer.
func (f DialerFunc) Dial(ctx context.Context, url string) (Conn, error) { return f(ctx, url) }

// readIdle : silence toléré avant de considérer la connexion morte. Le serveur
// envoie un ping de transport toutes les 30 s.
const readIdle = 70 * time.Second

// WSDialer est le Dialer de production (internal/ws, stdlib pure).
type WSDialer struct {
	// HandshakeTimeout borne l'établissement de la connexion (défaut 10 s).
	HandshakeTimeout time.Duration
}

// Dial ouvre un WebSocket texte vers le serveur.
func (d WSDialer) Dial(ctx context.Context, rawURL string) (Conn, error) {
	timeout := d.HandshakeTimeout
	if timeout <= 0 {
		timeout = 10 * time.Second
	}
	c, err := ws.DialWithConfig(ctx, rawURL, &ws.DialConfig{HandshakeTimeout: timeout})
	if err != nil {
		var he *ws.HandshakeError
		if errors.As(err, &he) {
			return nil, fmt.Errorf("connexion à %s refusée (HTTP %d): %w", rawURL, he.Status, err)
		}
		return nil, fmt.Errorf("connexion à %s impossible: %w", rawURL, err)
	}
	return newWSConn(c), nil
}

type wsConn struct {
	c     *ws.Conn
	wmu   sync.Mutex
	close sync.Once
}

func newWSConn(c *ws.Conn) *wsConn {
	_ = c.SetReadDeadline(time.Now().Add(readIdle))
	// Le pong de réponse est émis par internal/ws ; on ne fait que repousser
	// l'échéance de lecture, un serveur qui ne fait que pinguer garde donc la
	// connexion vivante. AutoWriteTimeout borne l'écriture de ce pong.
	c.AutoWriteTimeout = 5 * time.Second
	c.OnPing = func([]byte) {
		_ = c.SetReadDeadline(time.Now().Add(readIdle))
	}
	return &wsConn{c: c}
}

func (w *wsConn) ReadMessage() ([]byte, error) {
	for {
		typ, data, err := w.c.ReadMessage()
		if err != nil {
			return nil, err
		}
		_ = w.c.SetReadDeadline(time.Now().Add(readIdle))
		if typ != ws.TextMessage {
			continue
		}
		return data, nil
	}
}

func (w *wsConn) WriteMessage(data []byte) error {
	w.wmu.Lock()
	defer w.wmu.Unlock()
	if err := w.c.SetWriteDeadline(time.Now().Add(10 * time.Second)); err != nil {
		return err
	}
	return w.c.WriteMessage(ws.TextMessage, data)
}

func (w *wsConn) Close() error {
	var err error
	w.close.Do(func() {
		w.wmu.Lock()
		_ = w.c.SetWriteDeadline(time.Now().Add(time.Second))
		_ = w.c.WriteClose(ws.CloseNormalClosure, "")
		w.wmu.Unlock()
		// Fermeture immédiate, sans attendre l'écho : le moteur de reconnexion
		// ne doit jamais être bloqué par un pair muet.
		err = w.c.CloseNow()
	})
	return err
}
