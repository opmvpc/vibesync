package client

import (
	"context"
	"fmt"
	"net/http"
	"sync"
	"time"

	"github.com/gorilla/websocket"
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

// WSDialer est le Dialer de production (gorilla/websocket).
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
	dialer := &websocket.Dialer{HandshakeTimeout: timeout}
	c, resp, err := dialer.DialContext(ctx, rawURL, http.Header{})
	if err != nil {
		if resp != nil {
			_ = resp.Body.Close()
			return nil, fmt.Errorf("connexion à %s refusée (HTTP %d): %w", rawURL, resp.StatusCode, err)
		}
		return nil, fmt.Errorf("connexion à %s impossible: %w", rawURL, err)
	}
	if resp != nil {
		_ = resp.Body.Close()
	}
	return newWSConn(c), nil
}

type wsConn struct {
	c     *websocket.Conn
	wmu   sync.Mutex
	close sync.Once
}

func newWSConn(c *websocket.Conn) *wsConn {
	// Le serveur envoie un ping de transport toutes les 30 s ; on tolère 60 s
	// de silence avant de considérer la connexion morte.
	_ = c.SetReadDeadline(time.Now().Add(70 * time.Second))
	c.SetPingHandler(func(appData string) error {
		_ = c.SetReadDeadline(time.Now().Add(70 * time.Second))
		w := &wsConn{c: c}
		_ = w.writeControl(websocket.PongMessage, []byte(appData))
		return nil
	})
	return &wsConn{c: c}
}

func (w *wsConn) writeControl(messageType int, data []byte) error {
	w.wmu.Lock()
	defer w.wmu.Unlock()
	return w.c.WriteControl(messageType, data, time.Now().Add(5*time.Second))
}

func (w *wsConn) ReadMessage() ([]byte, error) {
	for {
		typ, data, err := w.c.ReadMessage()
		if err != nil {
			return nil, err
		}
		_ = w.c.SetReadDeadline(time.Now().Add(70 * time.Second))
		if typ != websocket.TextMessage {
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
	return w.c.WriteMessage(websocket.TextMessage, data)
}

func (w *wsConn) Close() error {
	var err error
	w.close.Do(func() {
		w.wmu.Lock()
		_ = w.c.WriteControl(websocket.CloseMessage,
			websocket.FormatCloseMessage(websocket.CloseNormalClosure, ""),
			time.Now().Add(time.Second))
		w.wmu.Unlock()
		err = w.c.Close()
	})
	return err
}
