package ws

import (
	"bytes"
	"context"
	"crypto/tls"
	"crypto/x509"
	"errors"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"sync"
	"testing"
	"time"
)

// newPair monte un vrai serveur HTTP (httptest), y branche [Upgrade] et s'y
// connecte avec [Dial]. Les deux extrémités sont rendues à l'appelant, qui les
// pilote pas à pas : aucune goroutine cachée dans le harnais.
func newPair(t *testing.T, secure bool) (client, server *Conn) {
	t.Helper()
	connCh := make(chan *Conn, 1)
	errCh := make(chan error, 1)
	h := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		c, err := Upgrade(w, r)
		if err != nil {
			errCh <- err
			return
		}
		connCh <- c
	})

	cfg := &DialConfig{HandshakeTimeout: 5 * time.Second}
	var srv *httptest.Server
	if secure {
		srv = httptest.NewTLSServer(h)
		pool := x509.NewCertPool()
		pool.AddCert(srv.Certificate())
		cfg.TLSConfig = &tls.Config{RootCAs: pool}
	} else {
		srv = httptest.NewServer(h)
	}
	t.Cleanup(srv.Close)

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	cli, err := DialWithConfig(ctx, wsURL(srv.URL)+"/ws?x=1", cfg)
	if err != nil {
		t.Fatalf("Dial: %v", err)
	}
	select {
	case sc := <-connCh:
		t.Cleanup(func() {
			_ = cli.CloseNow()
			_ = sc.CloseNow()
		})
		return cli, sc
	case err := <-errCh:
		t.Fatalf("Upgrade: %v", err)
	case <-time.After(10 * time.Second):
		t.Fatal("timeout à l'upgrade")
	}
	return nil, nil
}

func wsURL(httpURL string) string { return "ws" + strings.TrimPrefix(httpURL, "http") }

func TestInteropEcho(t *testing.T) {
	for _, secure := range []bool{false, true} {
		name := "ws"
		if secure {
			name = "wss"
		}
		t.Run(name, func(t *testing.T) {
			cli, srv := newPair(t, secure)

			if err := cli.WriteMessage(TextMessage, []byte("bonjour ✓")); err != nil {
				t.Fatalf("client WriteMessage: %v", err)
			}
			mt, data, err := srv.ReadMessage()
			if err != nil || mt != TextMessage || string(data) != "bonjour ✓" {
				t.Fatalf("serveur a lu (%d, %q, %v)", mt, data, err)
			}
			if err := srv.WriteMessage(BinaryMessage, []byte{1, 2, 3}); err != nil {
				t.Fatalf("serveur WriteMessage: %v", err)
			}
			mt, data, err = cli.ReadMessage()
			if err != nil || mt != BinaryMessage || !bytes.Equal(data, []byte{1, 2, 3}) {
				t.Fatalf("client a lu (%d, %v, %v)", mt, data, err)
			}
		})
	}
}

func TestInteropLargeMessage(t *testing.T) {
	cli, srv := newPair(t, false)
	payload := make([]byte, 300*1024)
	for i := range payload {
		payload[i] = byte(i)
	}
	werr := make(chan error, 1)
	go func() { werr <- cli.WriteMessage(BinaryMessage, payload) }()

	_, got, err := srv.ReadMessage()
	if err != nil {
		t.Fatalf("ReadMessage: %v", err)
	}
	if !bytes.Equal(got, payload) {
		t.Fatalf("charge utile différente (%d octets reçus)", len(got))
	}
	if err := <-werr; err != nil {
		t.Fatalf("WriteMessage: %v", err)
	}
}

func TestInteropFragmented(t *testing.T) {
	cli, srv := newPair(t, false)
	if err := srv.WriteFragments(TextMessage, []byte("un "), []byte("deux "), []byte("trois")); err != nil {
		t.Fatalf("WriteFragments: %v", err)
	}
	mt, data, err := cli.ReadMessage()
	if err != nil || mt != TextMessage || string(data) != "un deux trois" {
		t.Fatalf("lu (%d, %q, %v)", mt, data, err)
	}
}

func TestInteropPingPong(t *testing.T) {
	cli, srv := newPair(t, false)

	// Le serveur lit un message : le ping qui le précède doit recevoir un pong
	// automatique.
	done := make(chan error, 1)
	go func() {
		mt, data, err := srv.ReadMessage()
		if err == nil {
			err = srv.WriteMessage(mt, data)
		}
		done <- err
	}()

	if err := cli.WritePing([]byte("bip")); err != nil {
		t.Fatalf("WritePing: %v", err)
	}
	if err := cli.WriteMessage(TextMessage, []byte("écho")); err != nil {
		t.Fatalf("WriteMessage: %v", err)
	}
	if err := <-done; err != nil {
		t.Fatalf("serveur: %v", err)
	}

	var pong []byte
	cli.OnPong = func(p []byte) { pong = append([]byte(nil), p...) }
	_, data, err := cli.ReadMessage()
	if err != nil {
		t.Fatalf("client ReadMessage: %v", err)
	}
	if string(data) != "écho" {
		t.Fatalf("écho = %q", data)
	}
	if string(pong) != "bip" {
		t.Fatalf("pong = %q, attendu \"bip\"", pong)
	}

	// Sens inverse : pong non sollicité du client vers le serveur.
	got := make(chan string, 1)
	srv.OnPong = func(p []byte) { got <- string(p) }
	go func() { _, _, _ = srv.ReadMessage() }()
	if err := cli.WritePong([]byte("bop")); err != nil {
		t.Fatalf("WritePong: %v", err)
	}
	select {
	case p := <-got:
		if p != "bop" {
			t.Fatalf("OnPong = %q", p)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("OnPong jamais appelé")
	}
}

func TestInteropCloseHandshake(t *testing.T) {
	cli, srv := newPair(t, false)

	srvErr := make(chan error, 1)
	go func() {
		for {
			if _, _, err := srv.ReadMessage(); err != nil {
				srvErr <- err
				_ = srv.Close(CloseNormalClosure, "")
				return
			}
		}
	}()

	if err := cli.Close(CloseNormalClosure, "à plus"); err != nil {
		t.Fatalf("Close: %v", err)
	}
	select {
	case err := <-srvErr:
		var ce *CloseError
		if !errors.As(err, &ce) {
			t.Fatalf("serveur: %v", err)
		}
		if ce.Code != CloseNormalClosure || ce.Reason != "à plus" {
			t.Fatalf("close reçue = %d %q", ce.Code, ce.Reason)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("le serveur n'a pas vu la fermeture")
	}
	// Après Close, la connexion est inutilisable.
	if _, _, err := cli.ReadMessage(); err == nil {
		t.Fatal("lecture possible après Close")
	}
	if err := cli.WriteMessage(TextMessage, []byte("x")); !errors.Is(err, ErrClosed) {
		t.Fatalf("écriture après Close = %v", err)
	}
}

func TestInteropCloseInitiatedByPeer(t *testing.T) {
	cli, srv := newPair(t, false)
	if err := srv.WriteClose(ClosePolicyViolation, "règle"); err != nil {
		t.Fatalf("WriteClose: %v", err)
	}
	_, _, err := cli.ReadMessage()
	if !IsCloseCode(err, ClosePolicyViolation) {
		t.Fatalf("erreur = %v", err)
	}
	// L'écho a été envoyé automatiquement : le serveur le lit.
	if _, _, err := srv.ReadMessage(); !IsCloseCode(err, ClosePolicyViolation) {
		t.Fatalf("écho reçu par le serveur = %v", err)
	}
	// Handshake de fermeture complet des deux côtés : les sockets ne doivent
	// plus être ouvertes, sans attendre un appel à Close.
	for name, c := range map[string]*Conn{"client": cli, "serveur": srv} {
		if _, err := c.NetConn().Read(make([]byte, 1)); !errors.Is(err, net.ErrClosed) {
			t.Fatalf("socket %s encore ouverte après le close handshake (err = %v)", name, err)
		}
	}
	// Close après une fermeture reçue ne bloque pas et ne remonte pas
	// d'erreur « use of closed network connection ».
	if err := cli.Close(CloseNormalClosure, ""); err != nil {
		t.Fatalf("Close: %v", err)
	}
	if err := cli.CloseNow(); err != nil {
		t.Fatalf("CloseNow: %v", err)
	}
}

func TestInteropReadLimit(t *testing.T) {
	cli, srv := newPair(t, false)
	srv.SetReadLimit(16)
	if err := cli.WriteMessage(BinaryMessage, make([]byte, 64)); err != nil {
		t.Fatalf("WriteMessage: %v", err)
	}
	_, _, err := srv.ReadMessage()
	var pe *ProtocolError
	if !errors.As(err, &pe) || pe.Code != CloseMessageTooBig {
		t.Fatalf("erreur serveur = %v", err)
	}
	// Le serveur a envoyé la close 1009 avant de rendre l'erreur.
	if _, _, err := cli.ReadMessage(); !IsCloseCode(err, CloseMessageTooBig) {
		t.Fatalf("close reçue par le client = %v", err)
	}
}

func TestInteropReadDeadline(t *testing.T) {
	cli, _ := newPair(t, false)
	if err := cli.SetReadDeadline(time.Now().Add(50 * time.Millisecond)); err != nil {
		t.Fatalf("SetReadDeadline: %v", err)
	}
	start := time.Now()
	if _, _, err := cli.ReadMessage(); !errors.Is(err, os.ErrDeadlineExceeded) {
		t.Fatalf("erreur = %v, attendu ErrDeadlineExceeded", err)
	}
	if d := time.Since(start); d > 5*time.Second {
		t.Fatalf("échéance non respectée (%v)", d)
	}
}

func TestInteropWriteDeadline(t *testing.T) {
	cli, _ := newPair(t, false)
	if err := cli.SetWriteDeadline(time.Now().Add(-time.Second)); err != nil {
		t.Fatalf("SetWriteDeadline: %v", err)
	}
	if err := cli.WriteMessage(TextMessage, []byte("x")); err == nil {
		t.Fatal("écriture réussie malgré une échéance dépassée")
	}
	// L'erreur d'écriture est collante.
	_ = cli.SetWriteDeadline(time.Time{})
	if err := cli.WriteMessage(TextMessage, []byte("y")); err == nil {
		t.Fatal("écriture réussie après une erreur d'écriture")
	}
}

func TestInteropConcurrentWrites(t *testing.T) {
	cli, srv := newPair(t, false)
	const writers, perWriter = 8, 25

	var wg sync.WaitGroup
	for i := 0; i < writers; i++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			payload := bytes.Repeat([]byte{byte('a' + id)}, 100+id)
			for j := 0; j < perWriter; j++ {
				if err := cli.WriteMessage(BinaryMessage, payload); err != nil {
					t.Errorf("WriteMessage: %v", err)
					return
				}
			}
		}(i)
	}

	counts := map[byte]int{}
	for i := 0; i < writers*perWriter; i++ {
		_, data, err := srv.ReadMessage()
		if err != nil {
			t.Fatalf("ReadMessage %d: %v", i, err)
		}
		if len(data) == 0 {
			t.Fatal("message vide")
		}
		// Chaque message doit être homogène : preuve que les trames ne se
		// sont pas entrelacées.
		for _, b := range data {
			if b != data[0] {
				t.Fatal("trames entrelacées")
			}
		}
		if want := 100 + int(data[0]-'a'); len(data) != want {
			t.Fatalf("longueur %d, attendu %d", len(data), want)
		}
		counts[data[0]]++
	}
	wg.Wait()
	for i := 0; i < writers; i++ {
		if got := counts[byte('a'+i)]; got != perWriter {
			t.Fatalf("écrivain %d: %d messages", i, got)
		}
	}
}

func TestDialErrors(t *testing.T) {
	ctx := context.Background()
	if _, err := Dial(ctx, "http://exemple.invalid/"); err == nil {
		t.Fatal("schéma http accepté")
	}
	if _, err := Dial(ctx, "ws:///chemin"); err == nil {
		t.Fatal("hôte vide accepté")
	}
	if _, err := Dial(ctx, "ws://%zz/"); err == nil {
		t.Fatal("URL invalide acceptée")
	}
	h := http.Header{}
	h.Set("Sec-WebSocket-Key", "x")
	if _, err := DialWithConfig(ctx, "ws://127.0.0.1:1/", &DialConfig{Header: h}); err == nil {
		t.Fatal("en-tête réservée acceptée")
	}
	// Même refus quelle que soit la casse des clés de la map.
	for _, name := range []string{"sec-websocket-key", "CONNECTION", "uPgRaDe", "host"} {
		raw := http.Header{name: []string{"x"}}
		if _, err := DialWithConfig(ctx, "ws://127.0.0.1:1/", &DialConfig{Header: raw}); err == nil {
			t.Fatalf("en-tête réservée %q acceptée", name)
		}
	}

	// Serveur HTTP ordinaire : pas de 101.
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		http.Error(w, "non", http.StatusNotFound)
	}))
	defer srv.Close()
	_, err := Dial(ctx, wsURL(srv.URL))
	var he *HandshakeError
	if !errors.As(err, &he) || he.Status != http.StatusNotFound {
		t.Fatalf("erreur = %v, attendu HandshakeError 404", err)
	}

	// Mauvaise Sec-WebSocket-Accept.
	bad := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		hj, _ := w.(http.Hijacker)
		nc, _, _ := hj.Hijack()
		_, _ = nc.Write([]byte("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: mauvais\r\n\r\n"))
		_ = nc.Close()
	}))
	defer bad.Close()
	if _, err := Dial(ctx, wsURL(bad.URL)); !errors.As(err, &he) || !strings.Contains(err.Error(), "Accept") {
		t.Fatalf("erreur = %v, attendu un refus sur Sec-WebSocket-Accept", err)
	}
}

func TestDialContextCanceled(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := Dial(ctx, "ws://127.0.0.1:1/"); err == nil {
		t.Fatal("dial réussi avec un contexte annulé")
	}
}

func TestDialCustomHeader(t *testing.T) {
	seen := make(chan string, 1)
	srvConn := make(chan *Conn, 1)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		seen <- r.Header.Get("X-Salle")
		if c, err := Upgrade(w, r); err == nil {
			srvConn <- c
		}
	}))
	defer srv.Close()
	defer func() {
		select {
		case c := <-srvConn:
			_ = c.CloseNow()
		default:
		}
	}()
	h := http.Header{}
	h.Set("X-Salle", "cinema")
	c, err := DialWithConfig(context.Background(), wsURL(srv.URL), &DialConfig{Header: h})
	if err != nil {
		t.Fatalf("Dial: %v", err)
	}
	defer c.CloseNow()
	if got := <-seen; got != "cinema" {
		t.Fatalf("en-tête reçue = %q", got)
	}
}
